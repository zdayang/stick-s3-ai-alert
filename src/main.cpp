#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include "esp_bt.h"

static constexpr int SCREEN_W = 135;
static constexpr int SCREEN_H = 240;
static constexpr int STATUS_H = 12;
static constexpr int HINT_H = 14;
static constexpr int CONTENT_Y = STATUS_H;
static constexpr int CONTENT_H = SCREEN_H - STATUS_H - HINT_H;
static constexpr int HINT_Y = SCREEN_H - HINT_H;

enum class ScreenMode { Home, Settings, Dodge, Stone, Mine, Alert };

struct Settings {
  uint8_t brightness = 40;
  uint8_t volume = 0;
  bool wifi = false;
  bool ble = false;
  uint8_t sleep_seconds = 60;
};

struct Capabilities {
  bool wifi = false;
  bool ble = false;
  bool imu = false;
  bool speaker = false;
};

Capabilities caps(bool wifi, bool ble, bool imu, bool speaker) {
  Capabilities result;
  result.wifi = wifi;
  result.ble = ble;
  result.imu = imu;
  result.speaker = speaker;
  return result;
}

struct DodgeState {
  bool running = false;
  bool game_over = false;
  uint8_t level = 0;
  int score = 0;
  float player_x = SCREEN_W / 2.0f;
  int obstacle_x = 40;
  float obstacle_y = CONTENT_Y;
  uint32_t last_frame_ms = 0;
  uint32_t last_score_ms = 0;
};

struct StepDir {
  int dx = 0;
  int dy = 0;
};

struct TiltControl {
  float base_ax = 0;
  float base_ay = 0;
  bool ready = true;
  uint32_t last_step_ms = 0;
};

struct StoneState {
  uint8_t level = 0;
  int player_x = 1;
  int player_y = 1;
  int stone_x[4] = {0};
  int stone_y[4] = {0};
  uint8_t stone_count = 0;
  bool solved = false;
  bool show_help = true;
  TiltControl tilt;
  bool has_undo = false;
  int undo_player_x = 1;
  int undo_player_y = 1;
  int undo_stone_x[4] = {0};
  int undo_stone_y[4] = {0};
};

struct MineState {
  int player_x = 4;
  int player_y = 6;
  int facing_x = 0;
  int facing_y = -1;
  int zombie_x[2] = {0};
  int zombie_y[2] = {0};
  uint8_t zombie_count = 0;
  uint8_t hp = 3;
  uint8_t day = 1;
  bool night = false;
  bool game_over = false;
  bool win = false;
  bool show_help = true;
  TiltControl tilt;
  bool use_stone = false;
  uint8_t wood = 0;
  uint8_t stone = 0;
  uint32_t phase_start_ms = 0;
  uint32_t last_zombie_ms = 0;
};

struct AlertState {
  bool ble_started = false;
  bool connected = false;
  bool muted = false;
  bool pending_draw = true;
  bool active = false;
  bool screen_on = false;
  bool has_last = false;
  bool showing_last = false;
  char source[8] = "AI";
  char session[16] = "-";
  char state[8] = "WAIT";
  char time[8] = "--:--";
  char last_source[8] = "AI";
  char last_session[16] = "-";
  char last_state[8] = "WAIT";
  char last_time[8] = "--:--";
  uint32_t last_event_ms = 0;
  uint32_t last_pulse_ms = 0;
  uint32_t last_flash_ms = 0;
  uint32_t idle_preview_until_ms = 0;
  uint32_t alert_hide_at_ms = 0;
  uint32_t speaker_off_at_ms = 0;
  uint8_t beep_count = 0;
  uint8_t flash_count = 0;
  bool flash_on = false;
};

static Preferences prefs;
static Settings settings;
static ScreenMode mode = ScreenMode::Home;
static Capabilities active_caps;
static M5Canvas game_canvas(&M5.Display);
static uint8_t home_index = 0;
static uint8_t settings_index = 0;
static uint32_t last_input_ms = 0;
static uint32_t last_status_ms = 0;
static bool dimmed = false;
static bool combo_was_down = false;
static bool pm1_ldo_enabled = true;
static DodgeState dodge;
static StoneState stone_game;
static MineState mine;
static AlertState alert;
static BLEServer* alert_server = nullptr;
static BLEAdvertising* alert_advertising = nullptr;
static volatile bool alert_rx_pending = false;
static char alert_rx_message[48] = {0};

static const char* app_names[] = {"AI Alert", "Dodge", "Stone", "MineZ", "Settings"};
static constexpr uint8_t app_count = sizeof(app_names) / sizeof(app_names[0]);
static const char* setting_names[] = {"Bright", "Volume", "WiFi", "BLE", "Sleep"};
static constexpr uint8_t setting_count = sizeof(setting_names) / sizeof(setting_names[0]);
static const char* ALERT_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* ALERT_RX_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
static constexpr uint8_t ALERT_BEEP_COUNT = 3;
static constexpr uint8_t ALERT_FLASH_TOGGLES = ALERT_BEEP_COUNT * 2;
static constexpr uint8_t PM1_ADDR = 0x6E;
static constexpr uint8_t PM1_REG_PWR_CFG = 0x06;
static constexpr uint8_t PM1_REG_GPIO_MODE = 0x10;
static constexpr uint8_t PM1_REG_GPIO_OUT = 0x11;
static constexpr uint8_t PM1_REG_GPIO_DRV = 0x13;
static constexpr uint8_t PM1_REG_GPIO_FUNC = 0x16;
static constexpr uint8_t PM1_SPK_GPIO = 3;

void stopCodexAlertBle();

void saveSettings() {
  prefs.putUChar("bright", settings.brightness);
  prefs.putUChar("volume", settings.volume);
  prefs.putBool("wifi", settings.wifi);
  prefs.putBool("ble", settings.ble);
  prefs.putUChar("sleep", settings.sleep_seconds);
}

void loadSettings() {
  prefs.begin("stickos", false);
  settings.brightness = prefs.getUChar("bright", settings.brightness);
  settings.volume = prefs.getUChar("volume", settings.volume);
  settings.wifi = prefs.getBool("wifi", settings.wifi);
  settings.ble = prefs.getBool("ble", settings.ble);
  settings.sleep_seconds = prefs.getUChar("sleep", settings.sleep_seconds);
}

void setBrightness(uint8_t value) {
  M5.Display.setBrightness(value);
  dimmed = false;
}

void pm1SetBit(uint8_t reg, uint8_t mask, bool on) {
  if (on) {
    M5.In_I2C.bitOn(PM1_ADDR, reg, mask, 100000);
  } else {
    M5.In_I2C.bitOff(PM1_ADDR, reg, mask, 100000);
  }
}

void setPeripheralLdo(bool enable) {
  if (pm1_ldo_enabled == enable) return;
  pm1SetBit(PM1_REG_PWR_CFG, 1 << 2, enable);
  pm1_ldo_enabled = enable;
  if (enable) {
    delay(10);
  }
}

void setSpeakerPower(bool enable) {
  if (enable && active_caps.speaker) return;

  if (enable) {
    setPeripheralLdo(true);
    pm1SetBit(PM1_REG_GPIO_FUNC, 1 << PM1_SPK_GPIO, false);
    pm1SetBit(PM1_REG_GPIO_MODE, 1 << PM1_SPK_GPIO, true);
    pm1SetBit(PM1_REG_GPIO_DRV, 1 << PM1_SPK_GPIO, false);
    pm1SetBit(PM1_REG_GPIO_OUT, 1 << PM1_SPK_GPIO, true);
    M5.Speaker.begin();
    M5.Speaker.setVolume(settings.volume);
  } else {
    M5.Speaker.stop();
    M5.Speaker.end();
    pm1SetBit(PM1_REG_GPIO_OUT, 1 << PM1_SPK_GPIO, false);
  }
  active_caps.speaker = enable;
}

void setImuPower(bool enable) {
  if (enable == active_caps.imu) return;
  if (enable) {
    setPeripheralLdo(true);
    M5.Imu.begin(&M5.In_I2C, M5.getBoard());
  } else {
    M5.Imu.sleep();
  }
  active_caps.imu = enable;
}

void applyIdleLdoPolicy() {
  if (!active_caps.imu && !active_caps.speaker) {
    setPeripheralLdo(false);
  }
}

void applyCapabilities(Capabilities next) {
  if (!next.wifi && active_caps.wifi) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  if (next.wifi && !active_caps.wifi) {
    WiFi.mode(WIFI_STA);
  }

  if (!next.ble && active_caps.ble) {
    stopCodexAlertBle();
    btStop();
  }

  setSpeakerPower(next.speaker);
  setImuPower(next.imu);

  active_caps.wifi = next.wifi;
  active_caps.ble = next.ble;
  applyIdleLdoPolicy();
}

void wakeDisplay() {
  last_input_ms = millis();
  if (dimmed) {
    setBrightness(settings.brightness);
  }
}

bool comboHeld(uint32_t ms) {
  return M5.BtnA.pressedFor(ms) && M5.BtnB.pressedFor(ms);
}

bool comboPressedOnce(uint32_t ms) {
  bool down = comboHeld(ms);
  bool fired = down && !combo_was_down;
  combo_was_down = down;
  return fired;
}

StepDir readTiltStep(float threshold = 0.38f) {
  StepDir dir;
  if (!M5.Imu.update()) {
    return dir;
  }

  float ax = 0, ay = 0, az = 0;
  M5.Imu.getAccel(&ax, &ay, &az);

  if (fabsf(ay) > fabsf(ax) && fabsf(ay) > threshold) {
    dir.dx = ay > 0 ? -1 : 1;
  } else if (fabsf(ax) > threshold) {
    dir.dy = ax > 0 ? 1 : -1;
  }
  return dir;
}

void captureTiltBaseline(float& base_ax, float& base_ay) {
  float ax = 0, ay = 0, az = 0;
  for (int i = 0; i < 8; ++i) {
    M5.Imu.update();
    float sx = 0, sy = 0, sz = 0;
    M5.Imu.getAccel(&sx, &sy, &sz);
    ax += sx;
    ay += sy;
    delay(4);
  }
  base_ax = ax / 8.0f;
  base_ay = ay / 8.0f;
}

void resetTiltControl(TiltControl& tilt) {
  captureTiltBaseline(tilt.base_ax, tilt.base_ay);
  tilt.ready = true;
  tilt.last_step_ms = millis();
}

StepDir readRelativeTiltStep(const TiltControl& tilt, float threshold = 0.16f, float neutral = 0.08f) {
  StepDir dir;
  if (!M5.Imu.update()) {
    return dir;
  }

  float ax = 0, ay = 0, az = 0;
  M5.Imu.getAccel(&ax, &ay, &az);
  float dx = ay - tilt.base_ay;
  float dy = ax - tilt.base_ax;

  if (fabsf(dx) < neutral && fabsf(dy) < neutral) {
    return dir;
  }

  if (fabsf(dx) > fabsf(dy) && fabsf(dx) > threshold) {
    dir.dx = dx > 0 ? -1 : 1;
  } else if (fabsf(dy) > threshold) {
    dir.dy = dy > 0 ? -1 : 1;
  }
  return dir;
}

bool nearTiltBaseline(const TiltControl& tilt, float neutral = 0.20f) {
  if (!M5.Imu.update()) {
    return false;
  }
  float ax = 0, ay = 0, az = 0;
  M5.Imu.getAccel(&ax, &ay, &az);
  return fabsf(ax - tilt.base_ax) < neutral && fabsf(ay - tilt.base_ay) < neutral;
}

StepDir readTiltGesture(TiltControl& tilt, uint32_t cooldown_ms = 120) {
  StepDir none;
  uint32_t now = millis();
  if (nearTiltBaseline(tilt)) {
    tilt.ready = true;
    return none;
  }

  StepDir dir = readRelativeTiltStep(tilt);
  if ((dir.dx || dir.dy) && tilt.ready && now - tilt.last_step_ms >= cooldown_ms) {
    tilt.ready = false;
    tilt.last_step_ms = now;
    return dir;
  }

  return none;
}

void drawStatus(const char* title) {
  M5.Display.fillRect(0, 0, SCREEN_W, STATUS_H, TFT_DARKGREY);
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);
  M5.Display.setCursor(2, 3);
  M5.Display.printf("%d%% W%c B%c", M5.Power.getBatteryLevel(), active_caps.wifi ? '+' : '-', active_caps.ble ? '+' : '-');
  M5.Display.setTextColor(TFT_CYAN, TFT_DARKGREY);
  M5.Display.setCursor(96, 3);
  M5.Display.print(title);
}

void drawHint(const char* hint) {
  M5.Display.fillRect(0, HINT_Y, SCREEN_W, HINT_H, TFT_DARKGREY);
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);
  M5.Display.setCursor(2, HINT_Y + 3);
  M5.Display.print(hint);
}

void clearContent() {
  M5.Display.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, TFT_BLACK);
}

void enterHome();

void drawHome() {
  drawStatus("HOME");
  clearContent();
  drawHint("A OK B NEXT HOLD");

  M5.Display.setFont(&fonts::FreeMonoBold9pt7b);
  M5.Display.setTextSize(1);
  for (uint8_t i = 0; i < app_count; ++i) {
    int y = CONTENT_Y + 34 + i * 34;
    bool selected = i == home_index;
    M5.Display.setTextColor(selected ? TFT_BLACK : TFT_WHITE, selected ? TFT_CYAN : TFT_BLACK);
    if (selected) {
      M5.Display.fillRoundRect(6, y - 19, SCREEN_W - 12, 26, 4, TFT_CYAN);
    }
    M5.Display.setCursor(14, y);
    M5.Display.printf("%c %s", selected ? '>' : ' ', app_names[i]);
  }
}

const char* settingValue(uint8_t index) {
  static char value[16];
  switch (index) {
    case 0: snprintf(value, sizeof(value), "%u", settings.brightness); break;
    case 1: snprintf(value, sizeof(value), "%u", settings.volume); break;
    case 2: snprintf(value, sizeof(value), "%s", settings.wifi ? "ON" : "OFF"); break;
    case 3: snprintf(value, sizeof(value), "%s", settings.ble ? "ON" : "OFF"); break;
    case 4: snprintf(value, sizeof(value), "%us", settings.sleep_seconds); break;
    default: snprintf(value, sizeof(value), "-"); break;
  }
  return value;
}

void drawSettings() {
  drawStatus("SET");
  clearContent();
  drawHint("A SET B NEXT HOLD");

  M5.Display.setFont(&fonts::Font2);
  for (uint8_t i = 0; i < setting_count; ++i) {
    int y = CONTENT_Y + 12 + i * 30;
    bool selected = i == settings_index;
    M5.Display.setTextColor(selected ? TFT_BLACK : TFT_WHITE, selected ? TFT_YELLOW : TFT_BLACK);
    if (selected) {
      M5.Display.fillRoundRect(5, y - 3, SCREEN_W - 10, 21, 3, TFT_YELLOW);
    }
    M5.Display.setCursor(10, y);
    M5.Display.printf("%s", setting_names[i]);
    M5.Display.setCursor(88, y);
    M5.Display.print(settingValue(i));
  }
}

void changeCurrentSetting() {
  switch (settings_index) {
    case 0:
      settings.brightness = settings.brightness >= 160 ? 20 : settings.brightness + 20;
      setBrightness(settings.brightness);
      break;
    case 1:
      settings.volume = settings.volume >= 100 ? 0 : settings.volume + 20;
      if (active_caps.speaker) {
        M5.Speaker.setVolume(settings.volume);
      }
      break;
    case 2:
      settings.wifi = !settings.wifi;
      break;
    case 3:
      settings.ble = !settings.ble;
      break;
    case 4:
      settings.sleep_seconds = settings.sleep_seconds >= 180 ? 30 : settings.sleep_seconds + 30;
      break;
  }
  saveSettings();
  applyCapabilities(caps(false, false, false, false));
  drawSettings();
}

void cleanAlertField(const char* raw, char* dest, size_t size) {
  size_t out = 0;
  for (size_t i = 0; raw[i] && out < size - 1; ++i) {
    char c = raw[i];
    if (c == '\n' || c == '\r' || c == '|') break;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-' || c == ':') {
      dest[out++] = c;
    }
  }
  dest[out] = '\0';
}

void parseAlertMessage(const char* raw) {
  char source[8] = "AI";
  char session[16] = "-";
  char state[8] = {0};
  char time[8] = "--:--";
  const char* first = strchr(raw, '|');
  const char* second = first ? strchr(first + 1, '|') : nullptr;
  const char* third = second ? strchr(second + 1, '|') : nullptr;

  if (first && second) {
    char part[24] = {0};
    size_t len = min<size_t>(first - raw, sizeof(part) - 1);
    strncpy(part, raw, len);
    cleanAlertField(part, source, sizeof(source));

    len = min<size_t>(second - first - 1, sizeof(part) - 1);
    memset(part, 0, sizeof(part));
    strncpy(part, first + 1, len);
    cleanAlertField(part, session, sizeof(session));

    if (third) {
      len = min<size_t>(third - second - 1, sizeof(part) - 1);
      memset(part, 0, sizeof(part));
      strncpy(part, second + 1, len);
      cleanAlertField(part, state, sizeof(state));
      cleanAlertField(third + 1, time, sizeof(time));
    } else {
      cleanAlertField(second + 1, state, sizeof(state));
    }
  } else {
    cleanAlertField(raw, state, sizeof(state));
  }

  if (state[0] == '\0') {
    strncpy(state, "PING", sizeof(state) - 1);
  }
  strncpy(alert.source, source, sizeof(alert.source) - 1);
  strncpy(alert.session, session, sizeof(alert.session) - 1);
  strncpy(alert.state, state, sizeof(alert.state) - 1);
  strncpy(alert.time, time, sizeof(alert.time) - 1);
  strncpy(alert.last_source, source, sizeof(alert.last_source) - 1);
  strncpy(alert.last_session, session, sizeof(alert.last_session) - 1);
  strncpy(alert.last_state, state, sizeof(alert.last_state) - 1);
  strncpy(alert.last_time, time, sizeof(alert.last_time) - 1);
  alert.has_last = true;
}

void setAlertMessage(const char* raw) {
  parseAlertMessage(raw);
  alert.active = true;
  alert.screen_on = true;
  alert.showing_last = false;
  alert.last_event_ms = millis();
  alert.alert_hide_at_ms = alert.last_event_ms + 7000;
  alert.last_pulse_ms = 0;
  alert.last_flash_ms = 0;
  alert.beep_count = 0;
  alert.flash_count = 0;
  alert.flash_on = false;
  alert.pending_draw = true;
  wakeDisplay();
}

void clearAlert() {
  alert.active = false;
  alert.screen_on = false;
  alert.showing_last = false;
  strncpy(alert.source, "AI", sizeof(alert.source) - 1);
  strncpy(alert.session, "-", sizeof(alert.session) - 1);
  strncpy(alert.state, "WAIT", sizeof(alert.state) - 1);
  strncpy(alert.time, "--:--", sizeof(alert.time) - 1);
  alert.beep_count = 0;
  alert.flash_count = 0;
  alert.flash_on = false;
  alert.speaker_off_at_ms = 0;
  alert.pending_draw = false;
  setSpeakerPower(false);
  applyIdleLdoPolicy();
  M5.Display.setBrightness(0);
  M5.Display.fillScreen(TFT_BLACK);
}

void drawAlertIdleHint(const char* label = "AI ALERT READY") {
  M5.Display.setBrightness(settings.brightness);
  drawStatus("AI");
  clearContent();
  drawHint("A LAST B MUTE AB");
  M5.Display.setFont(&fonts::FreeMonoBold9pt7b);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.setCursor(18, CONTENT_Y + 48);
  M5.Display.print("AI");
  M5.Display.setCursor(18, CONTENT_Y + 86);
  M5.Display.print("ALERT");

  M5.Display.setFont(&fonts::Font2);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setCursor(28, CONTENT_Y + 134);
  M5.Display.print(strcmp(label, "NO ALERT YET") == 0 ? "NO ALERT" : "READY");

  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Display.setCursor(20, CONTENT_Y + 166);
  M5.Display.print(alert.muted ? "MUTED" : "BEEP ON");
  if (alert.has_last) {
    M5.Display.setCursor(20, CONTENT_Y + 184);
    M5.Display.print("A = LAST ALERT");
  }
  alert.screen_on = true;
  alert.pending_draw = false;
}

void queueAlertMessage(const char* raw) {
  size_t len = min<size_t>(strlen(raw), sizeof(alert_rx_message) - 1);
  for (size_t i = 0; i < len; ++i) {
    alert_rx_message[i] = raw[i];
  }
  alert_rx_message[len] = '\0';
  alert_rx_pending = true;
  Serial.printf("AIAlert RX: %s\n", alert_rx_message);
}

uint16_t alertColor() {
  if (strcmp(alert.state, "ASK") == 0) return TFT_ORANGE;
  if (strcmp(alert.state, "DONE") == 0) return TFT_CYAN;
  return TFT_YELLOW;
}

void drawAlert() {
  if (!alert.active && !alert.showing_last) {
    clearAlert();
    return;
  }

  const char* source = alert.showing_last ? alert.last_source : alert.source;
  const char* session = alert.showing_last ? alert.last_session : alert.session;
  const char* state = alert.showing_last ? alert.last_state : alert.state;
  const char* when = alert.showing_last ? alert.last_time : alert.time;
  uint16_t color = strcmp(state, "ASK") == 0 ? TFT_ORANGE : strcmp(state, "DONE") == 0 ? TFT_CYAN : TFT_YELLOW;
  M5.Display.setBrightness(settings.brightness);
  M5.Display.fillScreen(TFT_BLACK);
  drawStatus("AI");
  drawHint(alert.showing_last ? "A OFF B MUTE AB" : "A CLR B MUTE AB");

  M5.Display.setFont(&fonts::Font2);
  M5.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5.Display.setCursor(6, CONTENT_Y + 13);
  M5.Display.print(alert.showing_last ? "LAST" : "NOW");
  M5.Display.setFont(&fonts::Font4);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setCursor(58, CONTENT_Y + 9);
  M5.Display.print(when);

  M5.Display.fillRoundRect(5, CONTENT_Y + 45, SCREEN_W - 10, 86, 8, color);
  M5.Display.drawRoundRect(5, CONTENT_Y + 45, SCREEN_W - 10, 86, 8, TFT_WHITE);

  M5.Display.setFont(&fonts::Font4);
  M5.Display.setTextColor(TFT_BLACK, color);
  const char* tool_label = strcmp(source, "CC") == 0 ? "CLAUDE" : strcmp(source, "CX") == 0 ? "CODEX" : source;
  int tool_x = strcmp(tool_label, "CLAUDE") == 0 ? 19 : 24;
  M5.Display.setCursor(tool_x, CONTENT_Y + 61);
  M5.Display.print(tool_label);

  M5.Display.setFont(&fonts::Font2);
  M5.Display.setCursor(10, CONTENT_Y + 106);
  M5.Display.print(session);

  M5.Display.setFont(&fonts::Font4);
  M5.Display.setTextColor(color, TFT_BLACK);
  int state_x = strcmp(state, "DONE") == 0 ? 30 : 43;
  M5.Display.setCursor(state_x, CONTENT_Y + 145);
  M5.Display.print(state);

  M5.Display.setFont(&fonts::Font2);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setCursor(18, CONTENT_Y + 184);
  M5.Display.print(alert.muted ? "MUTED" : "BEEP ON");
  alert.screen_on = true;
  alert.pending_draw = false;
}

void pulseAlert(uint32_t now) {
  if (!alert.active) return;
  if (alert.beep_count < ALERT_BEEP_COUNT && !alert.muted && settings.volume > 0 && now - alert.last_pulse_ms > 260) {
    alert.last_pulse_ms = now;
    alert.beep_count++;
    setSpeakerPower(true);
    M5.Speaker.tone(strcmp(alert.state, "ASK") == 0 ? 2093 : 1568, 90);
    alert.speaker_off_at_ms = now + 140;
  }
  if (alert.speaker_off_at_ms > 0 && now > alert.speaker_off_at_ms) {
    alert.speaker_off_at_ms = 0;
    setSpeakerPower(false);
    applyIdleLdoPolicy();
  }
  if (alert.flash_count < ALERT_FLASH_TOGGLES && now - alert.last_flash_ms > 180) {
    alert.last_flash_ms = now;
    alert.flash_count++;
    alert.flash_on = !alert.flash_on;
    M5.Display.setBrightness(alert.flash_on ? min<uint8_t>(180, settings.brightness + 90) : max<uint8_t>(10, settings.brightness / 3));
  } else if (alert.flash_count >= ALERT_FLASH_TOGGLES && alert.screen_on) {
    M5.Display.setBrightness(settings.brightness);
  }
}

class AlertServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    alert.connected = true;
  }

  void onDisconnect(BLEServer* server) override {
    alert.connected = false;
    if (alert_advertising) {
      alert_advertising->start();
    }
  }
};

class AlertRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    std::string value = characteristic->getValue();
    if (!value.empty()) {
      queueAlertMessage(value.c_str());
    }
  }

  void onWrite(BLECharacteristic* characteristic, esp_ble_gatts_cb_param_t* param) override {
    if (param && param->write.len > 0) {
      char value[48] = {0};
      size_t len = min<size_t>(param->write.len, sizeof(value) - 1);
      for (size_t i = 0; i < len; ++i) {
        value[i] = static_cast<char>(param->write.value[i]);
      }
      queueAlertMessage(value);
      return;
    }
    onWrite(characteristic);
  }
};

void startCodexAlertBle() {
  if (alert.ble_started) return;
  BLEDevice::init("StickS3-AI");
  alert_server = BLEDevice::createServer();
  alert_server->setCallbacks(new AlertServerCallbacks());
  BLEService* service = alert_server->createService(ALERT_SERVICE_UUID);
  BLECharacteristic* rx = service->createCharacteristic(ALERT_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new AlertRxCallbacks());
  service->start();
  alert_advertising = BLEDevice::getAdvertising();
  alert_advertising->addServiceUUID(ALERT_SERVICE_UUID);
  alert_advertising->setScanResponse(true);
  alert_advertising->setMinInterval(0x0640);
  alert_advertising->setMaxInterval(0x0800);
  alert_advertising->setMinPreferred(0x06);
  alert_advertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  alert.ble_started = true;
  alert.connected = false;
  alert.pending_draw = false;
}

void stopCodexAlertBle() {
  if (!alert.ble_started) return;
  BLEDevice::deinit(true);
  alert_server = nullptr;
  alert_advertising = nullptr;
  alert.ble_started = false;
  alert.connected = false;
  alert.pending_draw = false;
}

void enterAlert() {
  applyCapabilities(caps(false, true, false, false));
  startCodexAlertBle();
  alert.active = false;
  alert.showing_last = false;
  alert.idle_preview_until_ms = millis() + 5000;
  drawAlertIdleHint("AI ALERT READY");
}

void updateAlert() {
  if (alert_rx_pending) {
    alert_rx_pending = false;
    setAlertMessage(alert_rx_message);
  }

  if (M5.BtnA.wasClicked()) {
    if (alert.active || alert.showing_last) {
      clearAlert();
    } else if (alert.has_last) {
      alert.showing_last = true;
      alert.screen_on = true;
      alert.pending_draw = true;
    } else {
      alert.idle_preview_until_ms = millis() + 5000;
      drawAlertIdleHint("NO ALERT YET");
    }
    return;
  }
  if (M5.BtnB.wasClicked()) {
    alert.muted = !alert.muted;
    if (alert.active) {
      alert.pending_draw = true;
    } else {
      drawAlertIdleHint();
      alert.idle_preview_until_ms = millis() + 5000;
    }
  }

  uint32_t now = millis();
  if (alert.showing_last) {
    if (alert.pending_draw) {
      drawAlert();
    }
    return;
  }

  if (!alert.active) {
    if (alert.screen_on && alert.idle_preview_until_ms > 0 && now > alert.idle_preview_until_ms) {
      alert.idle_preview_until_ms = 0;
      clearAlert();
    }
    return;
  }

  if (alert.pending_draw) {
    drawAlert();
  }
  pulseAlert(now);
  if (alert.active && alert.alert_hide_at_ms > 0 && now > alert.alert_hide_at_ms) {
    clearAlert();
  }
}

#if 0
void setAlertMessage_old(const char* raw) {
  char cleaned[28] = {0};
  uint8_t out = 0;
  for (uint8_t i = 0; raw[i] && out < sizeof(cleaned) - 1; ++i) {
    char c = raw[i];
    if (c == '\n' || c == '\r') break;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-') {
      cleaned[out++] = c;
    }
  }
  if (out == 0) {
    strncpy(cleaned, "PING", sizeof(cleaned) - 1);
  }
  strncpy(alert.message, cleaned, sizeof(alert.message) - 1);
  alert.message[sizeof(alert.message) - 1] = '\0';
  alert.last_event_ms = millis();
  alert.pulse = 0;
  alert.pending_draw = true;
  wakeDisplay();
  if (!alert.muted && settings.volume > 0) {
    M5.Speaker.tone(1760, 80);
  }
}
#endif

void resetDodge() {
  dodge.running = false;
  dodge.game_over = false;
  dodge.score = 0;
  dodge.player_x = SCREEN_W / 2.0f;
  dodge.obstacle_x = random(16, SCREEN_W - 28);
  dodge.obstacle_y = CONTENT_Y + 2;
  dodge.last_frame_ms = millis();
  dodge.last_score_ms = millis();
}

float dodgeSpeed() {
  return 1.6f + dodge.level * 0.7f + min(dodge.score, 80) * 0.015f;
}

void drawDodgeFrame() {
  game_canvas.fillSprite(TFT_BLACK);
  game_canvas.drawRect(0, 0, SCREEN_W, CONTENT_H, TFT_DARKGREY);

  game_canvas.fillRoundRect(dodge.obstacle_x, static_cast<int>(dodge.obstacle_y) - CONTENT_Y, 18, 12, 2, TFT_RED);
  game_canvas.fillRoundRect(static_cast<int>(dodge.player_x) - 14, CONTENT_H - 17, 28, 10, 3, TFT_GREEN);

  game_canvas.setFont(&fonts::Font0);
  game_canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  game_canvas.setCursor(4, 4);
  game_canvas.printf("S:%d L:%d", dodge.score, dodge.level + 1);

  if (!dodge.running) {
    game_canvas.setFont(&fonts::Font2);
    game_canvas.setTextColor(dodge.game_over ? TFT_RED : TFT_CYAN, TFT_BLACK);
    game_canvas.setCursor(dodge.game_over ? 42 : 48, CONTENT_H / 2 - 8);
    game_canvas.print(dodge.game_over ? "CRASH" : "READY");
  }

  game_canvas.pushSprite(0, CONTENT_Y);
}

void enterDodge() {
  applyCapabilities(caps(false, false, true, false));
  resetDodge();
  drawStatus("DODGE");
  drawHint("A GO B LVL AB");
  drawDodgeFrame();
}

void updateDodge() {
  if (M5.BtnA.wasPressed()) {
    if (dodge.game_over) {
      resetDodge();
    }
    dodge.running = !dodge.running;
    drawHint(dodge.running ? "A PAU B LVL AB" : "A GO B LVL AB");
    drawDodgeFrame();
  }

  if (M5.BtnB.wasPressed() && !dodge.running) {
    dodge.level = (dodge.level + 1) % 3;
    resetDodge();
    drawDodgeFrame();
  }

  if (!dodge.running) {
    return;
  }

  uint32_t now = millis();
  if (now - dodge.last_frame_ms < 33) {
    return;
  }
  dodge.last_frame_ms = now;

  if (M5.Imu.update()) {
    float ax = 0, ay = 0, az = 0;
    M5.Imu.getAccel(&ax, &ay, &az);
    dodge.player_x -= ay * 8.0f;
    if (dodge.player_x < 16) dodge.player_x = 16;
    if (dodge.player_x > SCREEN_W - 16) dodge.player_x = SCREEN_W - 16;
  }

  dodge.obstacle_y += dodgeSpeed();
  if (now - dodge.last_score_ms > 500) {
    dodge.score += 1;
    dodge.last_score_ms = now;
  }

  int player_left = static_cast<int>(dodge.player_x) - 14;
  int player_right = static_cast<int>(dodge.player_x) + 14;
  int player_top = CONTENT_Y + CONTENT_H - 17;
  int obstacle_left = dodge.obstacle_x;
  int obstacle_right = dodge.obstacle_x + 18;
  int obstacle_bottom = static_cast<int>(dodge.obstacle_y) + 12;
  bool overlap_x = obstacle_right > player_left && obstacle_left < player_right;
  bool overlap_y = obstacle_bottom > player_top;

  if (overlap_x && overlap_y) {
    dodge.running = false;
    dodge.game_over = true;
    drawHint("A TRY  B LVL  AB");
    drawDodgeFrame();
    return;
  }

  if (dodge.obstacle_y > HINT_Y - 6) {
    dodge.obstacle_y = CONTENT_Y + 2;
    dodge.obstacle_x = random(16, SCREEN_W - 34);
  }

  drawStatus("DODGE");
  drawDodgeFrame();
}

static constexpr int STONE_W = 5;
static constexpr int STONE_H = 7;
static constexpr int STONE_TILE = 24;
static constexpr int STONE_X0 = (SCREEN_W - STONE_W * STONE_TILE) / 2;
static constexpr int STONE_Y0 = 46;
static constexpr uint8_t STONE_LEVELS = 3;

static const char* stone_levels[STONE_LEVELS][STONE_H] = {
    {
        "#####",
        "#PO.#",
        "#   #",
        "# # #",
        "#   #",
        "#   #",
        "#####",
    },
    {
        "#####",
        "#P  #",
        "#OO #",
        "# # #",
        "#.. #",
        "#   #",
        "#####",
    },
    {
        "#####",
        "#PO #",
        "# # #",
        "# O.#",
        "# # #",
        "#  .#",
        "#####",
    },
};

bool stoneWall(int x, int y) {
  if (x < 0 || y < 0 || x >= STONE_W || y >= STONE_H) return true;
  return stone_levels[stone_game.level][y][x] == '#';
}

bool stoneTarget(int x, int y) {
  if (x < 0 || y < 0 || x >= STONE_W || y >= STONE_H) return false;
  return stone_levels[stone_game.level][y][x] == '.';
}

int stoneAt(int x, int y) {
  for (uint8_t i = 0; i < stone_game.stone_count; ++i) {
    if (stone_game.stone_x[i] == x && stone_game.stone_y[i] == y) return i;
  }
  return -1;
}

void saveStoneUndo() {
  stone_game.has_undo = true;
  stone_game.undo_player_x = stone_game.player_x;
  stone_game.undo_player_y = stone_game.player_y;
  for (uint8_t i = 0; i < stone_game.stone_count; ++i) {
    stone_game.undo_stone_x[i] = stone_game.stone_x[i];
    stone_game.undo_stone_y[i] = stone_game.stone_y[i];
  }
}

void undoStone() {
  if (!stone_game.has_undo) return;
  stone_game.player_x = stone_game.undo_player_x;
  stone_game.player_y = stone_game.undo_player_y;
  for (uint8_t i = 0; i < stone_game.stone_count; ++i) {
    stone_game.stone_x[i] = stone_game.undo_stone_x[i];
    stone_game.stone_y[i] = stone_game.undo_stone_y[i];
  }
  stone_game.solved = false;
}

void loadStoneLevel(uint8_t level) {
  stone_game.level = level % STONE_LEVELS;
  stone_game.stone_count = 0;
  stone_game.solved = false;
  stone_game.show_help = true;
  stone_game.tilt.ready = true;
  stone_game.has_undo = false;
  stone_game.tilt.last_step_ms = millis();

  for (int y = 0; y < STONE_H; ++y) {
    for (int x = 0; x < STONE_W; ++x) {
      char tile = stone_levels[stone_game.level][y][x];
      if (tile == 'P') {
        stone_game.player_x = x;
        stone_game.player_y = y;
      } else if (tile == 'O' && stone_game.stone_count < 4) {
        stone_game.stone_x[stone_game.stone_count] = x;
        stone_game.stone_y[stone_game.stone_count] = y;
        stone_game.stone_count++;
      }
    }
  }
}

void drawStoneGame() {
  game_canvas.fillSprite(TFT_BLACK);
  if (stone_game.show_help) {
    game_canvas.setFont(&fonts::Font2);
    game_canvas.setTextColor(TFT_CYAN, TFT_BLACK);
    game_canvas.setCursor(19, 16);
    game_canvas.print("Stone Push");

    game_canvas.setFont(&fonts::Font0);
    game_canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    game_canvas.setCursor(9, 52);
    game_canvas.print("Tilt = move");
    game_canvas.setCursor(9, 70);
    game_canvas.print("Push rocks into");
    game_canvas.setCursor(9, 88);
    game_canvas.print("yellow holes");
    game_canvas.setCursor(9, 106);
    game_canvas.print("All gone = clear");
    game_canvas.setCursor(9, 126);
    game_canvas.print("A undo  B reset");

    game_canvas.fillRoundRect(18, 146, 15, 15, 3, TFT_GREEN);
    game_canvas.fillCircle(52, 153, 8, TFT_LIGHTGREY);
    game_canvas.fillCircle(86, 153, 8, TFT_YELLOW);
    game_canvas.setCursor(13, 172);
    game_canvas.print("you  rock hole");
    game_canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
    game_canvas.setCursor(15, 190);
    game_canvas.print("Hold still");
    game_canvas.setCursor(30, 204);
    game_canvas.print("A START");
    game_canvas.pushSprite(0, CONTENT_Y);
    return;
  }

  game_canvas.setFont(&fonts::Font0);
  game_canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  game_canvas.setCursor(4, 4);
  game_canvas.printf("L:%d  rocks:%d", stone_game.level + 1, stone_game.stone_count);

  for (int y = 0; y < STONE_H; ++y) {
    for (int x = 0; x < STONE_W; ++x) {
      int px = STONE_X0 + x * STONE_TILE;
      int py = STONE_Y0 - CONTENT_Y + y * STONE_TILE;
      uint16_t color = TFT_BLACK;
      if (stoneWall(x, y)) color = TFT_DARKGREY;
      else color = TFT_NAVY;
      game_canvas.fillRect(px, py, STONE_TILE - 1, STONE_TILE - 1, color);
      game_canvas.drawRect(px, py, STONE_TILE - 1, STONE_TILE - 1, TFT_BLACK);
      if (stoneTarget(x, y)) {
        game_canvas.fillCircle(px + STONE_TILE / 2, py + STONE_TILE / 2, 9, TFT_YELLOW);
        game_canvas.fillCircle(px + STONE_TILE / 2, py + STONE_TILE / 2, 4, TFT_BLACK);
      }
    }
  }

  for (uint8_t i = 0; i < stone_game.stone_count; ++i) {
    int px = STONE_X0 + stone_game.stone_x[i] * STONE_TILE;
    int py = STONE_Y0 - CONTENT_Y + stone_game.stone_y[i] * STONE_TILE;
    game_canvas.fillCircle(px + STONE_TILE / 2, py + STONE_TILE / 2, 10, TFT_LIGHTGREY);
    game_canvas.drawCircle(px + STONE_TILE / 2, py + STONE_TILE / 2, 10, TFT_DARKGREY);
    game_canvas.drawLine(px + 8, py + 9, px + 16, py + 15, TFT_DARKGREY);
    game_canvas.drawLine(px + 7, py + 16, px + 14, py + 7, TFT_DARKGREY);
  }

  int player_px = STONE_X0 + stone_game.player_x * STONE_TILE;
  int player_py = STONE_Y0 - CONTENT_Y + stone_game.player_y * STONE_TILE;
  game_canvas.fillRoundRect(player_px + 3, player_py + 3, STONE_TILE - 7, STONE_TILE - 7, 6, TFT_GREEN);
  game_canvas.fillCircle(player_px + 9, player_py + 10, 2, TFT_BLACK);
  game_canvas.fillCircle(player_px + 15, player_py + 10, 2, TFT_BLACK);
  game_canvas.drawLine(player_px + 8, player_py + 16, player_px + 16, player_py + 16, TFT_BLACK);

  if (stone_game.solved) {
    game_canvas.setFont(&fonts::Font2);
    game_canvas.setTextColor(TFT_CYAN, TFT_BLACK);
    game_canvas.setCursor(34, 190);
    game_canvas.print("CLEAR!");
    game_canvas.setFont(&fonts::Font0);
    game_canvas.setCursor(18, 210);
    game_canvas.print("B dbl = next");
  }
  game_canvas.pushSprite(0, CONTENT_Y);
}

void enterStone() {
  applyCapabilities(caps(false, false, true, false));
  loadStoneLevel(stone_game.level);
  drawStatus("STONE");
  drawHint("A UND B RST AB");
  drawStoneGame();
}

void tryMoveStone(int dx, int dy) {
  if (stone_game.solved || (dx == 0 && dy == 0)) return;

  int nx = stone_game.player_x + dx;
  int ny = stone_game.player_y + dy;
  if (stoneWall(nx, ny)) return;

  int stone_index = stoneAt(nx, ny);
  saveStoneUndo();

  if (stone_index >= 0) {
    int sx = nx + dx;
    int sy = ny + dy;
    if (stoneWall(sx, sy) || stoneAt(sx, sy) >= 0) {
      stone_game.has_undo = false;
      return;
    }
    if (stoneTarget(sx, sy)) {
      for (uint8_t i = stone_index; i + 1 < stone_game.stone_count; ++i) {
        stone_game.stone_x[i] = stone_game.stone_x[i + 1];
        stone_game.stone_y[i] = stone_game.stone_y[i + 1];
      }
      stone_game.stone_count--;
      if (stone_game.stone_count == 0) stone_game.solved = true;
    } else {
      stone_game.stone_x[stone_index] = sx;
      stone_game.stone_y[stone_index] = sy;
    }
  }

  stone_game.player_x = nx;
  stone_game.player_y = ny;
}

void updateStone() {
  if (M5.BtnA.wasClicked()) {
    if (stone_game.show_help) {
      resetTiltControl(stone_game.tilt);
      stone_game.show_help = false;
    } else {
      undoStone();
    }
    drawStoneGame();
  }
  if (M5.BtnB.wasDoubleClicked()) {
    loadStoneLevel(stone_game.level + 1);
    stone_game.show_help = false;
    resetTiltControl(stone_game.tilt);
    drawStoneGame();
    return;
  }
  if (M5.BtnB.wasClicked()) {
    loadStoneLevel(stone_game.level);
    stone_game.show_help = false;
    resetTiltControl(stone_game.tilt);
    drawStoneGame();
    return;
  }

  if (stone_game.show_help) return;

  StepDir dir = readTiltGesture(stone_game.tilt, 120);
  if (dir.dx || dir.dy) {
    tryMoveStone(dir.dx, dir.dy);
    drawStoneGame();
  }
}

static constexpr int MINE_W = 5;
static constexpr int MINE_H = 8;
static constexpr int MINE_TILE = 24;
static constexpr int MINE_X0 = (SCREEN_W - MINE_W * MINE_TILE) / 2;
static constexpr int MINE_Y0 = 42;
static constexpr uint32_t MINE_DAY_MS = 30000;
static constexpr uint32_t MINE_NIGHT_MS = 25000;

static const char mine_seed[MINE_H][MINE_W + 1] = {
    "GTGRG",
    "GGGTG",
    "GRGGG",
    "GGGGG",
    "GGTGG",
    "GGGRG",
    "GGGGT",
    "GRGGG",
};

static char mine_map[MINE_H][MINE_W];

bool mineInside(int x, int y) {
  return x >= 0 && y >= 0 && x < MINE_W && y < MINE_H;
}

bool mineZombieAt(int x, int y) {
  for (uint8_t i = 0; i < mine.zombie_count; ++i) {
    if (mine.zombie_x[i] == x && mine.zombie_y[i] == y) return true;
  }
  return false;
}

bool minePassable(int x, int y) {
  if (!mineInside(x, y)) return false;
  char tile = mine_map[y][x];
  return tile == 'G';
}

void resetMine() {
  for (int y = 0; y < MINE_H; ++y) {
    for (int x = 0; x < MINE_W; ++x) {
      mine_map[y][x] = mine_seed[y][x];
    }
  }
  mine.player_x = 2;
  mine.player_y = 4;
  mine.facing_x = 0;
  mine.facing_y = -1;
  mine.zombie_count = 0;
  mine.hp = 3;
  mine.day = 1;
  mine.night = false;
  mine.game_over = false;
  mine.win = false;
  mine.show_help = true;
  mine.tilt.ready = true;
  mine.use_stone = false;
  mine.wood = 1;
  mine.stone = 0;
  mine.phase_start_ms = millis();
  mine.tilt.last_step_ms = millis();
  mine.last_zombie_ms = millis();
}

void spawnZombies() {
  mine.zombie_count = min<uint8_t>(mine.day, 2);
  mine.zombie_x[0] = 0;
  mine.zombie_y[0] = 0;
  mine.zombie_x[1] = MINE_W - 1;
  mine.zombie_y[1] = MINE_H - 1;
}

uint16_t mineColor(char tile) {
  switch (tile) {
    case 'T': return TFT_DARKGREEN;
    case 'R': return TFT_LIGHTGREY;
    case 'W': return TFT_ORANGE;
    case 'S': return TFT_DARKGREY;
    default: return TFT_GREEN;
  }
}

void drawMineGame() {
  game_canvas.fillSprite(TFT_BLACK);
  if (mine.show_help) {
    game_canvas.setFont(&fonts::Font2);
    game_canvas.setTextColor(TFT_CYAN, TFT_BLACK);
    game_canvas.setCursor(21, 12);
    game_canvas.print("MineZ");

    game_canvas.setFont(&fonts::Font0);
    game_canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    game_canvas.setCursor(8, 42);
    game_canvas.print("DAY: chop trees");
    game_canvas.setCursor(8, 60);
    game_canvas.print("and build walls");
    game_canvas.setCursor(8, 84);
    game_canvas.print("NIGHT: zombies");
    game_canvas.setCursor(8, 102);
    game_canvas.print("chase you");
    game_canvas.setCursor(8, 128);
    game_canvas.print("A chop/attack");
    game_canvas.setCursor(8, 146);
    game_canvas.print("B place wall");

    game_canvas.fillRoundRect(18, 176, 14, 14, 3, TFT_CYAN);
    game_canvas.fillRoundRect(52, 176, 14, 14, 3, TFT_RED);
    game_canvas.fillRect(86, 176, 14, 14, TFT_ORANGE);
    game_canvas.setCursor(9, 197);
    game_canvas.print("you zombie wall");
    game_canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
    game_canvas.setCursor(15, 190);
    game_canvas.print("Hold still");
    game_canvas.setCursor(30, 204);
    game_canvas.print("A START");
    game_canvas.pushSprite(0, CONTENT_Y);
    return;
  }

  game_canvas.setFont(&fonts::Font0);
  game_canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  game_canvas.setCursor(2, 3);
  uint32_t elapsed = millis() - mine.phase_start_ms;
  uint32_t limit = mine.night ? MINE_NIGHT_MS : MINE_DAY_MS;
  int remain = max<int>(0, static_cast<int>((limit - min(elapsed, limit)) / 1000));
  game_canvas.printf("%c%d HP%d W%d S%d %ds", mine.night ? 'N' : 'D', mine.day, mine.hp, mine.wood, mine.stone, remain);

  for (int y = 0; y < MINE_H; ++y) {
    for (int x = 0; x < MINE_W; ++x) {
      int px = MINE_X0 + x * MINE_TILE;
      int py = MINE_Y0 - CONTENT_Y + y * MINE_TILE;
      game_canvas.fillRect(px, py, MINE_TILE - 1, MINE_TILE - 1, mineColor(mine_map[y][x]));
      game_canvas.drawRect(px, py, MINE_TILE - 1, MINE_TILE - 1, TFT_BLACK);
      if (mine_map[y][x] == 'T') {
        game_canvas.fillCircle(px + 12, py + 8, 8, TFT_DARKGREEN);
        game_canvas.fillRect(px + 9, py + 12, 6, 9, TFT_BROWN);
      } else if (mine_map[y][x] == 'R') {
        game_canvas.fillCircle(px + 12, py + 12, 9, TFT_DARKGREY);
      } else if (mine_map[y][x] == 'W' || mine_map[y][x] == 'S') {
        game_canvas.fillRoundRect(px + 3, py + 3, MINE_TILE - 7, MINE_TILE - 7, 3, mine_map[y][x] == 'W' ? TFT_ORANGE : TFT_LIGHTGREY);
        game_canvas.drawRect(px + 5, py + 5, MINE_TILE - 11, MINE_TILE - 11, TFT_WHITE);
      }
    }
  }

  for (uint8_t i = 0; i < mine.zombie_count; ++i) {
    int px = MINE_X0 + mine.zombie_x[i] * MINE_TILE;
    int py = MINE_Y0 - CONTENT_Y + mine.zombie_y[i] * MINE_TILE;
    game_canvas.fillRoundRect(px + 2, py + 2, MINE_TILE - 5, MINE_TILE - 5, 6, TFT_RED);
    game_canvas.fillCircle(px + 8, py + 9, 2, TFT_WHITE);
    game_canvas.fillCircle(px + 16, py + 9, 2, TFT_WHITE);
    game_canvas.fillCircle(px + 8, py + 9, 1, TFT_BLACK);
    game_canvas.fillCircle(px + 16, py + 9, 1, TFT_BLACK);
    game_canvas.drawLine(px + 7, py + 17, px + 17, py + 17, TFT_BLACK);
  }

  int ppx = MINE_X0 + mine.player_x * MINE_TILE;
  int ppy = MINE_Y0 - CONTENT_Y + mine.player_y * MINE_TILE;
  game_canvas.fillRoundRect(ppx + 2, ppy + 2, MINE_TILE - 5, MINE_TILE - 5, 6, TFT_CYAN);
  game_canvas.fillCircle(ppx + 8, ppy + 9, 2, TFT_BLACK);
  game_canvas.fillCircle(ppx + 16, ppy + 9, 2, TFT_BLACK);
  game_canvas.drawLine(ppx + 8, ppy + 16, ppx + 16, ppy + 16, TFT_BLACK);

  int fx = mine.player_x + mine.facing_x;
  int fy = mine.player_y + mine.facing_y;
  if (mineInside(fx, fy)) {
    game_canvas.drawRect(MINE_X0 + fx * MINE_TILE, MINE_Y0 - CONTENT_Y + fy * MINE_TILE, MINE_TILE - 1, MINE_TILE - 1, TFT_WHITE);
  }

  if (mine.game_over || mine.win) {
    game_canvas.setFont(&fonts::Font2);
    game_canvas.setTextColor(mine.win ? TFT_CYAN : TFT_RED, TFT_BLACK);
    game_canvas.setCursor(mine.win ? 42 : 38, 190);
    game_canvas.print(mine.win ? "WIN" : "LOSE");
  } else {
    game_canvas.setTextColor(mine.use_stone ? TFT_LIGHTGREY : TFT_ORANGE, TFT_BLACK);
    game_canvas.setCursor(4, 203);
    game_canvas.print(mine.use_stone ? "MAT:STONE" : "MAT:WOOD");
  }

  game_canvas.pushSprite(0, CONTENT_Y);
}

void enterMine() {
  applyCapabilities(caps(false, false, true, false));
  resetMine();
  drawStatus("MINE");
  drawHint("A ACT B PUT AB");
  drawMineGame();
}

void mineAction() {
  int tx = mine.player_x + mine.facing_x;
  int ty = mine.player_y + mine.facing_y;
  if (!mineInside(tx, ty)) return;

  for (uint8_t i = 0; i < mine.zombie_count; ++i) {
    if (mine.zombie_x[i] == tx && mine.zombie_y[i] == ty) {
      for (uint8_t j = i; j + 1 < mine.zombie_count; ++j) {
        mine.zombie_x[j] = mine.zombie_x[j + 1];
        mine.zombie_y[j] = mine.zombie_y[j + 1];
      }
      mine.zombie_count--;
      return;
    }
  }

  char& tile = mine_map[ty][tx];
  if (tile == 'T') {
    tile = 'G';
    mine.wood++;
  } else if (tile == 'R') {
    tile = 'G';
    mine.stone++;
  } else if (tile == 'W') {
    tile = 'G';
    mine.wood++;
  } else if (tile == 'S') {
    tile = 'G';
    mine.stone++;
  }
}

void minePlace() {
  int tx = mine.player_x + mine.facing_x;
  int ty = mine.player_y + mine.facing_y;
  if (!mineInside(tx, ty) || mine_map[ty][tx] != 'G' || mineZombieAt(tx, ty)) return;
  if (mine.use_stone) {
    if (mine.stone == 0) return;
    mine.stone--;
    mine_map[ty][tx] = 'S';
  } else {
    if (mine.wood == 0) return;
    mine.wood--;
    mine_map[ty][tx] = 'W';
  }
}

void moveMinePlayer(int dx, int dy) {
  if (dx == 0 && dy == 0) return;
  mine.facing_x = dx;
  mine.facing_y = dy;
  int nx = mine.player_x + dx;
  int ny = mine.player_y + dy;
  if (minePassable(nx, ny) && !mineZombieAt(nx, ny)) {
    mine.player_x = nx;
    mine.player_y = ny;
  }
}

void moveZombies() {
  for (uint8_t i = 0; i < mine.zombie_count; ++i) {
    int dx = mine.player_x - mine.zombie_x[i];
    int dy = mine.player_y - mine.zombie_y[i];
    int step_x = abs(dx) >= abs(dy) ? (dx > 0 ? 1 : dx < 0 ? -1 : 0) : 0;
    int step_y = step_x == 0 ? (dy > 0 ? 1 : dy < 0 ? -1 : 0) : 0;
    int nx = mine.zombie_x[i] + step_x;
    int ny = mine.zombie_y[i] + step_y;
    if (nx == mine.player_x && ny == mine.player_y) {
      if (mine.hp > 0) mine.hp--;
      if (mine.hp == 0) mine.game_over = true;
      continue;
    }
    if (minePassable(nx, ny) && !mineZombieAt(nx, ny)) {
      mine.zombie_x[i] = nx;
      mine.zombie_y[i] = ny;
    } else {
      int alt_x = step_y ? (dx > 0 ? 1 : dx < 0 ? -1 : 0) : 0;
      int alt_y = step_x ? (dy > 0 ? 1 : dy < 0 ? -1 : 0) : 0;
      nx = mine.zombie_x[i] + alt_x;
      ny = mine.zombie_y[i] + alt_y;
      if (nx == mine.player_x && ny == mine.player_y) {
        if (mine.hp > 0) mine.hp--;
        if (mine.hp == 0) mine.game_over = true;
      } else if (minePassable(nx, ny) && !mineZombieAt(nx, ny)) {
        mine.zombie_x[i] = nx;
        mine.zombie_y[i] = ny;
      }
    }
  }
}

void updateMinePhase() {
  uint32_t now = millis();
  uint32_t limit = mine.night ? MINE_NIGHT_MS : MINE_DAY_MS;
  if (now - mine.phase_start_ms < limit) return;

  mine.phase_start_ms = now;
  if (!mine.night) {
    mine.night = true;
    spawnZombies();
  } else {
    mine.night = false;
    mine.zombie_count = 0;
    mine.day++;
    if (mine.day > 2) {
      mine.win = true;
      mine.game_over = true;
    }
  }
}

void updateMine() {
  if (M5.BtnA.wasClicked()) {
    if (mine.show_help) {
      resetTiltControl(mine.tilt);
      mine.show_help = false;
    } else if (mine.game_over) {
      resetMine();
      resetTiltControl(mine.tilt);
      mine.show_help = false;
    } else {
      mineAction();
    }
    drawMineGame();
  }

  if (mine.show_help) return;

  if (M5.BtnB.wasDoubleClicked()) {
    mine.use_stone = !mine.use_stone;
    drawMineGame();
    return;
  }
  if (M5.BtnB.wasClicked() && !mine.game_over) {
    minePlace();
    drawMineGame();
  }

  if (mine.game_over) return;

  uint32_t now = millis();
  StepDir dir = readTiltGesture(mine.tilt, 140);
  if (dir.dx || dir.dy) {
    moveMinePlayer(dir.dx, dir.dy);
    drawMineGame();
  }

  updateMinePhase();

  uint32_t zombie_interval = mine.day == 1 ? 850 : 650;
  if (mine.night && now - mine.last_zombie_ms > zombie_interval) {
    mine.last_zombie_ms = now;
    moveZombies();
    drawMineGame();
  }

  static uint32_t last_mine_clock_ms = 0;
  if (now - last_mine_clock_ms > 1000) {
    last_mine_clock_ms = now;
    drawMineGame();
  }
}

void enterHome() {
  mode = ScreenMode::Home;
  applyCapabilities(caps(false, false, false, false));
  drawHome();
}

void openSelectedApp() {
  if (home_index == 0) {
    mode = ScreenMode::Alert;
    enterAlert();
  } else if (home_index == 1) {
    mode = ScreenMode::Dodge;
    enterDodge();
  } else if (home_index == 2) {
    mode = ScreenMode::Stone;
    enterStone();
  } else if (home_index == 3) {
    mode = ScreenMode::Mine;
    enterMine();
  } else if (home_index == 4) {
    mode = ScreenMode::Settings;
    applyCapabilities(caps(false, false, false, false));
    drawSettings();
  }
}

void handleIdlePower() {
  if (mode == ScreenMode::Alert) {
    return;
  }
  if (!dimmed && settings.sleep_seconds > 0 && millis() - last_input_ms > static_cast<uint32_t>(settings.sleep_seconds) * 1000UL) {
    M5.Display.setBrightness(0);
    dimmed = true;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  setCpuFrequencyMhz(80);

  auto cfg = M5.config();
  cfg.fallback_board = m5::board_t::board_M5StickS3;
  cfg.internal_spk = false;
  cfg.internal_imu = false;
  cfg.internal_mic = false;
  M5.begin(cfg);
  M5.Display.setRotation(0);
  M5.Display.setTextDatum(textdatum_t::top_left);
  game_canvas.createSprite(SCREEN_W, CONTENT_H);

  loadSettings();
  setBrightness(settings.brightness);
  setSpeakerPower(false);
  applyIdleLdoPolicy();
  randomSeed(static_cast<uint32_t>(esp_random()));
  last_input_ms = millis();

  applyCapabilities(caps(false, false, false, false));
  enterHome();
}

void loop() {
  M5.update();

  bool any_input = M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnA.wasReleased() || M5.BtnB.wasReleased();
  if (any_input) {
    wakeDisplay();
  }

  if (comboPressedOnce(700)) {
    enterHome();
    return;
  }

  switch (mode) {
    case ScreenMode::Home:
      if (M5.BtnB.wasHold()) {
        home_index = (home_index + app_count - 1) % app_count;
        drawHome();
      } else if (M5.BtnB.wasClicked()) {
        home_index = (home_index + 1) % app_count;
        drawHome();
      }
      if (M5.BtnA.wasHold()) {
        mode = ScreenMode::Settings;
        drawSettings();
      } else if (M5.BtnA.wasClicked()) {
        openSelectedApp();
      }
      break;

    case ScreenMode::Settings:
      if (M5.BtnB.wasHold()) {
        settings_index = (settings_index + setting_count - 1) % setting_count;
        drawSettings();
      } else if (M5.BtnB.wasClicked()) {
        settings_index = (settings_index + 1) % setting_count;
        drawSettings();
      }
      if (M5.BtnA.wasHold()) {
        enterHome();
      } else if (M5.BtnA.wasClicked()) {
        changeCurrentSetting();
      }
      break;

    case ScreenMode::Dodge:
      updateDodge();
      break;

    case ScreenMode::Stone:
      updateStone();
      break;

    case ScreenMode::Mine:
      updateMine();
      break;

    case ScreenMode::Alert:
      if (M5.BtnA.wasHold()) {
        enterHome();
        break;
      }
      updateAlert();
      break;
  }

  handleIdlePower();
  if (mode == ScreenMode::Alert && !alert.active && !alert.screen_on && !alert.showing_last) {
    delay(150);
  } else if (mode == ScreenMode::Home || mode == ScreenMode::Settings) {
    delay(90);
  } else {
    delay(5);
  }
}
