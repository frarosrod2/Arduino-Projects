#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "esp_pm.h" // Esta es para el ahorro de energía de la CPU

const int redLed = 4;
const int greenLed = 5;

#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLECharacteristic *pCharacteristicTX;

class MyServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *pServer)
  {
    // Obtenemos la información del cliente conectado
    // En las versiones nuevas, se prefiere configurar esto mediante el stack directamente
    // o usando los tipos explícitos para evitar la ambigüedad que ves.

    esp_ble_conn_update_params_t conn_params = {0};
    memcpy(conn_params.bda, BLEDevice::getAddress().getNative(), ESP_BD_ADDR_LEN);
    conn_params.latency = 5;    // latencia
    conn_params.max_int = 0xA0; // max_interval (200ms)
    conn_params.min_int = 0x50; // min_interval (100ms)
    conn_params.timeout = 600;  // timeout (6s)

    esp_ble_gap_update_conn_params(&conn_params);
  }

  void onDisconnect(BLEServer *pServer)
  {
    BLEDevice::startAdvertising();
  }
};

class MyCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pCharacteristic)
  {
    String value = String(pCharacteristic->getValue().c_str());
    if (value.length() > 0)
    {
      char comando = tolower(value[0]);
      if (comando == 'g')
      {
        analogWrite(greenLed, 75);
        analogWrite(redLed, 0);
        pCharacteristicTX->setValue("LED Verde OK\n");
        pCharacteristicTX->notify();
      }
      else if (comando == 'r')
      {
        analogWrite(redLed, 125);
        analogWrite(greenLed, 0);
        pCharacteristicTX->setValue("LED Rojo OK\n");
        pCharacteristicTX->notify();
      }
    }
  }
};

void setup()
{
  // 1. Bajamos frecuencia de CPU
  setCpuFrequencyMhz(80);

  pinMode(greenLed, OUTPUT);
  pinMode(redLed, OUTPUT);

  // 2. Configuración de ahorro de energía (Power Management)
  esp_pm_config_esp32_t pm_config;
  pm_config.max_freq_mhz = 80;
  pm_config.min_freq_mhz = 10;
  pm_config.light_sleep_enable = true;
  esp_pm_configure(&pm_config);

  // 3. Inicializar BLE
  BLEDevice::init("ESP32_AHORRO");

  // 4. Ajuste de potencia (Usando la función de la librería BLEDevice para evitar errores)
  // ESP_PWR_LVL_N12 es el nivel más bajo. Si no conecta a 3m, sube a ESP_PWR_LVL_N6
  BLEDevice::setPower(ESP_PWR_LVL_N12);

  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  BLECharacteristic *pRX = pService->createCharacteristic(
      CHARACTERISTIC_UUID_RX,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pRX->setCallbacks(new MyCallbacks());

  pCharacteristicTX = pService->createCharacteristic(
      CHARACTERISTIC_UUID_TX,
      BLECharacteristic::PROPERTY_NOTIFY);
  pCharacteristicTX->addDescriptor(new BLE2902());

  pService->start();

  // 5. Configuración de anuncios (Advertising) lentos
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setMinInterval(0x0800); // ~1.28 seg
  pAdvertising->setMaxInterval(0x0800);
  pAdvertising->start();
  Serial.print('Listo!');
}

void loop()
{
  delay(1000); // El sistema dormirá durante este tiempo
}