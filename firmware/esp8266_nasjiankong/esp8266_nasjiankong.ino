#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <EEPROM.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <Updater.h>
#include <time.h>
#include "nas_notice_font.h"

#define EEPROM_SIZE 512
#define CONFIG_MAGIC 0x46
#define OLD_CONFIG_MAGIC 0x45
#define LCD_BL_PIN 5
#define LCD_PWM_BRIGHTEST_VALUE 120
#define LCD_PWM_DIMMEST_VALUE 900
#define LCD_PWM_OFF_VALUE 1023
#define UI_BG TFT_BLACK  // 这行是全局背景黑色。保留它，后面统一用 UI_BG，方便以后改背景色。
#define NOTICE_BANNER_MS 12000UL

#define AP_SSID "NAS-Monitor-Setup"
#define DNS_PORT 53

#define OTA_HOSTNAME "ESP8266-NAS-Monitor"
#define WEB_AUTH_USER "admin"

// Product-style Wi-Fi state machine
#define WIFI_CONNECT_WINDOW_MS 20000UL
#define WIFI_RETRY_WAIT_MS     10000UL
#define WIFI_AP_DELAY_MS       60000UL
#define WIFI_AP_RETRY_MS       30000UL
#define WIFI_STATUS_REFRESH_MS 1000UL

#define THEME_CLASSIC 0
#define THEME_CYBER_NET 1
#define THEME_CYBER_CPU_TEMP 2

TFT_eSPI tft = TFT_eSPI();
ESP8266WebServer server(80);
DNSServer dnsServer;

struct Config {
  uint8_t magic;
  char ssid[32];
  char wifiPass[64];
  char nasIp[32];
  uint16_t nasPort;
  char token[64];
  uint16_t refreshSec;
  uint8_t webAuthEnabled;
  char webPass[32];
  char title[24];   // 屏幕左上角 NAS 名称
  uint8_t displayPower;      // 1 = 屏幕开启，0 = 屏幕关闭
  uint8_t scheduleEnabled;   // 1 = 启用定时开关屏
  uint8_t onHour;
  uint8_t onMinute;
  uint8_t offHour;
  uint8_t offMinute;
  uint8_t brightness;        // 1-100 屏幕亮度百分比
  uint8_t theme;             // 0=经典 1=赛博网速 2=赛博CPU温度
};

Config cfg;

enum WifiState {
  WIFI_STATE_IDLE,
  WIFI_STATE_CONNECTING,
  WIFI_STATE_RETRY_WAIT,
  WIFI_STATE_AP_STA_RETRY,
  WIFI_STATE_CONNECTED
};

WifiState wifiState = WIFI_STATE_IDLE;

unsigned long lastFetch = 0;
unsigned long wifiBootStartMs = 0;
unsigned long wifiAttemptStartMs = 0;
unsigned long wifiRetryWaitStartMs = 0;
unsigned long lastWifiStatusDrawMs = 0;
unsigned long lastApStaRetryMs = 0;

bool configMode = false;
bool otaInProgress = false;
bool apStarted = false;
bool normalServicesStarted = false;
bool webServerStarted = false;
bool otaStarted = false;

String strongestSsid = "";
int strongestRssi = -999;

int lastCpu = -1;
int lastMem = -1;
int lastDisk = -1;
int lastTemp = -999;
String lastDown = "";
String lastUp = "";
String lastIp = "";
bool uiDrawn = false;
uint32_t otaExpectedSize = 0;
bool timeConfigured = false;
unsigned long lastClockDrawMs = 0;
String lastClockTime = "";
String lastClockDate = "";
unsigned long lastScheduleCheckMs = 0;
int lastScheduleMinute = -1;
String lastSeenEventId = "";
String noticeEventId = "";
String noticeEventApp = "";
String noticeEventLevel = "";
String noticeEventTime = "";
String noticeEventMessage = "";
bool noticeBannerActive = false;
unsigned long noticeBannerUntilMs = 0;
bool hasCachedStatus = false;
int cachedCpu = 0;
int cachedMem = 0;
int cachedDisk = 0;
int cachedTemp = 0;
String cachedDown = "-";
String cachedUp = "-";
String cachedStatusIp = "";

void fetchStatus();
void startWebServer();
void saveConfig();
bool requireWebAuth();
void handleClearWiFi();
void handleOpenAp();
void handleReboot();
void handleSetTheme();
void handleDisplayToggle();
void handleDisplayOn();
void handleDisplayOff();
void handleDisplayStatus();
void handleDisplayBrightness();
void sendDisplayStatusJson();
void setupTimeOnce();
bool isDisplayOn();
int getDisplayBrightnessPercent();
uint16_t brightnessPercentToPwm(int percent);
void applyBacklight();
void setDisplayBrightnessPercent(int percent, bool save);
void setDisplayPower(bool on, bool save);
void applyDisplaySchedule(bool force = false);
void drawClock(bool force = false);
void setupArduinoOTA();
void startConfigPortalNonBlocking();
void stopConfigPortalIfRunning();
void startNormalServicesOnce();
void beginWifiStateMachine();
void handleWifiState();
String getThemeName(uint8_t theme);
void drawClassicStaticUI(const String &ip);
void drawCyberNetStaticUI(const String &ip);
void drawCyberCpuTempStaticUI(const String &ip);
void updateCyberNetTheme(const String &down, const String &up);
void updateCyberCpuTempTheme(int cpu, int temp);
void drawNeonHex(int cx, int cy, int r, uint16_t color);
void drawCyberArrowDown(int x, int y, uint16_t color);
void drawCyberArrowUp(int x, int y, uint16_t color);
void drawCyberChip(int x, int y, uint16_t color);
void drawCyberThermo(int x, int y, uint16_t color);
void cacheStatusSnapshot(int cpu, int mem, int disk, int temp, const String &down, const String &up, const String &ip);
void redrawCachedStatus();
void clearNoticeBanner();
void handleNoticeBannerTimeout();
void processIncomingNotice(const JsonVariantConst &eventData);
void activateNoticeBanner(const String &eventId, const String &app, const String &level, const String &timeText, const String &message);
void drawNoticeBanner();
String sanitizeNoticeText(const String &input);
String nextUtf8Glyph(const String &input, int &index);
uint32_t utf8GlyphToCodepoint(const String &glyph);
bool findNoticeGb2312Glyph(uint32_t codepoint, uint16_t &glyphIndex);
bool isNoticeGlyphSupported(const String &glyph);
int noticeGlyphWidth(const String &glyph);
int noticeTextWidth(const String &text);
void trimTextToWidth(String &text, int maxWidth, const String &suffix = "");
int wrapNoticeText(const String &text, int maxWidth, int maxLines, String *lines);
void popLastUtf8Glyph(String &text);
void drawBitmapNoticeGlyph(int x, int y, const uint8_t *glyphData, int width, int height, uint16_t color);
void drawAsciiNoticeGlyph(int x, int y, char c, uint16_t color);
void drawGb2312NoticeGlyph(int x, int y, uint16_t glyphIndex, uint16_t color);
void drawNoticeGlyph(int x, int y, const String &glyph, uint16_t color);
void drawNoticeText(int x, int y, const String &text, uint16_t color);
String noticeLevelLabel(const String &level);
uint16_t noticeLevelColor(const String &level);
String formatNoticeTime(const String &timeText);

void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, cfg);

  if (cfg.magic == OLD_CONFIG_MAGIC) {
    cfg.magic = CONFIG_MAGIC;

    if (strlen(cfg.title) == 0) {
      strcpy(cfg.title, "UGREEN NAS");
    }
    cfg.displayPower = 1;
    cfg.scheduleEnabled = 0;
    cfg.onHour = 8;
    cfg.onMinute = 0;
    cfg.offHour = 23;
    cfg.offMinute = 0;
    cfg.brightness = 40;
    cfg.theme = THEME_CLASSIC;

    saveConfig();
    return;
  }

  if (cfg.magic != CONFIG_MAGIC) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = CONFIG_MAGIC;
    strcpy(cfg.ssid, "");
    strcpy(cfg.wifiPass, "");
    strcpy(cfg.nasIp, "");
    cfg.nasPort = 0;
    strcpy(cfg.token, "");
    cfg.refreshSec = 3;
    cfg.webAuthEnabled = 0;
    strcpy(cfg.webPass, "");
    strcpy(cfg.title, "UGREEN NAS");
    cfg.displayPower = 1;
    cfg.scheduleEnabled = 0;
    cfg.onHour = 8;
    cfg.onMinute = 0;
    cfg.offHour = 23;
    cfg.offMinute = 0;
    cfg.brightness = 40;
    cfg.theme = THEME_CLASSIC;
  }

  if (strlen(cfg.title) == 0) {
    strcpy(cfg.title, "UGREEN NAS");
  }
  if (cfg.displayPower > 1) cfg.displayPower = 1;
  if (cfg.scheduleEnabled > 1) cfg.scheduleEnabled = 0;
  if (cfg.onHour > 23) cfg.onHour = 8;
  if (cfg.onMinute > 59) cfg.onMinute = 0;
  if (cfg.offHour > 23) cfg.offHour = 23;
  if (cfg.offMinute > 59) cfg.offMinute = 0;
  if (cfg.brightness < 1 || cfg.brightness > 100) cfg.brightness = 40;
  if (cfg.theme > THEME_CYBER_CPU_TEMP) cfg.theme = THEME_CLASSIC;
}

bool isDisplayOn() {
  return cfg.displayPower == 1;
}


int getDisplayBrightnessPercent() {
  if (cfg.brightness < 1 || cfg.brightness > 100) return 40;
  return cfg.brightness;
}

uint16_t brightnessPercentToPwm(int percent) {
  percent = constrain(percent, 1, 100);
  long pwm = map(percent, 1, 100, LCD_PWM_DIMMEST_VALUE, LCD_PWM_BRIGHTEST_VALUE);
  return (uint16_t)constrain(pwm, LCD_PWM_BRIGHTEST_VALUE, LCD_PWM_DIMMEST_VALUE);
}

void applyBacklight() {
  if (isDisplayOn()) {
    analogWrite(LCD_BL_PIN, brightnessPercentToPwm(getDisplayBrightnessPercent()));
  } else {
    analogWrite(LCD_BL_PIN, LCD_PWM_OFF_VALUE);
  }
}

void setDisplayBrightnessPercent(int percent, bool save) {
  cfg.brightness = (uint8_t)constrain(percent, 1, 100);
  applyBacklight();
  if (save) saveConfig();
}

void setDisplayPower(bool on, bool save) {
  cfg.displayPower = on ? 1 : 0;

  if (on) {
    applyBacklight();
    uiDrawn = false;
    lastClockTime = "";
    lastClockDate = "";
  } else {
    clearNoticeBanner();
    tft.fillScreen(TFT_BLACK);
    applyBacklight();
    uiDrawn = false;
  }

  if (save) saveConfig();
}

bool isNowInsideDisplayWindow(int nowMin, int onMin, int offMin) {
  if (onMin == offMin) return true;
  if (onMin < offMin) return nowMin >= onMin && nowMin < offMin;
  return nowMin >= onMin || nowMin < offMin;
}

void applyDisplaySchedule(bool force) {
  if (cfg.scheduleEnabled != 1) return;
  if (!timeConfigured || WiFi.status() != WL_CONNECTED) return;

  unsigned long nowMs = millis();
  if (!force && nowMs - lastScheduleCheckMs < 10000UL) return;
  lastScheduleCheckMs = nowMs;

  time_t now = time(nullptr);
  if (now < 1700000000) return;

  struct tm *tmNow = localtime(&now);
  if (!tmNow) return;

  int currentMinute = tmNow->tm_hour * 60 + tmNow->tm_min;
  if (!force && currentMinute == lastScheduleMinute) return;
  lastScheduleMinute = currentMinute;

  int onMinuteTotal = cfg.onHour * 60 + cfg.onMinute;
  int offMinuteTotal = cfg.offHour * 60 + cfg.offMinute;
  bool shouldBeOn = isNowInsideDisplayWindow(currentMinute, onMinuteTotal, offMinuteTotal);

  if (shouldBeOn != isDisplayOn()) {
    setDisplayPower(shouldBeOn, true);
  }
}

bool isWebAuthEnabled() {
  return cfg.webAuthEnabled == 1 && strlen(cfg.webPass) > 0;
}

bool requireWebAuth() {
  if (!isWebAuthEnabled()) return true;
  if (server.authenticate(WEB_AUTH_USER, cfg.webPass)) return true;
  server.requestAuthentication();
  return false;
}

void saveConfig() {
  cfg.magic = CONFIG_MAGIC;
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

String htmlEscape(const String &s) {
  String out = s;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  out.replace("'", "&#39;");
  return out;
}

String getThemeName(uint8_t theme) {
  switch (theme) {
    case THEME_CYBER_NET: return "赛博网速";
    case THEME_CYBER_CPU_TEMP: return "赛博CPU+温度";
    default: return "经典主题";
  }
}


void drawMessage(const String &line1, const String &line2 = "", const String &line3 = "", const String &line4 = "") {
  uiDrawn = false;

  // 修复：如果屏幕处于手动关闭状态，重启或 Wi-Fi 重连时不要继续显示 Connecting WiFi。
  // 否则 LCD 画面会停留在连接界面，直到定时开启时才刷新。
  if (!isDisplayOn() && !otaInProgress) {
    tft.fillScreen(TFT_BLACK);
    return;
  }

  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  tft.setCursor(10, 36);
  tft.println(line1);
  tft.setCursor(10, 72);
  tft.println(line2);
  tft.setCursor(10, 108);
  tft.println(line3);
  tft.setCursor(10, 144);
  tft.println(line4);
}

void drawOtaProgress(int percent) {
  percent = constrain(percent, 0, 100);

  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(10, 45);
  tft.println("Firmware OTA");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(10, 85);
  tft.printf("Uploading %d%%", percent);

  tft.drawRect(10, 130, 220, 16, TFT_DARKGREY);
  tft.fillRect(12, 132, 216, 12, TFT_BLACK);
  tft.fillRect(12, 132, 216 * percent / 100, 12, TFT_GREEN);
}

void drawCpuIcon(int x, int y, uint16_t color) {
  tft.drawRoundRect(x, y, 20, 20, 3, color);
  tft.drawRect(x + 5, y + 5, 10, 10, color);
  for (int i = 3; i <= 15; i += 6) {
    tft.drawFastVLine(x - 3, y + i, 4, color);
    tft.drawFastVLine(x + 20, y + i, 4, color);
    tft.drawFastHLine(x + i, y - 3, 4, color);
    tft.drawFastHLine(x + i, y + 20, 4, color);
  }
}

void drawMemIcon(int x, int y, uint16_t color) {
  tft.drawRoundRect(x, y + 3, 22, 16, 3, color);
  for (int i = 3; i < 20; i += 5) {
    tft.drawFastVLine(x + i, y, 3, color);
    tft.drawFastVLine(x + i, y + 19, 3, color);
  }
}

void drawDiskIcon(int x, int y, uint16_t color) {
  tft.drawRoundRect(x, y, 22, 20, 3, color);
  tft.drawFastHLine(x + 3, y + 14, 16, color);
  tft.fillCircle(x + 17, y + 16, 1, color);
}

void drawTempIcon(int x, int y, uint16_t color) {
  tft.drawCircle(x + 10, y + 15, 5, color);
  tft.fillCircle(x + 10, y + 15, 3, color);
  tft.drawRoundRect(x + 7, y, 6, 15, 3, color);
}

void drawDownIcon(int x, int y, uint16_t color) {
  tft.drawFastVLine(x + 10, y, 14, color);
  tft.drawLine(x + 4, y + 8, x + 10, y + 15, color);
  tft.drawLine(x + 16, y + 8, x + 10, y + 15, color);
  tft.drawFastHLine(x + 4, y + 20, 13, color);
}

void drawUpIcon(int x, int y, uint16_t color) {
  tft.drawFastVLine(x + 10, y + 6, 14, color);
  tft.drawLine(x + 4, y + 8, x + 10, y + 1, color);
  tft.drawLine(x + 16, y + 8, x + 10, y + 1, color);
  tft.drawFastHLine(x + 4, y + 20, 13, color);
}

void drawCard(int x, int y, int w, int h, uint16_t borderColor) {
  tft.drawRoundRect(x, y, w, h, 8, borderColor);
  tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 7, TFT_DARKGREY);
}

void drawMiniBar(int x, int y, int w, int h, int percent, uint16_t color) {
  percent = constrain(percent, 0, 100);
  tft.drawRoundRect(x, y, w, h, 3, TFT_DARKGREY);
  tft.fillRect(x + 2, y + 2, w - 4, h - 4, UI_BG);
  int fillW = (w - 4) * percent / 100;
  if (fillW > 0) tft.fillRect(x + 2, y + 2, fillW, h - 4, color);
}

void drawClassicStaticUI(const String &ip) {
  if (!isDisplayOn()) return;
  tft.fillScreen(UI_BG);

  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, UI_BG);
  tft.setCursor(10, 8);

  String title = String(cfg.title);
  title.trim();
  if (title.length() == 0) title = "UGREEN NAS";
  if (title.length() > 10) title = title.substring(0, 10);
  tft.print(title);

  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, UI_BG);
  tft.setCursor(10, 30);
  tft.print("Panel ");
  tft.print(ip);

  drawClock(true);

  drawCard(8, 45, 108, 54, TFT_DARKGREY);
  drawCard(124, 45, 108, 54, TFT_DARKGREY);
  drawCpuIcon(16, 58, TFT_GREEN);
  drawMemIcon(132, 58, TFT_YELLOW);

  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, UI_BG);
  tft.setCursor(45, 55);
  tft.print("CPU");
  tft.setCursor(161, 55);
  tft.print("MEM");

  drawCard(8, 106, 108, 54, TFT_DARKGREY);
  drawCard(124, 106, 108, 54, TFT_DARKGREY);
  drawDiskIcon(16, 119, TFT_ORANGE);
  drawTempIcon(132, 119, TFT_RED);

  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, UI_BG);
  tft.setCursor(45, 116);
  tft.print("DISK");
  tft.setCursor(161, 116);
  tft.print("TEMP");

  drawCard(8, 168, 108, 58, TFT_DARKGREY);
  drawCard(124, 168, 108, 58, TFT_DARKGREY);
  drawDownIcon(16, 184, TFT_GREEN);
  drawUpIcon(132, 184, TFT_ORANGE);

  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, UI_BG);
  tft.setCursor(45, 180);
  tft.print("DOWN");
  tft.setCursor(161, 180);
  tft.print("UP");
}

void updatePercentCard(int value, int &lastValue, int valueX, int valueY, int barX, int barY, uint16_t color) {
  if (!isDisplayOn()) return;
  if (value == lastValue) return;
  tft.fillRect(valueX, valueY, 58, 18, UI_BG);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, UI_BG);
  tft.setCursor(valueX, valueY);
  tft.printf("%3d%%", value);
  drawMiniBar(barX, barY, 72, 6, value, color);
  lastValue = value;
}

void updateTempCard(int temp) {
  if (!isDisplayOn()) return;
  if (temp == lastTemp) return;

  tft.fillRect(161, 133, 62, 18, UI_BG);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, UI_BG);
  tft.setCursor(161, 133);

  if (temp > 0) {
    tft.printf("%2d", temp);
    tft.write(247);
    tft.print("C");
  } else {
    tft.print("N/A");
  }

  lastTemp = temp;
}

String formatSpeedForCard(String v) {
  v.trim();
  v.replace(" ", "");

  v.replace("KB/S", "K");
  v.replace("kb/s", "K");
  v.replace("KB/s", "K");
  v.replace("K/s", "K");

  v.replace("MB/S", "M");
  v.replace("mb/s", "M");
  v.replace("MB/s", "M");
  v.replace("M/s", "M");

  v.replace("B/S", "B");
  v.replace("b/s", "B");
  v.replace("B/s", "B");

  while (v.endsWith("s") || v.endsWith("S") || v.endsWith("/")) {
    v.remove(v.length() - 1);
  }

  if (v.length() > 6) v = v.substring(0, 6);
  return v;
}

void updateSpeedCard(const String &value, String &lastValue, int x, int y, uint16_t color) {
  if (!isDisplayOn()) return;
  String v = formatSpeedForCard(value);
  String oldV = formatSpeedForCard(lastValue);
  if (v == oldV) return;

  tft.fillRect(x, y - 2, 66, 24, UI_BG);

  tft.setTextSize(2);
  tft.setTextColor(color, UI_BG);
  tft.setCursor(x, y);
  tft.print(v);

  lastValue = value;
}

void setupTimeOnce() {
  if (timeConfigured) return;
  configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp1.aliyun.com", "pool.ntp.org");
  timeConfigured = true;
}

void drawClock(bool force) {
  if (!isDisplayOn()) return;
  if (!timeConfigured || WiFi.status() != WL_CONNECTED) return;

  unsigned long nowMs = millis();
  if (!force && nowMs - lastClockDrawMs < 1000UL) return;
  lastClockDrawMs = nowMs;

  time_t now = time(nullptr);
  if (now < 1700000000) return;

  struct tm *tmNow = localtime(&now);
  if (!tmNow) return;

  char timeBuf[8];
  char dateBuf[16];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", tmNow->tm_hour, tmNow->tm_min);
  snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", tmNow->tm_year + 1900, tmNow->tm_mon + 1, tmNow->tm_mday);

  String timeStr = String(timeBuf);
  String dateStr = String(dateBuf);

  if (!force && timeStr == lastClockTime && dateStr == lastClockDate) return;

  tft.fillRect(135, 6, 100, 32, UI_BG);

  tft.setTextColor(TFT_CYAN, UI_BG);
  tft.setTextSize(2);
  int timeX = 240 - 10 - tft.textWidth(timeStr);
  if (timeX < 135) timeX = 135;
  tft.setCursor(timeX, 8);
  tft.print(timeStr);

  tft.setTextColor(TFT_LIGHTGREY, UI_BG);
  tft.setTextSize(1);
  int dateX = 240 - 10 - tft.textWidth(dateStr);
  if (dateX < 135) dateX = 135;
  tft.setCursor(dateX, 30);
  tft.print(dateStr);

  lastClockTime = timeStr;
  lastClockDate = dateStr;
}


void drawNeonHex(int cx, int cy, int r, uint16_t color) {
  int h = (r * 86) / 100;
  tft.drawLine(cx - r, cy, cx - r / 2, cy - h, color);
  tft.drawLine(cx - r / 2, cy - h, cx + r / 2, cy - h, color);
  tft.drawLine(cx + r / 2, cy - h, cx + r, cy, color);
  tft.drawLine(cx + r, cy, cx + r / 2, cy + h, color);
  tft.drawLine(cx + r / 2, cy + h, cx - r / 2, cy + h, color);
  tft.drawLine(cx - r / 2, cy + h, cx - r, cy, color);
}

void drawCyberArrowDown(int x, int y, uint16_t color) {
  tft.drawFastVLine(x + 10, y, 22, color);
  tft.drawLine(x + 2, y + 14, x + 10, y + 24, color);
  tft.drawLine(x + 18, y + 14, x + 10, y + 24, color);
  tft.drawFastHLine(x + 1, y + 28, 19, color);
}

void drawCyberArrowUp(int x, int y, uint16_t color) {
  tft.drawFastVLine(x + 10, y + 6, 22, color);
  tft.drawLine(x + 2, y + 14, x + 10, y + 4, color);
  tft.drawLine(x + 18, y + 14, x + 10, y + 4, color);
  tft.drawFastHLine(x + 1, y + 28, 19, color);
}

void drawCyberChip(int x, int y, uint16_t color) {
  tft.drawRoundRect(x, y, 28, 28, 4, color);
  tft.drawRect(x + 7, y + 7, 14, 14, color);
  for (int i = 4; i <= 20; i += 8) {
    tft.drawFastVLine(x - 3, y + i, 4, color);
    tft.drawFastVLine(x + 27, y + i, 4, color);
    tft.drawFastHLine(x + i, y - 3, 4, color);
    tft.drawFastHLine(x + i, y + 27, 4, color);
  }
}

void drawCyberThermo(int x, int y, uint16_t color) {
  tft.drawRoundRect(x + 8, y, 8, 24, 4, color);
  tft.drawCircle(x + 12, y + 28, 8, color);
  tft.fillCircle(x + 12, y + 28, 3, color);
  tft.drawFastVLine(x + 12, y + 6, 17, color);
}



void drawCyberNetStaticUI(const String &ip) {
  if (!isDisplayOn()) return;
  tft.fillScreen(TFT_BLACK);

  String title = String(cfg.title);
  title.trim();
  if (title.length() == 0) title = "UGREEN NAS";
  if (title.length() > 11) title = title.substring(0, 11);

  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(8, 8);
  tft.print(title);

  drawClock(true);

  // 顶部科技感装饰线
  tft.drawFastHLine(8, 34, 224, TFT_DARKGREY);
  tft.drawFastHLine(8, 35, 224, TFT_DARKGREY);

  // 下载区域
  tft.drawFastHLine(18, 58, 204, TFT_DARKGREY);
  tft.drawFastHLine(18, 59, 204, TFT_DARKGREY);
  tft.drawFastVLine(18, 58, 16, TFT_CYAN);
  tft.drawFastVLine(19, 58, 16, TFT_CYAN);
  drawCyberArrowDown(24, 48, TFT_CYAN);

  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  int downLabelX = (240 - tft.textWidth("DOWNLOAD")) / 2;
  if (downLabelX < 0) downLabelX = 0;
  tft.setCursor(downLabelX, 44);
  tft.print("DOWNLOAD");

  // 中间分割带
  tft.drawFastHLine(12, 122, 216, TFT_DARKGREY);
  tft.drawFastHLine(12, 123, 216, TFT_DARKGREY);
  tft.drawFastHLine(24, 126, 192, TFT_DARKGREY);

  // 上传区域
  tft.drawFastHLine(18, 152, 204, TFT_DARKGREY);
  tft.drawFastHLine(18, 153, 204, TFT_DARKGREY);
  tft.drawFastVLine(220, 152, 16, TFT_MAGENTA);
  tft.drawFastVLine(221, 152, 16, TFT_MAGENTA);
  drawCyberArrowUp(190, 142, TFT_MAGENTA);

  tft.setTextSize(2);
  tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
  int upLabelX = (240 - tft.textWidth("UPLOAD")) / 2;
  if (upLabelX < 0) upLabelX = 0;
  tft.setCursor(upLabelX, 138);
  tft.print("UPLOAD");

  // 底部科技感线条
  tft.drawFastHLine(8, 230, 224, TFT_DARKGREY);
}




void drawCyberCpuTempStaticUI(const String &ip) {
  if (!isDisplayOn()) return;
  tft.fillScreen(TFT_BLACK);

  String title = String(cfg.title);
  title.trim();
  if (title.length() == 0) title = "UGREEN NAS";
  if (title.length() > 11) title = title.substring(0, 11);

  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(8, 8);
  tft.print(title);

  drawClock(true);

  tft.drawFastHLine(8, 34, 224, TFT_DARKGREY);
  tft.drawFastHLine(8, 35, 224, TFT_DARKGREY);

  drawCyberChip(20, 48, TFT_CYAN);
  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(52, 52);
  tft.print("CPU LOAD");

  tft.drawFastHLine(18, 78, 204, TFT_DARKGREY);
  tft.drawFastHLine(18, 79, 204, TFT_DARKGREY);

  tft.drawFastHLine(12, 122, 216, TFT_DARKGREY);
  tft.drawFastHLine(12, 123, 216, TFT_DARKGREY);
  tft.drawFastHLine(24, 126, 192, TFT_DARKGREY);

  drawCyberThermo(20, 142, TFT_ORANGE);
  tft.setTextSize(2);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.setCursor(52, 146);
  tft.print("CPU TEMP");

  tft.drawFastHLine(18, 172, 204, TFT_DARKGREY);
  tft.drawFastHLine(18, 173, 204, TFT_DARKGREY);

  tft.drawFastHLine(8, 230, 224, TFT_DARKGREY);
}




void updateCyberNetTheme(const String &down, const String &up) {
  if (!isDisplayOn()) return;

  String dv = formatSpeedForCard(down);
  String uv = formatSpeedForCard(up);

  if (dv != formatSpeedForCard(lastDown)) {
    tft.fillRect(0, 68, 240, 46, TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(4);
    int x = (240 - tft.textWidth(dv)) / 2;
    if (x < 0) x = 0;
    tft.setCursor(x, 76);
    tft.print(dv);

    // 下载速度装饰
    tft.drawFastHLine(36, 112, 168, TFT_CYAN);
    tft.drawFastHLine(48, 115, 144, TFT_DARKGREY);
    lastDown = down;
  }

  if (uv != formatSpeedForCard(lastUp)) {
    tft.fillRect(0, 162, 240, 46, TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(4);
    int x = (240 - tft.textWidth(uv)) / 2;
    if (x < 0) x = 0;
    tft.setCursor(x, 170);
    tft.print(uv);

    // 上传速度装饰
    tft.drawFastHLine(36, 206, 168, TFT_MAGENTA);
    tft.drawFastHLine(48, 209, 144, TFT_DARKGREY);
    lastUp = up;
  }
}




void updateCyberCpuTempTheme(int cpu, int temp) {
  if (!isDisplayOn()) return;

  if (cpu != lastCpu) {
    tft.fillRect(0, 82, 240, 44, TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(4);
    String cpuText = String(constrain(cpu, 0, 100)) + "%";
    int x = (240 - tft.textWidth(cpuText)) / 2;
    if (x < 0) x = 0;
    tft.setCursor(x, 88);
    tft.print(cpuText);

    tft.drawFastHLine(32, 118, 176, TFT_CYAN);
    tft.drawFastHLine(44, 121, 152, TFT_DARKGREY);
    int fill = (176 * constrain(cpu, 0, 100)) / 100;
    if (fill > 0) tft.drawFastHLine(32, 118, fill, TFT_CYAN);
    lastCpu = cpu;
  }

  if (temp != lastTemp) {
    tft.fillRect(0, 176, 240, 44, TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(4);
    String tempText;
    if (temp > 0) {
      tempText = String(temp) + "C";
    } else {
      tempText = "N/A";
    }
    int x = (240 - tft.textWidth(tempText)) / 2;
    if (x < 0) x = 0;
    tft.setCursor(x, 182);
    tft.print(tempText);

    int norm = constrain(temp, 0, 100);
    tft.drawFastHLine(32, 212, 176, TFT_ORANGE);
    tft.drawFastHLine(44, 215, 152, TFT_DARKGREY);
    int fill = (176 * norm) / 100;
    if (fill > 0) tft.drawFastHLine(32, 212, fill, TFT_ORANGE);
    lastTemp = temp;
  }
}


void drawStatus(int cpu, int mem, int disk, int temp, const String &down, const String &up, const String &ip) {
  if (!isDisplayOn()) {
    uiDrawn = false;
    return;
  }
  if (!uiDrawn || ip != lastIp) {
    lastCpu = -1;
    lastMem = -1;
    lastDisk = -1;
    lastTemp = -999;
    lastDown = "";
    lastUp = "";
    lastIp = ip;
    if (cfg.theme == THEME_CYBER_NET) {
      drawCyberNetStaticUI(ip);
    } else if (cfg.theme == THEME_CYBER_CPU_TEMP) {
      drawCyberCpuTempStaticUI(ip);
    } else {
      drawClassicStaticUI(ip);
    }
    uiDrawn = true;
  }

  if (cfg.theme == THEME_CYBER_NET) {
    updateCyberNetTheme(down, up);
    return;
  }

  if (cfg.theme == THEME_CYBER_CPU_TEMP) {
    updateCyberCpuTempTheme(cpu, temp);
    return;
  }

  updatePercentCard(cpu, lastCpu, 45, 70, 34, 91, TFT_GREEN);
  updatePercentCard(mem, lastMem, 161, 70, 150, 91, TFT_YELLOW);
  updatePercentCard(disk, lastDisk, 45, 131, 34, 152, TFT_ORANGE);
  updateTempCard(temp);
  updateSpeedCard(down, lastDown, 45, 199, TFT_GREEN);
  updateSpeedCard(up, lastUp, 161, 199, TFT_ORANGE);
}

void cacheStatusSnapshot(int cpu, int mem, int disk, int temp, const String &down, const String &up, const String &ip) {
  cachedCpu = cpu;
  cachedMem = mem;
  cachedDisk = disk;
  cachedTemp = temp;
  cachedDown = down;
  cachedUp = up;
  cachedStatusIp = ip;
  hasCachedStatus = true;
}

void redrawCachedStatus() {
  if (!hasCachedStatus || !isDisplayOn()) return;
  uiDrawn = false;
  drawStatus(cachedCpu, cachedMem, cachedDisk, cachedTemp, cachedDown, cachedUp, cachedStatusIp);
}

void clearNoticeBanner() {
  noticeBannerActive = false;
  noticeBannerUntilMs = 0;
  noticeEventId = "";
  noticeEventApp = "";
  noticeEventLevel = "";
  noticeEventTime = "";
  noticeEventMessage = "";
}

void handleNoticeBannerTimeout() {
  if (!noticeBannerActive) return;
  if ((int32_t)(millis() - noticeBannerUntilMs) < 0) return;
  clearNoticeBanner();
  redrawCachedStatus();
}

String nextUtf8Glyph(const String &input, int &index) {
  if (index >= input.length()) return "";

  uint8_t first = (uint8_t)input[index];
  int size = 1;
  if ((first & 0xE0) == 0xC0 && index + 1 < input.length()) size = 2;
  else if ((first & 0xF0) == 0xE0 && index + 2 < input.length()) size = 3;
  else if ((first & 0xF8) == 0xF0 && index + 3 < input.length()) size = 4;

  String glyph = input.substring(index, index + size);
  index += size;
  return glyph;
}

uint32_t utf8GlyphToCodepoint(const String &glyph) {
  int len = glyph.length();
  if (len <= 0) return 0;

  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(glyph.c_str());
  if (len == 1) return bytes[0];
  if (len == 2) {
    return ((uint32_t)(bytes[0] & 0x1F) << 6) |
           (uint32_t)(bytes[1] & 0x3F);
  }
  if (len == 3) {
    return ((uint32_t)(bytes[0] & 0x0F) << 12) |
           ((uint32_t)(bytes[1] & 0x3F) << 6) |
           (uint32_t)(bytes[2] & 0x3F);
  }
  if (len == 4) {
    return ((uint32_t)(bytes[0] & 0x07) << 18) |
           ((uint32_t)(bytes[1] & 0x3F) << 12) |
           ((uint32_t)(bytes[2] & 0x3F) << 6) |
           (uint32_t)(bytes[3] & 0x3F);
  }
  return 0;
}

bool findNoticeGb2312Glyph(uint32_t codepoint, uint16_t &glyphIndex) {
  if (codepoint == 0 || codepoint > 0xFFFF || kNasNoticeGb2312Count == 0) return false;

  int low = 0;
  int high = kNasNoticeGb2312Count - 1;
  while (low <= high) {
    int mid = low + ((high - low) / 2);
    uint16_t midCodepoint = pgm_read_word(&NasNoticeGb2312Codepoints[mid]);
    if (midCodepoint == codepoint) {
      glyphIndex = (uint16_t)mid;
      return true;
    }
    if (midCodepoint < codepoint) low = mid + 1;
    else high = mid - 1;
  }
  return false;
}

bool isNoticeGlyphSupported(const String &glyph) {
  if (glyph.length() == 0) return false;
  if (glyph.length() == 1) {
    uint8_t c = (uint8_t)glyph[0];
    if (c >= kNasNoticeAsciiFirst && c <= kNasNoticeAsciiLast) return true;
  }
  uint16_t glyphIndex = 0;
  return findNoticeGb2312Glyph(utf8GlyphToCodepoint(glyph), glyphIndex);
}

String sanitizeNoticeText(const String &input) {
  String out = "";
  int index = 0;
  while (index < input.length()) {
    String glyph = nextUtf8Glyph(input, index);
    if (glyph == "\r") continue;
    if (glyph == "\n" || glyph == "\t") {
      out += " ";
      continue;
    }
    if (isNoticeGlyphSupported(glyph)) out += glyph;
    else out += "?";
  }
  return out;
}

void popLastUtf8Glyph(String &text) {
  if (text.length() == 0) return;
  int index = text.length() - 1;
  while (index > 0 && (((uint8_t)text[index] & 0xC0) == 0x80)) {
    index--;
  }
  text.remove(index);
}

int noticeGlyphWidth(const String &glyph) {
  if (glyph.length() == 0) return 0;
  if (glyph.length() == 1) {
    uint8_t c = (uint8_t)glyph[0];
    if (c >= kNasNoticeAsciiFirst && c <= kNasNoticeAsciiLast) return kNasNoticeAsciiWidth;
  }
  uint16_t glyphIndex = 0;
  if (findNoticeGb2312Glyph(utf8GlyphToCodepoint(glyph), glyphIndex)) {
    return kNasNoticeGb2312Width;
  }
  return kNasNoticeAsciiWidth;
}

int noticeTextWidth(const String &text) {
  int width = 0;
  int index = 0;
  while (index < text.length()) {
    width += noticeGlyphWidth(nextUtf8Glyph(text, index));
  }
  return width;
}

void trimTextToWidth(String &text, int maxWidth, const String &suffix) {
  if (noticeTextWidth(text) <= maxWidth) return;
  while (text.length() > 0 && noticeTextWidth(text + suffix) > maxWidth) {
    popLastUtf8Glyph(text);
  }
  text += suffix;
}

int wrapNoticeText(const String &text, int maxWidth, int maxLines, String *lines) {
  String sanitized = sanitizeNoticeText(text);
  int sourceIndex = 0;
  int lineCount = 0;

  while (sourceIndex < sanitized.length() && lineCount < maxLines) {
    String line = "";
    while (sourceIndex < sanitized.length()) {
      int nextIndex = sourceIndex;
      String glyph = nextUtf8Glyph(sanitized, nextIndex);
      if (glyph == " " && line.length() == 0) {
        sourceIndex = nextIndex;
        continue;
      }

      String candidate = line + glyph;
      if (line.length() > 0 && noticeTextWidth(candidate) > maxWidth) break;

      line = candidate;
      sourceIndex = nextIndex;
    }

    if (line.length() == 0 && sourceIndex < sanitized.length()) {
      line = nextUtf8Glyph(sanitized, sourceIndex);
    }

    lines[lineCount++] = line;
  }

  if (sourceIndex < sanitized.length() && lineCount > 0) {
    trimTextToWidth(lines[lineCount - 1], maxWidth, "...");
  }

  return lineCount;
}

void drawBitmapNoticeGlyph(int x, int y, const uint8_t *glyphData, int width, int height, uint16_t color) {
  int rowBytes = (width + 7) / 8;
  for (int row = 0; row < height; ++row) {
    int runStart = -1;
    for (int col = 0; col < width; ++col) {
      uint8_t packed = pgm_read_byte(glyphData + (row * rowBytes) + (col / 8));
      bool on = (packed & (0x80 >> (col % 8))) != 0;
      if (on && runStart < 0) {
        runStart = col;
      } else if (!on && runStart >= 0) {
        tft.drawFastHLine(x + runStart, y + row, col - runStart, color);
        runStart = -1;
      }
    }
    if (runStart >= 0) {
      tft.drawFastHLine(x + runStart, y + row, width - runStart, color);
    }
  }
}

void drawAsciiNoticeGlyph(int x, int y, char c, uint16_t color) {
  uint8_t uc = (uint8_t)c;
  if (uc < kNasNoticeAsciiFirst || uc > kNasNoticeAsciiLast) uc = '?';
  int offset = (uc - kNasNoticeAsciiFirst) * kNasNoticeAsciiBytesPerGlyph;
  drawBitmapNoticeGlyph(x, y, NasNoticeAsciiGlyphs + offset, kNasNoticeAsciiWidth, kNasNoticeAsciiHeight, color);
}

void drawGb2312NoticeGlyph(int x, int y, uint16_t glyphIndex, uint16_t color) {
  int offset = glyphIndex * kNasNoticeGb2312BytesPerGlyph;
  drawBitmapNoticeGlyph(x, y, NasNoticeGb2312Glyphs + offset, kNasNoticeGb2312Width, kNasNoticeGb2312Height, color);
}

void drawNoticeGlyph(int x, int y, const String &glyph, uint16_t color) {
  if (glyph.length() == 0) return;
  if (glyph.length() == 1) {
    uint8_t c = (uint8_t)glyph[0];
    if (c >= kNasNoticeAsciiFirst && c <= kNasNoticeAsciiLast) {
      drawAsciiNoticeGlyph(x, y, c, color);
      return;
    }
  }

  uint16_t glyphIndex = 0;
  if (findNoticeGb2312Glyph(utf8GlyphToCodepoint(glyph), glyphIndex)) {
    drawGb2312NoticeGlyph(x, y, glyphIndex, color);
    return;
  }
  drawAsciiNoticeGlyph(x, y, '?', color);
}

void drawNoticeText(int x, int y, const String &text, uint16_t color) {
  int cursorX = x;
  int index = 0;
  while (index < text.length()) {
    String glyph = nextUtf8Glyph(text, index);
    drawNoticeGlyph(cursorX, y, glyph, color);
    cursorX += noticeGlyphWidth(glyph);
  }
}

String noticeLevelLabel(const String &level) {
  String lower = level;
  lower.toLowerCase();
  if (lower == "error" || lower == "alert") return "ERROR";
  if (lower == "warning" || lower == "warn") return "WARN";
  return "INFO";
}

uint16_t noticeLevelColor(const String &level) {
  String lower = level;
  lower.toLowerCase();
  if (lower == "error" || lower == "alert") return TFT_RED;
  if (lower == "warning" || lower == "warn") return TFT_ORANGE;
  return TFT_CYAN;
}

String formatNoticeTime(const String &timeText) {
  String trimmed = timeText;
  trimmed.trim();
  int colon = trimmed.lastIndexOf(':');
  if (colon >= 2 && colon + 2 < trimmed.length()) {
    return trimmed.substring(colon - 2, colon + 3);
  }
  return trimmed;
}

void activateNoticeBanner(const String &eventId, const String &app, const String &level, const String &timeText, const String &message) {
  noticeEventId = eventId;
  noticeEventApp = app;
  noticeEventLevel = level;
  noticeEventTime = timeText;
  noticeEventMessage = message;
  noticeBannerActive = true;
  noticeBannerUntilMs = millis() + NOTICE_BANNER_MS;
}

void processIncomingNotice(const JsonVariantConst &eventData) {
  if (eventData.isNull()) return;

  String eventId = eventData["id"] | "";
  eventId.trim();
  if (eventId.length() == 0) return;
  if (eventId == lastSeenEventId) return;

  lastSeenEventId = eventId;
  if (!isDisplayOn()) return;

  String app = eventData["app"] | "";
  String level = eventData["level"] | "";
  String timeText = eventData["time"] | "";
  String message = eventData["message"] | "";

  app.trim();
  level.trim();
  timeText.trim();
  message.trim();

  if (app.length() == 0) app = "NAS";
  if (message.length() == 0) message = "No details";

  activateNoticeBanner(eventId, app, level, timeText, message);
}

void drawNoticeBanner() {
  if (!noticeBannerActive || !isDisplayOn()) return;

  const int boxX = 8;
  const int boxY = 42;
  const int boxW = 224;
  const int boxH = 74;
  const int bodyX = boxX + 12;
  const int bodyWidth = boxW - 24;
  uint16_t accent = noticeLevelColor(noticeEventLevel);

  tft.fillRoundRect(boxX, boxY, boxW, boxH, 10, TFT_BLACK);
  tft.drawRoundRect(boxX, boxY, boxW, boxH, 10, accent);
  tft.drawFastHLine(boxX + 10, boxY + 22, boxW - 20, accent);

  String rightText = noticeLevelLabel(noticeEventLevel);
  String timeShort = formatNoticeTime(noticeEventTime);
  if (timeShort.length() > 0) {
    rightText += " ";
    rightText += timeShort;
  }

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextSize(1);
  int rightX = boxX + boxW - 12 - tft.textWidth(rightText, 2);
  if (rightX < boxX + 110) rightX = boxX + 110;
  tft.setCursor(rightX, boxY + 6);
  tft.print(rightText);

  String appText = sanitizeNoticeText(noticeEventApp);
  trimTextToWidth(appText, rightX - bodyX - 8, "...");
  drawNoticeText(bodyX, boxY + 3, appText, accent);

  String lines[2];
  int lineCount = wrapNoticeText(noticeEventMessage, bodyWidth, 2, lines);
  int lineHeight = kNasNoticeGb2312Height + 2;
  int firstLineY = boxY + 28;
  for (int i = 0; i < lineCount; ++i) {
    drawNoticeText(bodyX, firstLineY + (i * lineHeight), lines[i], TFT_WHITE);
  }
  tft.setTextFont(1);
  tft.setTextSize(1);
}

void scanWiFiNetworks() {
  strongestSsid = "";
  strongestRssi = -999;
  WiFi.mode(WIFI_AP_STA);
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    if (ssid.length() > 0 && rssi > strongestRssi) {
      strongestRssi = rssi;
      strongestSsid = ssid;
    }
  }
}

String getDefaultSsid() {
  if (strlen(cfg.ssid) > 0) return String(cfg.ssid);
  return strongestSsid;
}

String buildWiFiOptionsHtml() {
  String html = "";
  int n = WiFi.scanComplete();
  String selectedSsid = getDefaultSsid();

  html += "<div class='wifi-list'>";
  html += "<div class='hint'>附近 Wi-Fi：</div>";

  if (n <= 0) {
    html += "<div class='hint'>尚未扫描。点击上方“扫描附近 Wi-Fi”按钮后，可从列表选择。</div>";
  } else {
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;
      int rssi = WiFi.RSSI(i);
      String enc = WiFi.encryptionType(i) == ENC_TYPE_NONE ? "Open" : "Secured";
      String checked = (ssid == selectedSsid) ? " checked" : "";

      html += "<label class='wifi-item'>";
      html += "<input type='radio' name='ssidRadio' value='";
      html += htmlEscape(ssid);
      html += "'";
      html += checked;
      html += " onclick=\"document.getElementById('ssid').value=this.value\">";
      html += "<span>";
      html += htmlEscape(ssid);
      html += "</span>";
      html += "<em>";
      html += String(rssi);
      html += " dBm / ";
      html += enc;
      html += "</em>";
      html += "</label>";
    }
  }

  html += "</div>";
  return html;
}


void handleSetTheme() {
  if (!requireWebAuth()) return;
  if (!server.hasArg("theme")) {
    server.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"msg\":\"missing theme\"}");
    return;
  }
  int theme = server.arg("theme").toInt();
  if (theme < 0) theme = 0;
  if (theme > 2) theme = 2;
  cfg.theme = (uint8_t)theme;
  saveConfig();
  uiDrawn = false;
  lastClockTime = "";
  lastClockDate = "";
  server.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}

void handleRoot() {
  if (!requireWebAuth()) return;

  String defaultSsid = getDefaultSsid();
  String nasPortValue = cfg.nasPort > 0 ? String(cfg.nasPort) : "";

  String page = "";
  page += "<!doctype html><html><head><meta charset='utf-8'>";
  page += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>NAS Monitor</title>";
  page += "<style>";
  page += "body{font-family:Arial,'Microsoft YaHei',sans-serif;padding:20px;max-width:620px;margin:auto;background:#111;color:#eee}";
  page += "h2{margin-top:0}";
  page += "input,select{width:100%;padding:10px;margin:8px 0 16px;box-sizing:border-box;border-radius:6px;border:1px solid #444;background:#222;color:#fff}";
  page += "input::placeholder{color:#777}";
  page += "button{width:100%;padding:12px;background:#0aa7ff;color:white;border:0;border-radius:6px;font-size:16px;margin-top:8px;margin-bottom:12px}";
  page += ".passwrap{position:relative;margin:8px 0 16px}";
  page += ".passwrap input{margin:0;padding-right:48px}";
  page += ".eye{position:absolute;right:8px;top:50%;transform:translateY(-50%);width:34px;height:34px;margin:0;padding:0;border-radius:8px;background:#444;color:#eee;font-size:18px;line-height:34px}";
  page += "label{font-weight:bold}";
  page += ".hint{color:#aaa;font-size:13px;line-height:1.5;margin:6px 0}";
  page += ".wifi-list{background:#1b1b1b;border:1px solid #333;border-radius:8px;padding:10px;margin:8px 0 16px}";
  page += ".wifi-item{display:flex;align-items:center;gap:8px;font-weight:normal;padding:8px;border-bottom:1px solid #2a2a2a}";
  page += ".wifi-item:last-child{border-bottom:0}";
  page += ".wifi-item input{width:auto;margin:0}";
  page += ".wifi-item span{flex:1}";
  page += ".wifi-item em{font-style:normal;color:#aaa;font-size:12px}";
  page += ".smallbtn{background:#333}.statusbox{background:#1b1b1b;border:1px solid #333;border-radius:8px;padding:10px;margin:8px 0 16px}.on{color:#20c997}.off{color:#ff6b6b}.grid2{display:grid;grid-template-columns:1fr 1fr;gap:10px}.grid2 input{margin-top:8px}";
  page += "</style>";
  page += "<script>";
  page += "function pickStrongest(){";
  page += "var r=document.querySelector('input[name=ssidRadio]:checked');";
  page += "if(r){document.getElementById('ssid').value=r.value;}";
  page += "}";
  page += "function togglePass(id,btn){";
  page += "var i=document.getElementById(id);";
  page += "if(!i)return;";
  page += "if(i.type==='password'){i.type='text';btn.textContent='🙈';}else{i.type='password';btn.textContent='👁';}";
  page += "}";
  page += "</script>";
  page += "</head><body onload='pickStrongest()'>";

  page += "<h2>配置中心</h2>";
  page += "<p class='hint'>连接热点后，如果手机未自动弹出页面，请手动打开 http://192.168.4.1</p>";

  page += "<form method='POST' action='/save'>";

  page += "<label>设备名</label>";
  page += "<input name='title' value='" + htmlEscape(String(cfg.title)) + "' placeholder='例如 UGREEN NAS / HOME NAS / NAS-01'>";

  page += "<label>显示主题（切换后立即生效）</label>";
  page += "<select name='theme' onchange=\"fetch('/settheme?theme='+this.value).catch(function(){})\">";
  page += "<option value='0'" + String(cfg.theme == THEME_CLASSIC ? " selected" : "") + ">经典主题（保持当前样式）</option>";
  page += "<option value='1'" + String(cfg.theme == THEME_CYBER_NET ? " selected" : "") + ">赛博网速主题</option>";
  page += "<option value='2'" + String(cfg.theme == THEME_CYBER_CPU_TEMP ? " selected" : "") + ">赛博 CPU+温度主题</option>";
  page += "</select>";
  page += "<p class='hint'>经典主题保留当前所有元素不动；新增主题使用重新设计的赛博风格图标与布局。</p>";

  page += "<label>WiFi SSID</label>";
  page += "<input id='ssid' name='ssid' value='" + htmlEscape(defaultSsid) + "' placeholder='可手动输入 SSID'>";

  page += "<button class='smallbtn' type='button' onclick=\"location.href='/rescan'\">扫描附近 Wi-Fi</button>";

  page += buildWiFiOptionsHtml();

  page += "<label>WiFi 密码</label>";
  page += "<div class='passwrap'><input id='wifiPass' name='wifiPass' type='password' value='' placeholder='第一次配网必须填写；后续留空则保留旧密码'><button class='eye' type='button' onclick=\"togglePass('wifiPass',this)\">👁</button></div>";

  page += "<label>NAS IP</label>";
  page += "<input name='nasIp' value='" + htmlEscape(String(cfg.nasIp)) + "' placeholder='输入你的 NAS IP'>";

  page += "<label>NAS 端口</label>";
  page += "<input name='nasPort' type='number' value='" + nasPortValue + "' placeholder='输入 NAS 端口，例如 8088'>";

  page += "<label>Token</label>";
  page += "<input name='token' value='" + htmlEscape(String(cfg.token)) + "' placeholder='输入 NAS 状态接口 Token'>";

  page += "<label>刷新间隔 秒</label>";
  page += "<input name='refreshSec' type='number' min='1' max='60' value='" + String(cfg.refreshSec) + "'>";


  page += "<div class='statusbox'>";
  page += "<b>屏幕开关状态：</b>";
  if (isDisplayOn()) page += "<span class='on'>开启</span>";
  else page += "<span class='off'>关闭</span>";
  page += "<br><span class='hint'>手动开关会立即生效；如果启用了定时开关，到达设定时间后会自动同步状态。</span>";
  page += "<br><span class='hint'>Home Assistant / Homebridge HTTP 接口：/api/display/on、/api/display/off、/api/display/status、/api/display/brightness?value=80</span>";
  page += "</div>";

  if (isDisplayOn()) page += "<button class='smallbtn' type='button' onclick=\"location.href='/displaytoggle'\">手动关闭屏幕</button>";
  else page += "<button type='button' onclick=\"location.href='/displaytoggle'\">手动开启屏幕</button>";

  page += "<label>定时开关屏</label>";
  page += "<select name='scheduleEnabled'>";
  page += "<option value='0'";
  if (cfg.scheduleEnabled != 1) page += " selected";
  page += ">关闭定时</option>";
  page += "<option value='1'";
  if (cfg.scheduleEnabled == 1) page += " selected";
  page += ">启用定时</option>";
  page += "</select>";
  page += "<div class='grid2'>";
  page += "<div><label>自动开启时间</label><input name='onTime' type='time' value='";
  if (cfg.onHour < 10) page += "0";
  page += String(cfg.onHour);
  page += ":";
  if (cfg.onMinute < 10) page += "0";
  page += String(cfg.onMinute);
  page += "'></div>";
  page += "<div><label>自动关闭时间</label><input name='offTime' type='time' value='";
  if (cfg.offHour < 10) page += "0";
  page += String(cfg.offHour);
  page += ":";
  if (cfg.offMinute < 10) page += "0";
  page += String(cfg.offMinute);
  page += "'></div>";
  page += "</div>";
  page += "<p class='hint'>定时使用北京时间。跨天时间支持，例如 22:00 开启，07:00 关闭。</p>";
  page += "<label>屏幕亮度 <span id='brightnessValue'>" + String(getDisplayBrightnessPercent()) + "%</span></label>";
  page += "<input name='brightness' type='range' min='1' max='100' value='" + String(getDisplayBrightnessPercent()) + "' oninput=\"document.getElementById('brightnessValue').innerText=this.value+'%';\">";
  page += "<p class='hint'>当前亮度：<span>" + String(getDisplayBrightnessPercent()) + "%</span>。Home Assistant 的亮度滑条会同步调用 HTTP 接口。</p>";

  page += "<label>Web 访问密码</label>";
  if (isWebAuthEnabled()) {
    page += "<div class='passwrap'><input id='webPass' name='webPass' type='password' value='' placeholder='已开启；留空不修改，输入新密码则更新'><button class='eye' type='button' onclick=\"togglePass('webPass',this)\">👁</button></div>";
  } else {
    page += "<div class='passwrap'><input id='webPass' name='webPass' type='password' value='' placeholder='留空不开启；输入密码后开启 Web 访问保护'><button class='eye' type='button' onclick=\"togglePass('webPass',this)\">👁</button></div>";
  }

  page += "<p class='hint'>Web 用户名：admin。恢复出厂设置会清除 Web 访问密码保护。</p>";
  page += "<p class='hint'>屏幕标题建议使用英文、数字。默认字体不支持中文，中文可能无法显示。</p>";

  page += "<button type='submit'>保存并重启</button>";
  page += "</form>";

  page += "<form method='GET' action='/update'>";
  page += "<button class='smallbtn' type='submit'>网页 OTA 上传固件</button>";
  page += "</form>";

  page += "<form method='GET' action='/clearwifi'>";
  page += "<button class='smallbtn' type='submit'>清除 Wi-Fi 信息</button>";
  page += "</form>";

  page += "<form method='GET' action='/openap'>";
  page += "<button class='smallbtn' type='submit'>开启 AP</button>";
  page += "</form>";

  page += "<form method='GET' action='/reboot'>";
  page += "<button class='smallbtn' type='submit'>重启设备</button>";
  page += "</form>";

  page += "<form method='GET' action='/reset'>";
  page += "<button class='smallbtn' type='submit'>恢复出厂设置</button>";
  page += "</form>";

  page += "<p class='hint'>当前 ESP IP: ";
  if (WiFi.getMode() & WIFI_STA) page += WiFi.localIP().toString();
  else page += WiFi.softAPIP().toString();
  page += "</p>";

  page += "<p class='hint'>Web OTA: http://";
  if (WiFi.getMode() & WIFI_STA) page += WiFi.localIP().toString();
  else page += WiFi.softAPIP().toString();
  page += "/update</p>";

  page += "<p class='hint'>NAS URL: http://";
  page += htmlEscape(String(cfg.nasIp));
  page += ":";
  page += cfg.nasPort > 0 ? String(cfg.nasPort) : "未设置";
  page += "/status?token=******</p>";

  page += "</body></html>";
  server.send(200, "text/html; charset=utf-8", page);
}

void handleUpdatePage() {
  if (!requireWebAuth()) return;

  String page = "";
  page += "<!doctype html><html><head><meta charset='utf-8'>";
  page += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>Web OTA</title>";
  page += "<style>";
  page += "body{font-family:Arial,'Microsoft YaHei',sans-serif;padding:20px;max-width:560px;margin:auto;background:#111;color:#eee}";
  page += "input{width:100%;padding:10px;margin:12px 0;box-sizing:border-box;border-radius:6px;border:1px solid #444;background:#222;color:#fff}";
  page += "button{width:100%;padding:12px;background:#0aa7ff;color:white;border:0;border-radius:6px;font-size:16px;margin-top:8px}";
  page += ".btnrow{display:grid;grid-template-columns:1fr 1fr;gap:10px}";
  page += ".clearbtn{background:#444}";
  page += ".hint{color:#aaa;font-size:13px;line-height:1.6}";
  page += ".bar{height:16px;background:#222;border:1px solid #444;border-radius:10px;overflow:hidden;margin:16px 0}";
  page += ".fill{height:100%;width:0%;background:#20c997;transition:width .15s}";
  page += ".pct{font-size:18px;margin:8px 0;color:#eee}";
  page += "</style></head><body>";
  page += "<h2>OTA固件更新</h2>";
  page += "<p class='hint'>选择 Arduino IDE 导出的 .bin 固件文件上传。上传期间网页和屏幕都会显示进度。</p>";
  page += "<input id='fw' type='file' accept='.bin'>";
  page += "<div class='btnrow'>";
  page += "<button type='button' onclick='uploadFw()'>Upload Firmware</button>";
  page += "<button class='clearbtn' type='button' onclick='clearFw()'>取消选择</button>";
  page += "</div>";
  page += "<div class='bar'><div id='fill' class='fill'></div></div>";
  page += "<div id='pct' class='pct'>0%</div>";
  page += "<p id='msg' class='hint'>等待选择固件。</p>";
  page += "<p class='hint'>OTA 上传不再单独设置密码；如果已开启 Web 访问保护，则进入本页面前需要 Web 登录。</p>";
  page += "<script>";
  page += "function clearFw(){";
  page += "var old=document.getElementById('fw');";
  page += "var n=old.cloneNode(true);";
  page += "n.value='';";
  page += "old.parentNode.replaceChild(n,old);";
  page += "document.getElementById('fill').style.width='0%';";
  page += "document.getElementById('pct').innerText='0%';";
  page += "document.getElementById('msg').innerText='已取消选择固件。';";
  page += "}";
  page += "function uploadFw(){";
  page += "var f=document.getElementById('fw').files[0];";
  page += "if(!f){alert('请选择 .bin 固件');return;}";
  page += "var fd=new FormData();fd.append('firmware',f);";
  page += "var x=new XMLHttpRequest();";
  page += "x.upload.onprogress=function(e){if(e.lengthComputable){var p=Math.round(e.loaded*100/e.total);document.getElementById('fill').style.width=p+'%';document.getElementById('pct').innerText=p+'%';document.getElementById('msg').innerText='正在上传，请勿断电。';}};";
  page += "x.onload=function(){document.getElementById('msg').innerHTML=x.responseText || '上传完成，设备将重启。';};";
  page += "x.onerror=function(){document.getElementById('msg').innerText='上传失败，请检查网络后重试。';};";
  page += "x.open('POST','/update?size='+encodeURIComponent(f.size),true);x.send(fd);";
  page += "}";
  page += "</script>";
  page += "</body></html>";
  server.send(200, "text/html; charset=utf-8", page);
}

void handleUpdateFinished() {
  if (!requireWebAuth()) return;

  bool ok = !Update.hasError();
  String page = "";
  page += "<!doctype html><html><head><meta charset='utf-8'>";
  page += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "</head><body>";
  page += ok ? "<h2>Update Success. Rebooting...</h2>" : "<h2>Update Failed.</h2>";
  page += "</body></html>";
  server.send(200, "text/html; charset=utf-8", page);

  if (ok) {
    delay(1200);
    ESP.restart();
  }
}

void handleUpdateUpload() {
  if (isWebAuthEnabled() && !server.authenticate(WEB_AUTH_USER, cfg.webPass)) return;

  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaInProgress = true;
    setDisplayPower(true, false);
    uiDrawn = false;
    WiFiUDP::stopAll();
    uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;

    otaExpectedSize = 0;
    if (server.hasArg("size")) {
      otaExpectedSize = server.arg("size").toInt();
    }
    if (otaExpectedSize == 0) {
      otaExpectedSize = server.header("Content-Length").toInt();
    }

    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(10, 45);
    tft.println("Web OTA");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(10, 85);
    tft.println("Starting...");

    Update.begin(maxSketchSpace);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    Update.write(upload.buf, upload.currentSize);
    static unsigned long lastDraw = 0;
    if (millis() - lastDraw > 200) {
      lastDraw = millis();
      int percent = 0;
      if (otaExpectedSize > 0) {
        percent = (int)((upload.totalSize * 100UL) / otaExpectedSize);
      } else {
        percent = (upload.totalSize / 4096) % 100;
      }
      if (percent > 100) percent = 100;
      if (percent < 0) percent = 0;
      int bar = 216 * percent / 100;

      tft.fillRect(10, 120, 220, 50, TFT_BLACK);
      tft.setTextSize(2);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.setCursor(10, 120);
      tft.printf("Uploading %d%%", percent);
      tft.drawRect(10, 155, 220, 12, TFT_DARKGREY);
      tft.fillRect(12, 157, 216, 8, TFT_BLACK);
      tft.fillRect(12, 157, bar, 8, TFT_GREEN);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      tft.fillScreen(TFT_BLACK);
      tft.setTextSize(2);
      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.setCursor(10, 70);
      tft.println("OTA Done");
      tft.setCursor(10, 105);
      tft.println("Rebooting...");
    } else {
      tft.fillScreen(TFT_BLACK);
      tft.setTextSize(2);
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.setCursor(10, 70);
      tft.println("OTA Failed");
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    otaInProgress = false;
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(10, 70);
    tft.println("OTA Aborted");
  }
  yield();
}

void copyArgAlways(const char *name, char *dest, size_t len) {
  String v = server.arg(name);
  v.trim();
  strncpy(dest, v.c_str(), len - 1);
  dest[len - 1] = 0;
}

void copyArgIfNotEmpty(const char *name, char *dest, size_t len) {
  String v = server.arg(name);
  v.trim();
  if (v.length() == 0) return;
  strncpy(dest, v.c_str(), len - 1);
  dest[len - 1] = 0;
}

void handleSave() {
  if (!requireWebAuth()) return;

  copyArgAlways("title", cfg.title, sizeof(cfg.title));
  if (strlen(cfg.title) == 0) {
    strcpy(cfg.title, "UGREEN NAS");
  }

  copyArgAlways("ssid", cfg.ssid, sizeof(cfg.ssid));
  copyArgIfNotEmpty("wifiPass", cfg.wifiPass, sizeof(cfg.wifiPass));
  copyArgAlways("nasIp", cfg.nasIp, sizeof(cfg.nasIp));
  copyArgAlways("token", cfg.token, sizeof(cfg.token));

  int port = server.arg("nasPort").toInt();
  if (port > 0 && port <= 65535) cfg.nasPort = (uint16_t)port;
  else cfg.nasPort = 0;

  int refresh = server.arg("refreshSec").toInt();
  if (refresh < 1) refresh = 3;
  if (refresh > 60) refresh = 60;
  cfg.refreshSec = (uint16_t)refresh;

  int schedule = server.arg("scheduleEnabled").toInt();
  cfg.scheduleEnabled = schedule == 1 ? 1 : 0;

  int theme = server.arg("theme").toInt();
  if (theme < THEME_CLASSIC || theme > THEME_CYBER_CPU_TEMP) theme = THEME_CLASSIC;
  cfg.theme = (uint8_t)theme;

  int brightness = server.arg("brightness").toInt();
  if (brightness < 1) brightness = getDisplayBrightnessPercent();
  if (brightness > 100) brightness = 100;
  cfg.brightness = (uint8_t)brightness;

  String onTime = server.arg("onTime");
  if (onTime.length() >= 5) {
    int h = onTime.substring(0, 2).toInt();
    int m = onTime.substring(3, 5).toInt();
    if (h >= 0 && h <= 23 && m >= 0 && m <= 59) {
      cfg.onHour = (uint8_t)h;
      cfg.onMinute = (uint8_t)m;
    }
  }

  String offTime = server.arg("offTime");
  if (offTime.length() >= 5) {
    int h = offTime.substring(0, 2).toInt();
    int m = offTime.substring(3, 5).toInt();
    if (h >= 0 && h <= 23 && m >= 0 && m <= 59) {
      cfg.offHour = (uint8_t)h;
      cfg.offMinute = (uint8_t)m;
    }
  }

  String webPass = server.arg("webPass");
  webPass.trim();
  if (webPass.length() > 0) {
    strncpy(cfg.webPass, webPass.c_str(), sizeof(cfg.webPass) - 1);
    cfg.webPass[sizeof(cfg.webPass) - 1] = 0;
    cfg.webAuthEnabled = 1;
  }

  applyBacklight();
  saveConfig();
  server.send(200, "text/html; charset=utf-8", "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'></head><body><h2>已保存，正在重启...</h2></body></html>");
  delay(1000);
  ESP.restart();
}

void handleResetConfig() {
  if (!requireWebAuth()) return;

  memset(&cfg, 0, sizeof(cfg));
  EEPROM.put(0, cfg);
  EEPROM.commit();
  server.send(200, "text/html; charset=utf-8", "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'></head><body><h2>已恢复出厂设置，正在重启...</h2></body></html>");
  delay(1000);
  ESP.restart();
}

void handleClearWiFi() {
  if (!requireWebAuth()) return;

  strcpy(cfg.ssid, "");
  strcpy(cfg.wifiPass, "");
  saveConfig();
  server.send(200, "text/html; charset=utf-8", "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'></head><body><h2>Wi-Fi 信息已清除，正在重启...</h2></body></html>");
  delay(1000);
  ESP.restart();
}

void handleOpenAp() {
  if (!requireWebAuth()) return;

  startConfigPortalNonBlocking();
  server.send(200, "text/html; charset=utf-8", "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'></head><body><h2>AP 已开启</h2><p>热点：NAS-Monitor-Setup</p><p>地址：http://192.168.4.1</p><p><a href='/'>返回配置页</a></p></body></html>");
}


void sendDisplayStatusJson() {
  String json = "{";
  json += "\"power\":";
  json += isDisplayOn() ? "true" : "false";
  json += ",\"state\":\"";
  json += isDisplayOn() ? "on" : "off";
  json += "\"";
  json += ",\"brightness\":";
  json += String(getDisplayBrightnessPercent());
  json += ",\"brightness_percent\":";
  json += String(getDisplayBrightnessPercent());
  json += ",\"schedule_enabled\":";
  json += cfg.scheduleEnabled == 1 ? "true" : "false";
  json += ",\"on_time\":\"";
  if (cfg.onHour < 10) json += "0";
  json += String(cfg.onHour);
  json += ":";
  if (cfg.onMinute < 10) json += "0";
  json += String(cfg.onMinute);
  json += "\"";
  json += ",\"off_time\":\"";
  if (cfg.offHour < 10) json += "0";
  json += String(cfg.offHour);
  json += ":";
  if (cfg.offMinute < 10) json += "0";
  json += String(cfg.offMinute);
  json += "\"";
  json += "}";
  server.send(200, "application/json; charset=utf-8", json);
}

void handleDisplayStatus() {
  if (!requireWebAuth()) return;
  sendDisplayStatusJson();
}


void handleDisplayBrightness() {
  if (!requireWebAuth()) return;

  if (!server.hasArg("value")) {
    server.send(400, "application/json; charset=utf-8", "{\"error\":\"missing value\"}");
    return;
  }

  int value = server.arg("value").toInt();
  if (value < 1) value = 1;
  if (value > 100) value = 100;

  setDisplayBrightnessPercent(value, true);
  sendDisplayStatusJson();
}

void handleDisplayOn() {
  if (!requireWebAuth()) return;
  setDisplayPower(true, true);
  if (server.hasArg("plain")) {
    server.send(200, "text/plain; charset=utf-8", "ON");
  } else {
    sendDisplayStatusJson();
  }
}

void handleDisplayOff() {
  if (!requireWebAuth()) return;
  setDisplayPower(false, true);
  if (server.hasArg("plain")) {
    server.send(200, "text/plain; charset=utf-8", "OFF");
  } else {
    sendDisplayStatusJson();
  }
}

void handleDisplayToggle() {
  if (!requireWebAuth()) return;

  setDisplayPower(!isDisplayOn(), true);
  if (server.hasArg("json")) {
    sendDisplayStatusJson();
    return;
  }
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleReboot() {
  if (!requireWebAuth()) return;

  server.send(200, "text/html; charset=utf-8", "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'></head><body><h2>正在重启...</h2></body></html>");
  delay(1000);
  ESP.restart();
}

void handleRescan() {
  if (!requireWebAuth()) return;

  drawMessage("Scanning WiFi", "Please wait...");
  scanWiFiNetworks();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleCaptivePortal() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
  server.send(302, "text/plain", "");
}

void startWebServer() {
  if (webServerStarted) return;

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/reset", HTTP_GET, handleResetConfig);
  server.on("/clearwifi", HTTP_GET, handleClearWiFi);
  server.on("/openap", HTTP_GET, handleOpenAp);
  server.on("/reboot", HTTP_GET, handleReboot);
  server.on("/displaytoggle", HTTP_GET, handleDisplayToggle);
  server.on("/display/on", HTTP_GET, handleDisplayOn);
  server.on("/display/off", HTTP_GET, handleDisplayOff);
  server.on("/display/status", HTTP_GET, handleDisplayStatus);
  server.on("/display/brightness", HTTP_GET, handleDisplayBrightness);
  server.on("/api/display/on", HTTP_GET, handleDisplayOn);
  server.on("/api/display/off", HTTP_GET, handleDisplayOff);
  server.on("/api/display/status", HTTP_GET, handleDisplayStatus);
  server.on("/api/display/brightness", HTTP_GET, handleDisplayBrightness);
  server.on("/rescan", HTTP_GET, handleRescan);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateFinished, handleUpdateUpload);

  server.on("/generate_204", HTTP_GET, handleCaptivePortal);
  server.on("/gen_204", HTTP_GET, handleCaptivePortal);
  server.on("/hotspot-detect.html", HTTP_GET, handleRoot);
  server.on("/library/test/success.html", HTTP_GET, handleRoot);
  server.on("/ncsi.txt", HTTP_GET, handleCaptivePortal);
  server.on("/connecttest.txt", HTTP_GET, handleCaptivePortal);
  server.on("/fwlink", HTTP_GET, handleCaptivePortal);

  server.onNotFound(handleCaptivePortal);
  server.begin();
  webServerStarted = true;
}

void startConfigPortalNonBlocking() {
  if (apStarted) return;
  apStarted = true;
  configMode = true;
  uiDrawn = false;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  drawMessage("Setup Mode", "WiFi:", AP_SSID, "STA retry active");
  startWebServer();
}

void stopConfigPortalIfRunning() {
  if (!apStarted) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  apStarted = false;
  configMode = false;
  WiFi.mode(WIFI_STA);
}

void setupArduinoOTA() {
  if (otaStarted) return;

  ArduinoOTA.setHostname(OTA_HOSTNAME);

  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    setDisplayPower(true, false);
    uiDrawn = false;
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(10, 50);
    tft.println("IDE OTA");
    tft.setCursor(10, 85);
    tft.println("Starting...");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    int percent = (progress * 100) / total;
    drawOtaProgress(percent);
  });

  ArduinoOTA.onEnd([]() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(10, 70);
    tft.println("OTA Done");
    tft.setCursor(10, 105);
    tft.println("Rebooting...");
  });

  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(10, 60);
    tft.println("OTA Error");
  });

  ArduinoOTA.begin();
  otaStarted = true;
}

void startNormalServicesOnce() {
  if (normalServicesStarted) return;
  stopConfigPortalIfRunning();

  wifiState = WIFI_STATE_CONNECTED;
  normalServicesStarted = true;
  configMode = false;
  uiDrawn = false;

  setupTimeOnce();
  applyDisplaySchedule(true);

  if (isDisplayOn()) drawMessage("WiFi OK", WiFi.localIP().toString(), "Web Config:", WiFi.localIP().toString());
  delay(800);

  setupArduinoOTA();
  startWebServer();
  fetchStatus();
  lastFetch = millis();
}

void startWifiAttempt() {
  if (apStarted) WiFi.mode(WIFI_AP_STA);
  else WiFi.mode(WIFI_STA);

  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false);
  delay(50);
  WiFi.begin(cfg.ssid, cfg.wifiPass);

  wifiAttemptStartMs = millis();
  lastWifiStatusDrawMs = 0;
  wifiState = apStarted ? WIFI_STATE_AP_STA_RETRY : WIFI_STATE_CONNECTING;
}

void beginWifiStateMachine() {
  wifiBootStartMs = millis();
  wifiAttemptStartMs = 0;
  wifiRetryWaitStartMs = 0;
  lastWifiStatusDrawMs = 0;
  lastApStaRetryMs = 0;
  apStarted = false;
  normalServicesStarted = false;
  wifiState = WIFI_STATE_IDLE;

  if (strlen(cfg.ssid) == 0) {
    startConfigPortalNonBlocking();
    wifiState = WIFI_STATE_AP_STA_RETRY;
    return;
  }

  drawMessage("Connecting WiFi", cfg.ssid, "20s window");
  startWifiAttempt();
}

void drawConnectingStatus(const String &line3) {
  unsigned long now = millis();
  if (lastWifiStatusDrawMs != 0 && now - lastWifiStatusDrawMs < WIFI_STATUS_REFRESH_MS) return;
  lastWifiStatusDrawMs = now;

  unsigned long elapsedTotal = now - wifiBootStartMs;
  unsigned long remainToAp = 0;
  if (!apStarted && elapsedTotal < WIFI_AP_DELAY_MS) remainToAp = (WIFI_AP_DELAY_MS - elapsedTotal) / 1000;

  if (!apStarted) drawMessage("Connecting WiFi", cfg.ssid, line3, String(remainToAp) + "s to AP");
  else drawMessage("Setup Mode", AP_SSID, line3, cfg.ssid);
}

void handleWifiState() {
  if (WiFi.status() == WL_CONNECTED) {
    startNormalServicesOnce();
    return;
  }

  unsigned long now = millis();

  if (strlen(cfg.ssid) == 0) {
    if (!apStarted) startConfigPortalNonBlocking();
    return;
  }

  if (!apStarted && now - wifiBootStartMs >= WIFI_AP_DELAY_MS) {
    startConfigPortalNonBlocking();
    lastApStaRetryMs = 0;
  }

  if (wifiState == WIFI_STATE_CONNECTING) {
    drawConnectingStatus("Connecting...");
    if (now - wifiAttemptStartMs >= WIFI_CONNECT_WINDOW_MS) {
      WiFi.disconnect(false);
      wifiRetryWaitStartMs = now;
      wifiState = WIFI_STATE_RETRY_WAIT;
      drawConnectingStatus("Retry wait...");
    }
    return;
  }

  if (wifiState == WIFI_STATE_RETRY_WAIT) {
    drawConnectingStatus("Retry wait...");
    if (now - wifiRetryWaitStartMs >= WIFI_RETRY_WAIT_MS) startWifiAttempt();
    return;
  }

  if (wifiState == WIFI_STATE_AP_STA_RETRY) {
    if (!apStarted) startConfigPortalNonBlocking();
    if (lastApStaRetryMs == 0 || now - lastApStaRetryMs >= WIFI_AP_RETRY_MS) {
      lastApStaRetryMs = now;
      startWifiAttempt();
    }
    drawConnectingStatus("STA retry...");
    return;
  }

  if (wifiState == WIFI_STATE_IDLE) startWifiAttempt();
}

String buildStatusUrl() {
  String url = "http://";
  url += cfg.nasIp;
  url += ":";
  url += String(cfg.nasPort);
  url += "/status?token=";
  url += cfg.token;
  return url;
}

void fetchStatus() {
  if (otaInProgress) return;

  if (WiFi.status() != WL_CONNECTED) {
    uiDrawn = false;
    normalServicesStarted = false;
    wifiBootStartMs = millis();
    wifiState = WIFI_STATE_IDLE;
    drawMessage("WiFi Lost", "Restart WiFi", cfg.ssid);
    startWifiAttempt();
    return;
  }

  if (strlen(cfg.nasIp) == 0 || cfg.nasPort == 0 || strlen(cfg.token) == 0) {
    uiDrawn = false;
    drawMessage("NAS Config", "Incomplete", "Open Web Config", WiFi.localIP().toString());
    return;
  }

  WiFiClient client;
  HTTPClient http;
  String url = buildStatusUrl();

  if (!http.begin(client, url)) {
    uiDrawn = false;
    drawMessage("HTTP Begin", "Failed");
    return;
  }

  http.setTimeout(1500);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<1536> doc;
    DeserializationError err = deserializeJson(doc, payload);

    if (err) {
      uiDrawn = false;
      drawMessage("JSON Error", err.c_str());
    } else {
      int cpu = doc["cpu"] | 0;
      int mem = doc["mem"] | 0;
      int disk = doc["disk"] | 0;
      int temp = doc["temp"] | 0;
      String down = doc["down"] | "-";
      String up = doc["up"] | "-";
      String ip = WiFi.localIP().toString();
      cacheStatusSnapshot(cpu, mem, disk, temp, down, up, ip);
      drawStatus(cpu, mem, disk, temp, down, up, ip);
      processIncomingNotice(doc["event"]);
      if (noticeBannerActive) drawNoticeBanner();
    }
  } else if (code == 401) {
    uiDrawn = false;
    drawMessage("Token Error", "Unauthorized");
  } else {
    uiDrawn = false;
    drawMessage("HTTP Error", String(code));
  }

  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(LCD_BL_PIN, OUTPUT);

  tft.init();
  tft.setRotation(0);

  analogWriteRange(1023);
  analogWriteFreq(10000);  // 频率，肉眼可见屏幕闪烁时可调低，例如 5000 或 1000

  tft.fillScreen(TFT_BLACK);

  loadConfig();

  // 修复：上一次如果手动关闭了屏幕，重启后必须立刻恢复关闭状态。
  // 否则启动阶段会先点亮并显示 Connecting WiFi，看起来像卡住。
  setDisplayPower(isDisplayOn(), false);

  beginWifiStateMachine();
}

void loop() {
  handleWifiState();

  if (apStarted) dnsServer.processNextRequest();
  if (webServerStarted) server.handleClient();
  if (normalServicesStarted && WiFi.status() == WL_CONNECTED) ArduinoOTA.handle();

  if (!normalServicesStarted || otaInProgress) return;

  applyDisplaySchedule(false);
  drawClock(false);
  handleNoticeBannerTimeout();

  unsigned long interval = max((uint16_t)1, cfg.refreshSec) * 1000UL;
  if (millis() - lastFetch >= interval) {
    lastFetch = millis();
    fetchStatus();
  }
}
