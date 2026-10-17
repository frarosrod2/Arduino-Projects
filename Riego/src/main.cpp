#include <Arduino.h>
#include <esp_sleep.h>
#include "driver/gpio.h" // gpio_hold_en / gpio_deep_sleep_hold_en
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include "secrets.h"

// ─── Configuración ────────────────────────────────────────────────────────────

#define SENSOR_PIN 0
#define RELAY_PIN 3
#define DEBUG_MODE false

// Calibración basada en mediciones reales
const int VAL_AIR_DRY = 2300;     // Tierra seca (0%)
const int VAL_WATER_WET = 1200;   // Tierra muy regada (100%)
const int DRY_THRESHOLD_PCT = 50; // Umbral de riego al 50% de humedad

const int NUM_SAMPLES = 5;
const int WATERING_TIME_MS = 5000;
const int MEASURE_INTERVAL = 300;  // segundos entre mediciones
const int FILTER_WAIT = 300;       // segundos de espera tras regar
const int BATCH_SIZE = 4;          // lecturas antes de enviar
const int WIFI_TIMEOUT_MS = 15000; // tiempo máximo esperando WiFi

const char *NTP_SERVER = "pool.ntp.org";
const unsigned long NTP_TIMEOUT_MS = 5000; // espera máxima de sincronización NTP

// Todo el firmware trabaja en UTC y así se guarda en Supabase. Convertir a hora
// local es trabajo del dashboard, no del sensor: si el firmware manda hora local
// sin offset, Postgres la interpreta con el TimeZone del servidor y la fecha
// queda corrida 1 o 2 horas sin que nadie se entere.
const time_t TIME_VALID_EPOCH = 1700000000UL; // 2023-11-14: umbral de "hora real"

// ─── Buffer en RTC memory (sobrevive deep sleep) ──────────────────────────────

struct Reading
{
  int humidity;
  bool watered;
  uint32_t offset_s; // segundos acumulados desde el primer arranque
};

RTC_DATA_ATTR Reading buffer[BATCH_SIZE];
RTC_DATA_ATTR int bufferCount = 0;
RTC_DATA_ATTR uint32_t totalSeconds = 0;
RTC_DATA_ATTR bool initialized = false;

// ─── Sensor ───────────────────────────────────────────────────────────────────

int getPercentageHumidity()
{
  for (int i = 0; i < 5; i++)
  {
    analogRead(SENSOR_PIN);
    delay(10);
  }

  long sum = 0;
  for (int i = 0; i < NUM_SAMPLES; i++)
  {
    sum += analogRead(SENSOR_PIN);
    delay(20);
  }
  int raw = sum / NUM_SAMPLES;

  // Mapeo corregido: 2300 es seco (0%) y 1200 es mojado (100%)
  int pct = map(raw, VAL_AIR_DRY, VAL_WATER_WET, 0, 100);
  pct = constrain(pct, 0, 100);

  Serial.printf("Raw: %d | Humedad: %d%%\n", raw, pct);
  return pct;
}

// ─── Sleep ────────────────────────────────────────────────────────────────────

// Deja el relé en LOW y "congela" el pin durante el deep sleep. Sin esto, al
// apagarse el dominio digital el GPIO queda flotante y el driver del relé puede
// activarse solo. gpio_hold_en() por sí solo no alcanza en deep sleep: hay que
// habilitar además el hold de todos los pads (gpio_deep_sleep_hold_en()).
// setup() libera ambos al despertar, si no, el relé no volvería a responder.
void holdRelayOff()
{
  digitalWrite(RELAY_PIN, LOW);
  gpio_hold_en((gpio_num_t)RELAY_PIN);
  gpio_deep_sleep_hold_en();
}

void sleepSeconds(int seconds)
{
  if (DEBUG_MODE)
  {
    delay((uint64_t)seconds * 1000);
    return;
  }
  Serial.flush();
  holdRelayOff();
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
  // El programa reinicia desde setup() tras despertar
}

// ─── WiFi + envío batch ───────────────────────────────────────────────────────

bool connectWifi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED)
  {
    if (millis() - start > WIFI_TIMEOUT_MS)
    {
      Serial.println("WiFi timeout — se reintentará en el próximo batch");
      WiFi.mode(WIFI_OFF);
      return false;
    }
    delay(200);
  }
  Serial.printf("WiFi conectado (%dms)\n", (int)(millis() - start));
  return true;
}

// Convierte un time_t a string ISO 8601 en UTC ("2025-04-20T14:32:00Z").
// El sufijo Z no es decorativo: sin offset, Postgres interpreta el valor con el
// TimeZone de la sesión (UTC en Supabase) y cualquier conversión previa a hora
// local desplazaría la fecha.
String toISOUTC(time_t t)
{
  struct tm *ti = gmtime(&t);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", ti);
  return String(buf);
}

void sendBatch()
{
  Serial.printf("Enviando batch de %d lecturas...\n", bufferCount);

  if (!connectWifi())
    return;

  // Sincronizar la hora por NTP. No configuramos TZ: la ISO se arma con gmtime()
  // y el resultado es UTC, independiente de la zona del sistema.
  configTime(0, 0, NTP_SERVER);

  // time() devuelve casi 0 hasta que SNTP responde, así que esperamos a que
  // cruce un umbral conocido para dar la hora por válida.
  unsigned long ntpStart = millis();
  time_t now = time(nullptr);
  while (now < TIME_VALID_EPOCH && millis() - ntpStart < NTP_TIMEOUT_MS)
  {
    delay(100);
    now = time(nullptr);
  }
  bool hasTime = (now >= TIME_VALID_EPOCH);

  if (hasTime)
  {
    Serial.printf("Hora NTP (UTC): %s\n", toISOUTC(now).c_str());
  }
  else
  {
    Serial.println("NTP no disponible — se omitirá recorded_at");
  }

  // Calcular el tiempo real de cada lectura usando offset_s relativo
  // La última lectura del buffer es la más reciente (acaba de medirse)
  uint32_t lastOffset = buffer[bufferCount - 1].offset_s;

  // Construir JSON array
  String body = "[";
  for (int i = 0; i < bufferCount; i++)
  {
    // Tiempo de esta lectura = ahora - (offset de la última - offset de esta)
    time_t readingTime = now - (lastOffset - buffer[i].offset_s);

    if (i > 0)
      body += ",";
    body += "{\"humidity\":" + String(buffer[i].humidity) + ",\"watered\":" + (buffer[i].watered ? "true" : "false");
    // recorded_at solo si hay hora válida: un string vacío NO significa "usá el
    // default", PostgREST lo rechaza con "invalid input syntax for type timestamp
    // with time zone" y se pierde el lote completo. Sin hora, omitimos la clave.
    if (hasTime)
      body += ",\"recorded_at\":\"" + toISOUTC(readingTime) + "\"";
    body += "}";
  }
  body += "]";

  HTTPClient http;
  http.begin(SUPABASE_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Prefer", "return=minimal"); // no devolver las filas insertadas
  http.setTimeout(8000);

  int code = http.POST(body);
  // OJO: HTTPClient devuelve el status HTTP como positivo (400, 401, 403...) y
  // solo los errores de red como negativos. Con "code > 0" un 401 daba por
  // exitoso el envío y borraba el buffer: pérdida de datos silenciosa.
  if (code >= 200 && code < 300)
  {
    Serial.printf("Batch enviado: %d\n", code);
    bufferCount = 0; // limpiar buffer solo si el envío fue exitoso
  }
  else if (code > 0)
  {
    Serial.printf("Error HTTP %d: %s — se reintentará\n", code, http.getString().c_str());
  }
  else
  {
    Serial.printf("Error de red: %s — se reintentará\n", http.errorToString(code).c_str());
  }

  http.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// ─── Setup + loop (deep sleep reinicia desde setup cada vez) ──────────────────

void setup()
{
  Serial.begin(115200);

  // Configuramos el relé en LOW ANTES de liberar el hold del deep sleep anterior
  // (ver holdRelayOff): liberarlo con el pin en input lo dejaría flotante un
  // instante, y con el hold activo el pin no respondería a los digitalWrite().
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  gpio_hold_dis((gpio_num_t)RELAY_PIN);
  gpio_deep_sleep_hold_dis();

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  btStop();

  if (!initialized)
  {
    Serial.println("Primera ejecución — inicializando buffer");
    bufferCount = 0;
    totalSeconds = 0;
    initialized = true;
  }
}

void loop()
{
  int percentageHumidity = getPercentageHumidity();
  Serial.printf("Lectura %d: humedad=%d\n", bufferCount + 1, percentageHumidity);
  bool watered = percentageHumidity < DRY_THRESHOLD_PCT;

  // Guardar lectura en buffer RTC
  if (bufferCount >= BATCH_SIZE)
  {
    Serial.println("Buffer lleno — se descarta la lectura más antigua");
    memmove(&buffer[0], &buffer[1], sizeof(Reading) * (BATCH_SIZE - 1));
    bufferCount = BATCH_SIZE - 1;
  }
  buffer[bufferCount++] = {percentageHumidity, watered, totalSeconds};

  if (watered)
  {
    Serial.println("Suelo seco — regando...");
    digitalWrite(RELAY_PIN, HIGH); // Activa el riego
    delay(WATERING_TIME_MS);
    digitalWrite(RELAY_PIN, LOW); // Corta el riego
    Serial.println("Riego completado. Esperando absorción...");

    // Enviar inmediatamente al regar (evento importante)
    sendBatch();

    totalSeconds += WATERING_TIME_MS / 1000 + FILTER_WAIT;
    sleepSeconds(FILTER_WAIT);
  }
  else
  {
    if (bufferCount >= BATCH_SIZE)
    {
      sendBatch();
    }
    totalSeconds += MEASURE_INTERVAL;
    sleepSeconds(MEASURE_INTERVAL);
  }
}