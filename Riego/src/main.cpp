#include <Arduino.h>
#include <esp_sleep.h>

#define RELAY_PIN 3
#define SENSOR_PIN 4
#define DEBUG_MODE true // Cambiar a false en producción

const int DRY_THRESHOLD = 3000;
const int NUM_SAMPLES = 5;
const int WATERING_TIME_MS = 2000;
const int MEASURE_INTERVAL = 10; // seconds between readings
const int FILTER_WAIT = 6;       // TODO:60   // seconds after watering

int getFilteredHumidity()
{
  long sum = 0;
  for (int i = 0; i < NUM_SAMPLES; i++)
  {
    sum += analogRead(SENSOR_PIN);
    delay(20);
  }

  int average = sum / NUM_SAMPLES;
  Serial.printf("Humidity: %d\n", average);
  return average;
}
#define DEBUG_MODE true // Cambiar a false en producción

void sleepSeconds(int seconds)
{
  if (DEBUG_MODE)
  {
    delay(seconds * 1000); // En desarrollo: simple delay, USB estable
    return;
  }

  Serial.flush();
  Serial.end();
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_light_sleep_start();
  Serial.begin(115200);
}

void setup()
{
  Serial.begin(115200);
  pinMode(RELAY_PIN, INPUT);
  analogReadResolution(12);

  btStop();
}

void loop()
{
  int humidity = getFilteredHumidity();

  if (humidity > DRY_THRESHOLD)
  {
    Serial.println("Dry soil — watering...");

    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);
    delay(WATERING_TIME_MS);
    pinMode(RELAY_PIN, INPUT);

    Serial.println("Watering done. Waiting for absorption...");
    sleepSeconds(FILTER_WAIT);
  }
  else
  {
    sleepSeconds(MEASURE_INTERVAL);
  }
}