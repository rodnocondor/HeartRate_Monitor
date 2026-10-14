/*
  PulseTrack — тест дисплея и кнопок (без BLE)
  ESP32 (классический) + 3.2" ILI9341, 16-bit параллельная шина, тач не используется.

  Библиотека: LovyanGFX (Library Manager -> "LovyanGFX")
  Serial Monitor: 115200

  Пины совпадают с таблицей подключения из чата. Если меняете проводку —
  меняйте номера только в блоке LGFX ниже.
*/

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_Parallel16 _bus;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.freq_write = 20000000;   // при артефактах снизьте до 10000000
      cfg.pin_wr = 15;
      cfg.pin_rd = -1;             // RD подтянут к 3V3 на модуле/проводом
      cfg.pin_rs = 4;              // DC
      cfg.pin_d0 = 2;   cfg.pin_d1 = 13;  cfg.pin_d2 = 14;  cfg.pin_d3 = 16;
      cfg.pin_d4 = 17;  cfg.pin_d5 = 18;  cfg.pin_d6 = 19;  cfg.pin_d7 = 21;
      cfg.pin_d8 = 22;  cfg.pin_d9 = 23;  cfg.pin_d10 = 25; cfg.pin_d11 = 26;
      cfg.pin_d12 = 27; cfg.pin_d13 = 32; cfg.pin_d14 = 33; cfg.pin_d15 = 5;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = -1;             // CS посажен на GND
      cfg.pin_rst = -1;            // RST заведён на EN
      cfg.pin_busy = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.readable = false;
      cfg.invert = false;          // если цвета инвертированы — поставьте true
      cfg.rgb_order = false;       // если красный и синий поменялись — true
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};

LGFX tft;

// Кнопки на входных пинах (внешняя подтяжка 10 кОм к 3V3, кнопка замыкает на GND)
const int BTN_UP = 34;
const int BTN_DOWN = 35;
const int BTN_OK = 36;

void drawHeart(int cx, int cy, int r, uint16_t color) {
  tft.fillCircle(cx - r, cy, r, color);
  tft.fillCircle(cx + r, cy, r, color);
  tft.fillTriangle(cx - 2 * r, cy + r / 3, cx + 2 * r, cy + r / 3, cx, cy + 3 * r, color);
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN_UP, INPUT);
  pinMode(BTN_DOWN, INPUT);
  pinMode(BTN_OK, INPUT);

  tft.init();
  tft.setRotation(0);   // портрет 240x320

  // 1. Проверка цветов
  tft.fillScreen(TFT_RED);   delay(500);
  tft.fillScreen(TFT_GREEN); delay(500);
  tft.fillScreen(TFT_BLUE);  delay(500);
  tft.fillScreen(TFT_BLACK);

  // 2. Макет будущего главного экрана
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(8, 6);
  tft.print("BLE: поиск...");
  tft.drawFastHLine(0, 28, 240, TFT_DARKGREY);

  drawHeart(120, 90, 22, TFT_RED);

  tft.setTextSize(6);
  tft.setCursor(60, 170);
  tft.print("128");
  tft.setTextSize(2);
  tft.setCursor(80, 225);
  tft.print("BPM");

  Serial.println("Дисплей инициализирован. Нажимайте кнопки.");
}

void loop() {
  static int lastUp = HIGH, lastDown = HIGH, lastOk = HIGH;
  int up = digitalRead(BTN_UP), down = digitalRead(BTN_DOWN), ok = digitalRead(BTN_OK);

  if (up != lastUp && up == LOW)     Serial.println("Кнопка UP");
  if (down != lastDown && down == LOW) Serial.println("Кнопка DOWN");
  if (ok != lastOk && ok == LOW)     Serial.println("Кнопка OK");

  lastUp = up; lastDown = down; lastOk = ok;
  delay(20);
}
