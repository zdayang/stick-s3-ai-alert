#include <M5Unified.h>

struct Theme {
  uint16_t bg;
  uint16_t fg;
  uint16_t accent;
  const char* name;
};

static const Theme themes[] = {
    {TFT_BLACK, TFT_WHITE, TFT_CYAN, "CYAN"},
    {TFT_NAVY, TFT_WHITE, TFT_ORANGE, "ORANGE"},
    {TFT_DARKGREEN, TFT_WHITE, TFT_YELLOW, "YELLOW"},
    {TFT_MAROON, TFT_WHITE, TFT_GREENYELLOW, "GREEN"},
};

static constexpr int theme_count = sizeof(themes) / sizeof(themes[0]);

static int score = 0;
static int theme_index = 0;
static bool needs_redraw = true;
static uint32_t last_status_ms = 0;

void drawHeader(const Theme& theme) {
  M5.Display.fillRect(0, 0, M5.Display.width(), 28, theme.accent);
  M5.Display.setTextColor(TFT_BLACK, theme.accent);
  M5.Display.setCursor(8, 7);
  M5.Display.print("StickS3 Playground");
}

void drawStatus(const Theme& theme) {
  bool charging = M5.Power.isCharging();
  int battery = M5.Power.getBatteryLevel();
  int voltage = M5.Power.getBatteryVoltage();

  M5.Display.fillRect(0, 105, M5.Display.width(), 30, theme.bg);
  M5.Display.setTextColor(theme.fg, theme.bg);
  M5.Display.setCursor(8, 108);
  M5.Display.printf("BAT %d%%  %dmV", battery, voltage);
  M5.Display.setCursor(8, 122);
  M5.Display.printf("USB %s", charging ? "charging" : "not charging");
}

void drawScreen() {
  const Theme& theme = themes[theme_index];

  M5.Display.setRotation(1);
  M5.Display.fillScreen(theme.bg);
  M5.Display.setTextSize(1);
  M5.Display.setTextFont(&fonts::FreeMonoBold9pt7b);

  drawHeader(theme);

  M5.Display.setTextColor(theme.fg, theme.bg);
  M5.Display.setCursor(8, 42);
  M5.Display.printf("Score: %d", score);

  M5.Display.setCursor(8, 64);
  M5.Display.printf("Theme: %s", theme.name);

  M5.Display.setCursor(8, 86);
  M5.Display.print("A:+1  B:color  A+B:reset");

  drawStatus(theme);
  needs_redraw = false;
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setBrightness(120);
  drawScreen();
}

void loop() {
  M5.update();

  if (M5.BtnA.wasPressed()) {
    score += 1;
    needs_redraw = true;
  }

  if (M5.BtnB.wasPressed()) {
    theme_index = (theme_index + 1) % theme_count;
    needs_redraw = true;
  }

  if (M5.BtnA.pressedFor(700) && M5.BtnB.pressedFor(700)) {
    score = 0;
    needs_redraw = true;
  }

  uint32_t now = millis();
  if (now - last_status_ms > 2000) {
    last_status_ms = now;
    drawStatus(themes[theme_index]);
  }

  if (needs_redraw) {
    drawScreen();
  }

  delay(10);
}
