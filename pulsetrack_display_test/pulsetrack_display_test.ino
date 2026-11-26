/*
  PulseTrack — ЧСС с датчика Polar H7 (BLE) на дисплей ILI9341 + оценка физического состояния
  ESP32 (классический) + 3.2" LCD 240x320, 16-bit 8080 (на модуле запаяна перемычка 16Bit).

  Библиотеки (Library Manager):
    1) "GFX Library for Arduino" (Moon On Our Nation)
    2) "U8g2" (oliver) — только ради шрифтов с кириллицей
    BLE отдельно ставить не нужно: он входит в пакет платы esp32 (Espressif).
  Плата: ESP32 Dev Module. Если "Sketch too big" — Tools -> Partition Scheme -> "Huge APP (3MB No OTA/1MB SPIFFS)".

  Распиновка (LCD-модуль -> GPIO ESP32):
    RS (DC) -> 4      WR -> 15     CS -> GND     RD -> 3V3     RST -> EN
    DB0..DB3  -> 2, 13, 14, 16      DB4..DB7  -> 17, 18, 19, 21
    DB8..DB11 -> 22, 23, 25, 26     DB12..DB15 -> 27, 32, 33, 5
    VDD -> VIN (5 В), GND -> GND, BL свободен
*/

#include <U8g2lib.h>              // должен идти ДО Arduino_GFX_Library.h
#include <Arduino_GFX_Library.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <esp_system.h>

// ---------------- Настройки пользователя ----------------
const int AGE = 20;                          // <-- ваш возраст, от него зависят пороги состояния
const int HR_MAX = 220 - AGE;                // оценка максимального пульса
const int T_LIGHT  = HR_MAX * 50 / 100;      // < 50% HRmax  -> покой
const int T_ACTIVE = HR_MAX * 65 / 100;      // 50..65%      -> лёгкая активность
const int T_HIGH   = HR_MAX * 80 / 100;      // 65..80%      -> активность, >= 80% -> высокая нагрузка
const int HYST = 3;                          // гистерезис, уд/мин (чтобы состояние не "дрожало")
const uint32_t NO_DATA_MS = 10000;           // сколько ждать пакет от датчика

// ---------------- Дисплей ----------------
Arduino_DataBus *bus = new Arduino_ESP32PAR16(
    4 /* DC */, GFX_NOT_DEFINED /* CS */, 15 /* WR */, GFX_NOT_DEFINED /* RD */,
    2, 13, 14, 16, 17, 18, 19, 21,
    22, 23, 25, 26, 27, 32, 33, 5);
Arduino_GFX *gfx = new Arduino_ILI9341(bus, GFX_NOT_DEFINED /* RST на EN */, 0 /* портрет */, false);

#define FONT_TEXT u8g2_font_10x20_t_cyrillic   // 10x20 px, есть кириллица
#define FONT_BIG  u8g2_font_logisoso42_tn      // крупные цифры

const uint16_t C_BLACK  = 0x0000;
const uint16_t C_WHITE  = 0xFFFF;
const uint16_t C_RED    = 0xF800;
const uint16_t C_GREEN  = 0x07E0;
const uint16_t C_YELLOW = 0xFFE0;
const uint16_t C_ORANGE = 0xFD20;
const uint16_t C_GREY   = 0x7BEF;

// ---------------- Состояния ----------------
enum Status { S_SCAN, S_CONNECTING, S_ERROR, S_NODATA, S_NOCONTACT, S_OK };
enum Zone   { Z_NONE, Z_REST, Z_LIGHT, Z_ACTIVE, Z_HIGH };

struct ZoneInfo { const char *label; uint16_t color; };
const ZoneInfo ZONES[] = {
  { "ОЖИДАНИЕ",           C_GREY   },   // Z_NONE
  { "ПОКОЙ",              C_GREEN  },   // Z_REST
  { "ЛЕГКАЯ АКТИВНОСТЬ",  C_YELLOW },   // Z_LIGHT
  { "АКТИВНОСТЬ",         C_ORANGE },   // Z_ACTIVE
  { "ВЫСОКАЯ НАГРУЗКА",   C_RED    },   // Z_HIGH
};

// ---------------- BLE ----------------
static BLEUUID hrService("180d");            // Heart Rate Service
static BLEUUID hrChar("2a37");               // Heart Rate Measurement
static BLEAdvertisedDevice *myDevice = nullptr;
static BLEClient *client = nullptr;

volatile bool g_doConnect = false;
volatile bool g_connected = false;
volatile bool g_connecting = false;
volatile bool g_failed = false;
volatile bool g_contact = true;
volatile bool g_new = false;
volatile int g_bpm = 0;
volatile uint32_t g_lastNotify = 0;

// Разбор пакета Heart Rate Measurement (0x2A37)
static void notifyCallback(BLERemoteCharacteristic *c, uint8_t *d, size_t len, bool isNotify) {
  if (len < 2) return;
  uint8_t flags = d[0];
  int hr;
  if (flags & 0x01) {                         // пульс в 16 бит
    if (len < 3) return;
    hr = d[1] | (d[2] << 8);
  } else {                                    // пульс в 8 бит
    hr = d[1];
  }
  bool contactSupported = flags & 0x04;
  bool contactDetected  = flags & 0x02;
  g_contact = !contactSupported || contactDetected;
  g_bpm = hr;
  g_lastNotify = millis();
  g_new = true;
}

class AdvCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    bool isHr = advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(hrService);
    bool isPolar = strstr(advertisedDevice.toString().c_str(), "Polar") != nullptr;
    if ((isHr || isPolar) && !g_doConnect) {
      Serial.printf("Найден датчик: %s\n", advertisedDevice.toString().c_str());
      BLEDevice::getScan()->stop();
      if (myDevice) delete myDevice;
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      g_doConnect = true;
    }
  }
};

bool connectToSensor() {
  if (!client) client = BLEDevice::createClient();
  if (!client->connect(myDevice)) { Serial.println("connect() не удался"); return false; }
  BLERemoteService *svc = client->getService(hrService);
  if (!svc) { Serial.println("нет сервиса 180D"); client->disconnect(); return false; }
  BLERemoteCharacteristic *ch = svc->getCharacteristic(hrChar);
  if (!ch || !ch->canNotify()) { Serial.println("нет характеристики 2A37"); client->disconnect(); return false; }
  ch->registerForNotify(notifyCallback);
  Serial.println("Подключено, ждём пульс...");
  return true;
}

// ---------------- Логика состояния ----------------
Zone zoneOf(int bpm, int off) {
  if (bpm < T_LIGHT + off)  return Z_REST;
  if (bpm < T_ACTIVE + off) return Z_LIGHT;
  if (bpm < T_HIGH + off)   return Z_ACTIVE;
  return Z_HIGH;
}

Zone classify(int bpm, Zone cur) {
  if (cur == Z_NONE) return zoneOf(bpm, 0);
  Zone up = zoneOf(bpm, +HYST);               // вверх переходим, только если превысили порог на HYST
  if (up > cur) return up;
  Zone dn = zoneOf(bpm, -HYST);               // вниз — если опустились ниже порога на HYST
  if (dn < cur) return dn;
  return cur;
}

int curBpm = 0;
Zone curZone = Z_NONE;
uint32_t heartBigUntil = 0;
int sbuf[5]; int sIdx = 0, sCount = 0;       // сглаживание по 5 последним значениям

void processNewData(uint32_t now) {
  if (!g_new) return;
  g_new = false;
  int b = g_bpm;
  if (b <= 0 || !g_contact) return;
  curBpm = b;
  heartBigUntil = now + 200;
  sbuf[sIdx] = b; sIdx = (sIdx + 1) % 5; if (sCount < 5) sCount++;
  int sum = 0; for (int i = 0; i < sCount; i++) sum += sbuf[i];
  curZone = classify(sum / sCount, curZone);
  Serial.printf("BPM = %d, состояние: %s\n", b, ZONES[curZone].label);
}

Status computeStatus(uint32_t now) {
  if (!g_connected) {
    if (g_connecting) return S_CONNECTING;
    if (g_failed) return S_ERROR;
    return S_SCAN;
  }
  if (now - g_lastNotify > NO_DATA_MS) return S_NODATA;
  if (!g_contact) return S_NOCONTACT;
  return S_OK;
}

// ---------------- Отрисовка ----------------
int utf8len(const char *s) { int n = 0; while (*s) { if ((*s & 0xC0) != 0x80) n++; s++; } return n; }

void drawTextCentered(const char *s, int baseline, uint16_t color) {
  gfx->setFont(FONT_TEXT);
  gfx->setTextColor(color);
  gfx->setCursor((240 - utf8len(s) * 10) / 2, baseline);
  gfx->print(s);
}

void drawStatus(Status s) {
  const char *txt = ""; uint16_t col = C_WHITE;
  switch (s) {
    case S_SCAN:       txt = "BLE: поиск Polar...";  col = C_YELLOW; break;
    case S_CONNECTING: txt = "BLE: подключение..."; col = C_YELLOW; break;
    case S_ERROR:      txt = "BLE: ошибка, повтор"; col = C_RED;    break;
    case S_NODATA:     txt = "Нет данных";          col = C_ORANGE; break;
    case S_NOCONTACT:  txt = "Наденьте ремень";     col = C_ORANGE; break;
    case S_OK:         txt = "Polar H7: подключен"; col = C_GREEN;  break;
  }
  gfx->fillRect(0, 0, 240, 27, C_BLACK);
  drawTextCentered(txt, 21, col);
  gfx->drawFastHLine(0, 28, 240, C_GREY);
}

void drawHeart(bool big, uint16_t col) {
  gfx->fillRect(30, 40, 180, 115, C_BLACK);
  int r = big ? 24 : 20, cx = 120, cy = 75;
  gfx->fillCircle(cx - r, cy, r, col);
  gfx->fillCircle(cx + r, cy, r, col);
  gfx->fillTriangle(cx - 2 * r, cy + r / 3, cx + 2 * r, cy + r / 3, cx, cy + 3 * r, col);
}

void drawBpm(int bpm) {
  gfx->fillRect(0, 160, 240, 50, C_BLACK);
  char b[8];
  if (bpm > 0) snprintf(b, sizeof b, "%d", bpm); else strcpy(b, "--");
  gfx->setFont(FONT_BIG);
  gfx->setTextColor(C_WHITE);
  gfx->setCursor((240 - (int)strlen(b) * 26) / 2, 205);
  gfx->print(b);
  gfx->fillRect(0, 212, 240, 26, C_BLACK);
  drawTextCentered("уд/мин", 232, C_GREY);
}

void drawZone(Zone z) {
  gfx->fillRoundRect(10, 245, 220, 44, 8, ZONES[z].color);
  gfx->setFont(FONT_TEXT);
  gfx->setTextColor(C_BLACK);
  gfx->setCursor((240 - utf8len(ZONES[z].label) * 10) / 2, 274);
  gfx->print(ZONES[z].label);
}

void drawPct(int pct) {
  gfx->fillRect(0, 294, 240, 26, C_BLACK);
  if (pct > 0) {
    char t[40];
    snprintf(t, sizeof t, "%d%% от макс. (%d)", pct, HR_MAX);
    drawTextCentered(t, 312, C_GREY);
  }
}

int shownStatus = -1, shownBpm = -1, shownZone = -1, shownPct = -1;
int shownHeart = -1;                           // (big << 16) | цвет

void uiTick() {
  uint32_t now = millis();
  Status st = computeStatus(now);
  bool live = (st == S_OK);
  if (!live) { curBpm = 0; curZone = Z_NONE; sCount = 0; }

  if ((int)st != shownStatus) { drawStatus(st); shownStatus = st; }

  bool big = live && now < heartBigUntil;
  uint16_t hc = live ? C_RED : C_GREY;
  int heartKey = ((big ? 1 : 0) << 16) | hc;
  if (heartKey != shownHeart) { drawHeart(big, hc); shownHeart = heartKey; }

  if (curBpm != shownBpm) { drawBpm(curBpm); shownBpm = curBpm; }

  int pct = curBpm > 0 ? curBpm * 100 / HR_MAX : 0;
  if (pct != shownPct) { drawPct(pct); shownPct = pct; }

  if ((int)curZone != shownZone) { drawZone(curZone); shownZone = curZone; }
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== BOOT, reset reason = %d ===\n", (int)esp_reset_reason());

  gfx->begin();
  gfx->setUTF8Print(true);
  gfx->fillScreen(C_BLACK);

  BLEDevice::init("PulseTrack");
  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new AdvCb());
  scan->setInterval(100);
  scan->setWindow(99);
  scan->setActiveScan(true);

  uiTick();
}

void loop() {
  uint32_t now = millis();

  // потеря связи
  if (g_connected && client && !client->isConnected()) {
    g_connected = false;
    Serial.println("Связь с датчиком потеряна");
  }

  if (!g_connected) {
    if (g_doConnect) {
      g_doConnect = false;
      g_connecting = true; g_failed = false;
      uiTick();                                // показать "подключение" до блокирующего connect()
      bool ok = connectToSensor();
      g_connecting = false;
      if (ok) { g_connected = true; g_contact = true; g_lastNotify = millis(); }
      else    { g_failed = true; }
      uiTick();
      if (!ok) delay(1500);
    } else {
      g_failed = false;
      uiTick();                                // показать "поиск" до блокирующего скана
      BLEDevice::getScan()->start(5, false);   // блокирует на 5 с или до нахождения датчика
      BLEDevice::getScan()->clearResults();
    }
  }

  processNewData(now);
  uiTick();
  delay(20);
}
