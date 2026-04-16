#include <Arduino.h>

#define PIN_RELE 3
#define PIN_SENSOR 4

// Configuración
const int UMBRAL_RIEGO = 3000; // Mayor a esto = Seco
const int MUESTRAS = 10;       // Número de lecturas para promediar
const int TIEMPO_RIEGO = 2000; // 2 segundos de agua

void setup()
{
  Serial.begin(115200);
  pinMode(PIN_RELE, INPUT); // Relé apagado (flotante)
  analogReadResolution(12); // Aseguramos resolución de 4095
}

int obtenerHumedadFiltrada()
{
  long suma = 0;
  for (int i = 0; i < MUESTRAS; i++)
  {
    suma += analogRead(PIN_SENSOR);
    delay(10); // Pequeña pausa entre lecturas
  }
  Serial.print("Humedad Calculada ");
  Serial.println(suma / MUESTRAS);
  return suma / MUESTRAS;
}

void loop()
{
  int humedad = obtenerHumedadFiltrada();

  Serial.print("Humedad (Media): ");
  Serial.println(humedad);

  if (humedad > UMBRAL_RIEGO)
  {
    Serial.println(">> Límite superado. Verificando...");

    // Doble comprobación: esperamos 2 segundos y volvemos a medir
    // para estar 100% seguros de que no es un error puntual.
    delay(2000);
    if (obtenerHumedadFiltrada() > UMBRAL_RIEGO)
    {
      Serial.println(">> Confirmado: Tierra seca. Regando...");
      pinMode(PIN_RELE, OUTPUT);
      digitalWrite(PIN_RELE, LOW);
      delay(TIEMPO_RIEGO);
      pinMode(PIN_RELE, INPUT);
      Serial.println(">> Riego terminado.");

      // Espera larga para que el agua se filtre y el sensor se entere
      delay(60000);
    }
  }

  delay(10000); // Medir cada 10 segundos
}