#include <Arduino.h>
#include <esp_sleep.h>
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
const int DRY_THRESHOLD_PCT = 40; // Umbral de riego al 40% de humedad

const int NUM_SAMPLES = 5;
const int WATERING_TIME_MS = 3000;
const int MEASURE_INTERVAL = 300;  // segundos entre mediciones
const int FILTER_WAIT = 300;       // segundos de espera tras regar
const int BATCH_SIZE = 4;          // lecturas antes de enviar
const int WIFI_TIMEOUT_MS = 15000; // tiempo máximo esperando WiFi

const char *NTP_SERVER = "pool.ntp.org";
// Cadena POSIX para España: gestiona el cambio horario automáticamente
// CET-1CEST = invierno UTC+1, verano UTC+2
// M3.5.0 = último domingo de marzo | M10.5.0/3 = último domingo de octubre a las 3h
const char *TZ_STRING = "CET-1CEST,M3.5.0,M10.5.0/3";

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

void sleepSeconds(int seconds)
{
  if (DEBUG_MODE)
  {
    delay((uint64_t)seconds * 1000);
    return;
  }
  Serial.flush();
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

// Convierte un time_t a string ISO 8601 ("2025-04-20T14:32:00")
String toISO(time_t t)
{
  struct tm *ti = localtime(&t);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", ti);
  return String(buf);
}

void sendBatch()
{
  Serial.printf("Enviando batch de %d lecturas...\n", bufferCount);

  if (!connectWifi())
    return;

  // Sincronizar hora por NTP con zona horaria correcta (DST automático)
  setenv("TZ", TZ_STRING, 1);
  tzset();
  configTime(0, 0, NTP_SERVER);
  struct tm timeinfo;
  bool hasTime = getLocalTime(&timeinfo, 5000);
  if (hasTime)
  {
    Serial.printf("Hora NTP: %02d:%02d:%02d\n",
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  }
  else
  {
    Serial.println("NTP no disponible — se usará timestamp del servidor");
  }

  // Calcular el tiempo real de cada lectura usando offset_s relativo
  // La última lectura del buffer es la más reciente (acaba de medirse)
  time_t now = time(nullptr);
  uint32_t lastOffset = buffer[bufferCount - 1].offset_s;

  // Construir JSON array
  String body = "[";
  for (int i = 0; i < bufferCount; i++)
  {
    // Tiempo de esta lectura = ahora - (offset de la última - offset de esta)
    time_t readingTime = now - (lastOffset - buffer[i].offset_s);

    if (i > 0)
      body += ",";
    body += "{\"humidity\":" + String(buffer[i].humidity) + ",\"watered\":" + (buffer[i].watered ? "true" : "false") + ",\"recorded_at\":\"" + (hasTime ? toISO(readingTime) : "") + "\"}";
  }
  body += "]";

  HTTPClient http;
  http.begin(SUPABASE_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.setTimeout(8000);

  int code = http.POST(body);
  if (code > 0)
  {
    Serial.printf("Respuesta HTTP: %d\n", code);
    bufferCount = 0; // limpiar buffer solo si el envío fue exitoso
  }
  else
  {
    Serial.printf("Error HTTP: %s — se reintentará\n", http.errorToString(code).c_str());
  }

  http.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// ─── Setup + loop (deep sleep reinicia desde setup cada vez) ──────────────────

void setup()
{
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
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