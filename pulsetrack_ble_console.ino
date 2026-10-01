/*
  PulseTrack — минимальный пример
  ESP32 подключается к нагрудному датчику Polar H7 по BLE
  (стандартный профиль Heart Rate Service) и выводит BPM в Serial Monitor.

  Требуется библиотека: "NimBLE-Arduino" (автор h2zero) версии 2.x или выше
  Установка: Arduino IDE -> Sketch -> Include Library -> Manage Libraries -> "NimBLE-Arduino"

  Плата: ESP32 Dev Module (или ваша конкретная плата)
  Serial Monitor: 115200 baud
*/

#include <NimBLEDevice.h>

// Стандартные UUID сервиса и характеристики Heart Rate (Bluetooth SIG)
static NimBLEUUID serviceUUID((uint16_t)0x180D);   // Heart Rate Service
static NimBLEUUID charUUID((uint16_t)0x2A37);      // Heart Rate Measurement

static NimBLEAdvertisedDevice* targetDevice = nullptr;
static NimBLEClient* pClient = nullptr;
static bool doConnect = false;
static bool connected = false;

// --- Парсинг пакета Heart Rate Measurement согласно спецификации Bluetooth SIG ---
void notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  if (length < 2) return;

  uint8_t flags = pData[0];
  bool hrFormat16 = flags & 0x01;  // бит 0: 0 = BPM как UINT8, 1 = BPM как UINT16

  uint16_t bpm;
  if (hrFormat16) {
    bpm = pData[1] | (pData[2] << 8);
  } else {
    bpm = pData[1];
  }

  Serial.print("ЧСС: ");
  Serial.print(bpm);
  Serial.println(" BPM");
}

// --- Колбэки подключения к клиенту ---
class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pclient) {
    Serial.println("Подключено к датчику");
  }
  void onDisconnect(NimBLEClient* pclient, int reason) {
    connected = false;
    Serial.println("Соединение разорвано, ищем датчик снова...");
  }
};

// --- Колбэк сканирования (v2.x) ---
class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    if (advertisedDevice->haveServiceUUID() && advertisedDevice->isAdvertisingService(serviceUUID)) {
      Serial.print("Найден датчик: ");
      Serial.println(advertisedDevice->toString().c_str());

      NimBLEDevice::getScan()->stop();
      targetDevice = new NimBLEAdvertisedDevice(*advertisedDevice);
      doConnect = true;
    }
  }
};

bool connectToServer() {
  Serial.println("Подключаемся...");

  pClient = NimBLEDevice::createClient();
  pClient->setClientCallbacks(new ClientCallbacks());

  if (!pClient->connect(targetDevice)) {
    Serial.println("Не удалось подключиться");
    return false;
  }

  NimBLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    Serial.println("Сервис Heart Rate не найден на устройстве");
    pClient->disconnect();
    return false;
  }

  NimBLERemoteCharacteristic* pRemoteChar = pRemoteService->getCharacteristic(charUUID);
  if (pRemoteChar == nullptr) {
    Serial.println("Характеристика Heart Rate Measurement не найдена");
    pClient->disconnect();
    return false;
  }

  if (pRemoteChar->canNotify()) {
    pRemoteChar->subscribe(true, notifyCallback);
  }

  connected = true;
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Старт: поиск Polar H7...");

  NimBLEDevice::init("");
  NimBLEScan* pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setScanCallbacks(new ScanCallbacks()); // v2.x: setScanCallbacks
  pBLEScan->setActiveScan(true);
  pBLEScan->start(10, false);
}

void loop() {
  if (doConnect) {
    if (connectToServer()) {
      Serial.println("Готово, ждём данные с датчика...");
    }
    doConnect = false;
  }

  // Если связь разорвана — запускаем сканирование заново
  if (!connected && targetDevice != nullptr && !doConnect) {
    Serial.println("Повторный поиск датчика...");
    NimBLEDevice::getScan()->start(5, false);
    delay(3000);
  }

  delay(500);
}