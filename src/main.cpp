// gencord ADV: USB-only, 16 kHz PCM16 stereo. L/R are never mixed.
// Files and configuration are isolated under /gencord-adv; uploaded originals stay on SD.
// Build-only artifact: do not flash over Launcher/Nucleo/Switch Bridge partitions.

#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <base64.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <freertos/queue.h>
#include <unistd.h>
#include <Preferences.h>
#include <sys/time.h>
#include <vector>
#include "usb_mic.h"

// Settings come from /gencord-adv/recorder.cfg on the SD card (editable on the device with 'c').
// config.h (optional, gitignored) only provides defaults for a private build.
#if __has_include("config.h")
#include "config.h"
#endif
#define GENCORD_VERSION "ADV-dual-1"  // shown on the menu cassette; raise with every flash

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef DAV_URL
#define DAV_URL ""
#endif
#ifndef DAV_USER
#define DAV_USER ""
#endif
#ifndef DAV_PASS
#define DAV_PASS ""
#endif
#ifndef TZ_INFO
#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3"
#endif
#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif
#ifndef SAMPLE_RATE
#define SAMPLE_RATE 16000
#endif
#ifndef WIFI_CHECK_MIN
#define WIFI_CHECK_MIN 20
#endif
#ifndef IDLE_SLEEP_S
#define IDLE_SLEEP_S 60
#endif
#ifndef MENU_SCREEN_S
#define MENU_SCREEN_S 20  // menu: screen off after N s without input (device stays awake)
#endif
#ifndef NAME_TIMEOUT_S
#define NAME_TIMEOUT_S 30
#endif
#ifndef LOW_BATT_WARN_MV
#define LOW_BATT_WARN_MV 3550
#endif
#ifndef LOW_BATT_MV
#define LOW_BATT_MV 3400
#endif
#ifndef SCREEN_BRIGHTNESS
#define SCREEN_BRIGHTNESS 60
#endif

struct Settings {
  String ssid = WIFI_SSID, pass = WIFI_PASS;
  String davUrl = DAV_URL, davUser = DAV_USER, davPass = DAV_PASS;
  String tz = TZ_INFO, ntp = NTP_SERVER;
  int wifiCheckMin = WIFI_CHECK_MIN;
} S;
static constexpr const char* CFG_PATH = "/gencord-adv/recorder.cfg";

// ---------------------------------------------------------------- hardware
static constexpr int PIN_SD_SCK = 40, PIN_SD_MISO = 39, PIN_SD_MOSI = 14, PIN_SD_CS = 12;
static constexpr gpio_num_t PIN_BTN = GPIO_NUM_0;
// ADV has TCA8418 keyboard and codec: never drive legacy matrix/audio pins.
static constexpr gpio_num_t HOLD_LOW_PINS[] = {GPIO_NUM_38};

static constexpr const char* REC_DIR = "/gencord-adv/recordings";
static constexpr const char* LOG_PATH = "/gencord-adv/log.csv";
static constexpr size_t LOG_MAX_BYTES = 256 * 1024;

// ---------------------------------------------------------------- audio buffers
// 12 x 125 ms stereo = 96 KB fixed PCM pool; overflow ends capture with an error.
static_assert(SAMPLE_RATE == gencord::OutputRate, "USB output is fixed at 16 kHz");
static constexpr size_t BUF_SAMPLES = SAMPLE_RATE / 4;
static constexpr int NBUF = 12;
static int16_t bufs[NBUF][BUF_SAMPLES];
static QueueHandle_t filledQ, freeQ;


// ---------------------------------------------------------------- state kept in deep sleep
RTC_DATA_ATTR static uint32_t rtcBoots = 0;
RTC_DATA_ATTR static uint16_t rtcPending = 0;
RTC_DATA_ATTR static time_t rtcLastSync = 0;
RTC_DATA_ATTR static int8_t rtcLastSyncOk = -1;  // -1 never, 0 failed, 1 ok
RTC_DATA_ATTR static uint8_t rtcWifiCh = 0;
RTC_DATA_ATTR static uint8_t rtcWifiBssid[6];
RTC_DATA_ATTR static uint8_t rtcScanMiss = 0;

static bool sdOk = false;
static bool screenIsOn = false;

// ================================================================ small helpers
static bool timeValid() { return time(nullptr) > 1704067200; }  // after 2024-01-01

static String timeStr(const char* fmt) {
  if (!timeValid()) return "--";
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char b[40];
  strftime(b, sizeof(b), fmt, &t);
  return b;
}

// The clock survives deep sleep, but not the power switch. The last known time is
// kept in NVS and restored at power-on, so it is at least approximate until the
// next NTP sync (shown as "~" in the menu).
RTC_DATA_ATTR static bool rtcTimeExact = false;  // NTP or manually set since power-on

static void saveClock() {
  if (!timeValid()) return;
  Preferences p;
  p.begin("gencord-adv", false);
  p.putLong64("t", (int64_t)time(nullptr));
  p.end();
}

static bool restoreClock() {
  if (timeValid()) return false;
  Preferences p;
  p.begin("gencord-adv", true);
  int64_t t = p.getLong64("t", 0);
  p.end();
  if (t < 1704067200) return false;
  struct timeval tv = {(time_t)t, 0};
  settimeofday(&tv, nullptr);
  rtcTimeExact = false;
  return true;
}

static int battMv() { return M5.Power.getBatteryVoltage(); }
static int battPct() { return M5.Power.getBatteryLevel(); }

static void logEvent(const char* ev, const String& detail = "") {
  if (!sdOk) return;
  File f = SD.open(LOG_PATH, FILE_APPEND);
  if (!f) return;
  if (f.size() > LOG_MAX_BYTES) {
    f.close();
    SD.remove(LOG_PATH);
    f = SD.open(LOG_PATH, FILE_APPEND);
    if (!f) return;
  }
  if (f.size() == 0) f.println("time,boot,event,batt_mv,detail");
  f.printf("%s,%lu,%s,%d,%s\n", timeStr("%Y-%m-%d %H:%M:%S").c_str(), (unsigned long)rtcBoots, ev, battMv(),
           detail.c_str());
  f.close();
}

// ================================================================ screen
static void screenOn() {
  if (screenIsOn) return;
  M5.Display.wakeup();
  M5.Display.setBrightness(SCREEN_BRIGHTNESS);
  screenIsOn = true;
}

static void screenOff() {
  M5.Display.setBrightness(0);
  M5.Display.sleep();
  screenIsOn = false;
}

// Frames are composed off-screen (8-bit, 32 KB) and pushed in one go: no black flash
// between redraws. Identical frames are skipped altogether.
static M5Canvas canvas(&M5.Display);
static String lastFrame;

// Wi-Fi state for the header icon: 0 = radio off (no icon), 1 = searching/connecting
// (grey), 2 = connected (white).
static int wifiState() {
  if (WiFi.getMode() == WIFI_OFF) return 0;
  return WiFi.status() == WL_CONNECTED ? 2 : 1;
}

// Classic Wi-Fi fan: dot plus three arcs, (cx, cy) = the dot.
static void wifiIcon(LovyanGFX& d, int cx, int cy) {
  const int st = wifiState();
  if (!st) return;
  const uint16_t c = st == 2 ? TFT_WHITE : TFT_DARKGREY;
  d.fillCircle(cx, cy, 2, c);
  for (int r = 4; r <= 12; r += 4) d.fillArc(cx, cy, r, r - 2, 225, 315, c);  // 2 px arcs, 2 px gaps
}

static bool micUsb = false;  // the running recording comes from the USB microphone

// Wireless mic (DJI receiver on USB-C): capsule on a stand plus two radio waves.
static void micIcon(LovyanGFX& d, int cx, int cy) {
  if (!micUsb) return;
  const uint16_t c = TFT_CYAN;
  d.fillRoundRect(cx - 3, cy - 9, 7, 11, 3, c);
  d.drawArc(cx, cy - 2, 6, 5, 0, 180, c);
  d.drawFastVLine(cx, cy + 4, 3, c);
  d.drawFastHLine(cx - 3, cy + 7, 7, c);
  d.drawArc(cx, cy - 4, 10, 9, 300, 345, c);
  d.drawArc(cx, cy - 4, 10, 9, 195, 240, c);
}

static void ui(const String& title, uint16_t color, const String& l1 = "", const String& l2 = "",
               const String& l3 = "", const String& l4 = "") {
  String frame = title + '\x1f' + color + '\x1f' + l1 + '\x1f' + l2 + '\x1f' + l3 + '\x1f' + l4 + wifiState() + micUsb;
  if (frame == lastFrame) return;
  lastFrame = frame;
  if (!canvas.getBuffer()) {
    canvas.setColorDepth(8);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
  }
  const bool buffered = canvas.getBuffer() != nullptr;
  LovyanGFX& d = buffered ? (LovyanGFX&)canvas : (LovyanGFX&)M5.Display;
  d.startWrite();
  d.fillScreen(TFT_BLACK);
  d.setTextColor(color, TFT_BLACK);
  d.setTextSize(title.length() > 13 ? 2 : 3);  // 240 px: 13 chars at size 3, 20 at size 2
  d.setCursor(4, 6);
  d.print(title);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextSize(1.5f);  // 26 chars per line
  const String* lines[] = {&l1, &l2, &l3, &l4};
  for (int i = 0; i < 4; i++) {
    d.setCursor(4, 40 + i * 23);
    d.print(*lines[i]);
  }
  wifiIcon(d, 226, 15);
  micIcon(d, 206, 15);
  d.endWrite();
  if (buffered) canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------- cassette screens
static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static LovyanGFX& frameBegin() {
  if (!canvas.getBuffer()) {
    canvas.setColorDepth(8);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
  }
  LovyanGFX& d = canvas.getBuffer() ? (LovyanGFX&)canvas : (LovyanGFX&)M5.Display;
  d.startWrite();
  d.fillScreen(TFT_BLACK);
  return d;
}

static void frameEnd(LovyanGFX& d) {
  d.endWrite();
  if (&d == (LovyanGFX*)&canvas) canvas.pushSprite(0, 0);
  lastFrame = "";  // the next ui() call must redraw
}

static void reel(LovyanGFX& d, int cx, int cy, int tapeR, float ang) {
  d.fillCircle(cx, cy, tapeR, rgb(92, 54, 30));       // wound tape
  d.drawCircle(cx, cy, tapeR, rgb(130, 80, 45));
  d.fillCircle(cx, cy, 10, rgb(236, 236, 226));       // white hub
  d.fillCircle(cx, cy, 4, rgb(30, 30, 34));
  for (int i = 0; i < 6; i++) {                       // drive teeth
    float a = ang + i * (float)M_PI / 3;
    d.drawWideLine(cx + cosf(a) * 4, cy + sinf(a) * 4, cx + cosf(a) * 9, cy + sinf(a) * 9, 1.2f, rgb(30, 30, 34));
  }
}

// A compact-cassette: label sticker with text, a window with two reels
// (progress 0..1 moves tape from the left reel to the right), head notch below.
static void cassette(LovyanGFX& d, int x, int y, int w, int h, const String& label, uint16_t stripe, float ang,
                     float progress) {
  const uint16_t body = rgb(48, 48, 56), edge = rgb(120, 120, 132);
  d.fillRoundRect(x, y, w, h, 8, body);
  d.drawRoundRect(x, y, w, h, 8, edge);
  for (int sx : {x + 6, x + w - 7})
    for (int sy : {y + 6, y + h - 7}) d.fillCircle(sx, sy, 2, edge);  // screws

  const int lx = x + 12, ly = y + 5, lw = w - 24, lh = h - 30;
  d.fillRoundRect(lx, ly, lw, lh, 5, rgb(250, 238, 204));
  d.fillRect(lx, ly + 17, lw, 4, stripe);
  d.fillRect(lx, ly + 23, lw, 2, rgb(240, 160, 60));
  // Name in bold, like a felt-tip on a tape label; long names show their end.
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextSize(1);
  d.setTextColor(rgb(25, 20, 20));
  d.setTextDatum(textdatum_t::top_left);
  String shown = label;
  while (shown.length() > 1 && d.textWidth(shown) > lw - 10) shown.remove(0, 1);
  d.drawString(shown, lx + 5, ly + 1);
  d.setFont(&fonts::Font0);

  const int wh = lh - 30, wy = ly + 28, ww = w / 2, wx = x + (w - ww) / 2;
  d.fillRoundRect(wx, wy, ww, wh, wh / 2, rgb(18, 18, 22));
  d.setClipRect(wx, wy, ww, wh);
  const int r0 = 11, r1 = wh / 2 + 8;
  const int cy = wy + wh / 2, cl = wx + wh / 2 + 2, cr = wx + ww - wh / 2 - 2;
  reel(d, cl, cy, r0 + (r1 - r0) * (1 - progress), ang);
  reel(d, cr, cy, r0 + (r1 - r0) * progress, ang);
  d.clearClipRect();

  const int by = y + h - 14, bw = w / 2;  // head area
  d.fillRect(x + (w - bw) / 2, by, bw, 14, rgb(72, 72, 82));
  d.fillTriangle(x + (w - bw) / 2 - 10, y + h - 1, x + (w - bw) / 2, by, x + (w - bw) / 2, y + h - 1, rgb(72, 72, 82));
  d.fillTriangle(x + (w + bw) / 2 + 10, y + h - 1, x + (w + bw) / 2, by, x + (w + bw) / 2, y + h - 1, rgb(72, 72, 82));
  for (int hx : {x + w / 2 - 22, x + w / 2 + 22}) d.fillCircle(hx, by + 7, 3, body);
}

static void footer(LovyanGFX& d, const String& a, const String& b) {
  d.setTextSize(1);
  d.setTextColor(rgb(200, 200, 200));
  d.setCursor(4, 116);
  d.print(a);
  d.setTextColor(rgb(140, 140, 150));
  d.setCursor(4, 126);
  d.print(b);
}

static int32_t levelPeak[2] = {0, 0};
static int32_t recPeak = 0;
static void measureLevels(const int16_t* p, size_t samples) {
  levelPeak[0] = levelPeak[1] = 0;
  for (size_t i = 0; i < samples; ++i) {
    const int32_t a = abs((int32_t)p[i]);
    if (a > levelPeak[i & 1]) levelPeak[i & 1] = a;
    if (a > recPeak) recPeak = a;
  }
}
static void levelMeter(LovyanGFX& d, int x, int y, int w, int h, int channel) {
  const float db = levelPeak[channel] ? 20.0f * log10f(levelPeak[channel] / 32768.0f) : -96.0f;
  const int lit = constrain((int)((db + 48.0f) / 48.0f * 16), 0, 16);
  for (int i = 0; i < 16; ++i) {
    const uint16_t col = i >= 14 ? TFT_RED : (i >= 11 ? TFT_YELLOW : TFT_GREEN);
    d.fillRect(x, y + h - (i + 1) * (h / 16), w, h / 16 - 1, i < lit ? col : TFT_DARKGREY);
  }
  d.setTextSize(1); d.setTextColor(TFT_WHITE); d.setCursor(x, y+h+4); d.print(channel ? "R" : "L");
}

static void recScreen(uint32_t elapsedMs, const String& label, float mb, int gaps) {
  LovyanGFX& d = frameBegin();
  uint32_t s = elapsedMs / 1000;
  char el[16];
  snprintf(el, sizeof(el), "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
  if ((millis() / 500) % 2) d.fillCircle(10, 10, 6, TFT_RED);
  d.setTextSize(2);
  d.setTextColor(TFT_RED);
  d.setCursor(22, 3);
  d.print("REC ");
  d.setTextColor(TFT_WHITE);
  d.print(el);
  d.setCursor(186, 3);
  d.print(String(battPct()) + "%");
  wifiIcon(d, 174, 14);
  micIcon(d, 160, 14);
  // one tape side = 60 min; the reels turn about once per 2 s like a real deck
  float progress = (s % 3600) / 3600.0f;
  cassette(d, 4, 22, 186, 92, label, TFT_RED, -(millis() % 100000) * 0.0031f, progress);
  levelMeter(d, 201, 22, 12, 80, 0);
  levelMeter(d, 221, 22, 12, 80, 1);
  footer(d, String(mb, 1) + " MB" + (gaps ? "  gaps " + String(gaps) : "") + "   type = name",
         "double-click G0 = stop");
  frameEnd(d);
}

static void menuScreen(const String& header, uint16_t headerColor, const String& label, const String& info,
                       const String& keys) {
  LovyanGFX& d = frameBegin();
  d.setTextSize(2);
  d.setTextColor(headerColor);
  d.setCursor(4, 3);
  d.print(header);
  d.setTextColor(TFT_WHITE);
  d.setCursor(186, 3);
  d.print(String(battPct()) + "%");
  wifiIcon(d, 174, 14);
  cassette(d, 27, 22, 186, 92, label, rgb(60, 140, 220), 0, 0);
  footer(d, info, keys);
  frameEnd(d);
}

// Upload: small tape -> moving dots -> NAS box, and a bar that fills up.
static void uploadScreen(const String& name, size_t sent, size_t total, int idx, int count) {
  LovyanGFX& d = frameBegin();
  const float frac = total ? (float)sent / total : 0;
  d.setTextSize(2);
  d.setTextColor(TFT_YELLOW);
  d.setCursor(4, 3);
  d.print("UPLOAD " + String(idx) + "/" + String(count));
  d.setTextColor(TFT_WHITE);
  d.setCursor(186, 3);
  d.print(String(battPct()) + "%");
  wifiIcon(d, 174, 14);

  // tape
  d.fillRoundRect(8, 30, 64, 40, 5, rgb(48, 48, 56));
  d.fillRoundRect(14, 34, 52, 16, 3, rgb(250, 238, 204));
  const float ang = millis() * 0.004f;
  for (int cx : {26, 54}) {
    d.fillCircle(cx, 59, 7, rgb(236, 236, 226));
    for (int i = 0; i < 3; i++) {
      float a = ang + i * 2.094f;
      d.drawLine(cx, 59, cx + cosf(a) * 6, 59 + sinf(a) * 6, rgb(30, 30, 34));
    }
  }
  // moving dots
  const int phase = (millis() / 120) % 6;
  for (int i = 0; i < 6; i++) d.fillCircle(84 + i * 12, 50, i == phase ? 3 : 2, i == phase ? TFT_YELLOW : rgb(90, 90, 90));
  // NAS: two drive bays and a blinking activity LED
  d.fillRoundRect(160, 26, 70, 48, 5, rgb(35, 35, 40));
  d.drawRoundRect(160, 26, 70, 48, 5, rgb(120, 120, 132));
  for (int bay : {32, 50}) d.fillRoundRect(167, bay, 46, 13, 2, rgb(70, 70, 80));
  d.fillCircle(221, 38, 2, (millis() / 200) % 2 ? rgb(40, 200, 255) : rgb(20, 60, 80));
  d.fillCircle(221, 56, 2, TFT_GREEN);

  // progress bar
  d.drawRoundRect(8, 82, 224, 16, 4, rgb(150, 150, 160));
  const int w = (int)(220 * frac);
  if (w > 0) d.fillRoundRect(10, 84, w, 12, 3, frac >= 1 ? TFT_GREEN : rgb(60, 140, 220));
  d.setTextSize(1);
  d.setTextColor(TFT_WHITE);
  d.setTextDatum(textdatum_t::middle_center);
  d.drawString(String((int)(frac * 100)) + " %", 120, 90);
  d.setTextDatum(textdatum_t::top_left);

  String n = name;
  n.replace(".wav", "");
  footer(d, n.length() > 40 ? n.substring(0, 40) : n,
         String(sent / 1048576.0f, 1) + " / " + String(total / 1048576.0f, 1) + " MB   G0 = stop & record");
  frameEnd(d);
}

// ================================================================ SD + WAV
static bool sdBegin() {
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS, SPI, 25000000)) return false;
  if (!SD.exists("/gencord-adv") && !SD.mkdir("/gencord-adv")) return false;
  return SD.exists(REC_DIR) || SD.mkdir(REC_DIR);
}

static void sdEnd() {
  SD.end();
  SPI.end();
}

static bool writeWavHeader(File& f, uint32_t dataBytes) {
  uint8_t h[44];
  return gencord::wavHeader(h, dataBytes) && f.seek(0) && f.write(h, sizeof(h)) == sizeof(h);
}
static String uploadedMarker(const String& name) { return String(REC_DIR) + "/" + name + ".uploaded"; }
// Actual FAT commit/rename and marker persistence still need physical SD qualification.
static bool isUploaded(const String& name, uint32_t bytes) {
  File marker = SD.open(uploadedMarker(name), FILE_READ);
  if (!marker || marker.size() > 32) { marker.close(); return false; }
  const String value = marker.readStringUntil('\n'); marker.close();
  return bytes > 44 && (uint32_t)value.toInt() == bytes;
}
static bool trimFile(const String& path, uint32_t bytes) {
  const String full = String(SD.mountpoint()) + path;
  return truncate(full.c_str(), bytes) == 0;
}

// Recovery repairs/trims the stereo file BEFORE publishing it as a finished WAV.
// Failed/empty parts remain on SD, never masquerading as a valid upload.
static std::vector<String> listRecordings(bool recover) {
  std::vector<String> done, parts;
  File dir = SD.open(REC_DIR);
  if (!dir) return done;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String n = f.name();
    if (!f.isDirectory()) {
      if (n.endsWith(".wav") && !isUploaded(n, f.size())) done.push_back(n);
      else if (n.endsWith(".wav.part")) parts.push_back(n);
    }
    f.close();
  }
  dir.close();
  if (!recover) return done;
  for (auto& p : parts) {
    const String path = String(REC_DIR) + "/" + p;
    File f = SD.open(path, "r+");
    if (!f || f.size() <= 44 || f.size() - 44 > gencord::MaxDataBytes) { f.close(); continue; }
    const uint32_t bytes = ((f.size() - 44) / gencord::FrameBytes) * gencord::FrameBytes;
    const bool headerOk = bytes && writeWavHeader(f, bytes);
    const bool needsTrim = f.size() != 44 + bytes;
    f.close();
    if (!headerOk || (needsTrim && !trimFile(path, 44 + bytes))) { logEvent("recover_failed", p); continue; }
    String fin = p.substring(0, p.length() - 9) + "_recovered.wav";
    if (SD.exists(String(REC_DIR) + "/" + fin) || !SD.rename(path, String(REC_DIR) + "/" + fin)) continue;
    done.push_back(fin); logEvent("recovered", fin);
  }
  return done;
}

// ================================================================ keyboard text input
enum class Input { Digits, Name, Any };

static bool accepts(Input mode, char c) {
  switch (mode) {
    case Input::Digits: return isdigit((unsigned char)c);
    case Input::Name: return isalnum((unsigned char)c) || c == '-' || c == '_';
    default: return c >= 32 && c <= 126;
  }
}

// Line editor. Enter/G0 or timeout accept; Esc (the ` key, top left) cancels and
// sets *escaped. `hint` is shown right below the input, e.g. the expected format.
static String textInput(const String& prompt, uint32_t timeoutMs, Input mode, size_t maxLen,
                        const String& initial = "", const String& hint = "", bool* escaped = nullptr) {
  String s = initial;
  uint32_t last = millis(), lastDraw = 0;
  bool dirty = true;
  if (escaped) *escaped = false;
  for (;;) {
    M5Cardputer.update();
    auto& kb = M5Cardputer.Keyboard;
    if (kb.isChange() && kb.isPressed()) {
      auto& ks = kb.keysState();
      last = millis();
      if (ks.enter) break;
      if (!ks.word.empty() && ks.word[0] == '`') {  // Esc
        if (escaped) *escaped = true;
        return initial;
      }
      if (ks.del && s.length()) s.remove(s.length() - 1);
      for (char c : ks.word) {
        if (c == ' ' && mode == Input::Name) c = '_';
        if (s.length() < maxLen && accepts(mode, c)) s += c;
      }
      dirty = true;
    }
    if (M5.BtnA.wasClicked()) break;
    if (millis() - last > timeoutMs) break;
    if (dirty || millis() - lastDraw > 1000) {
      uint32_t left = (timeoutMs - (millis() - last)) / 1000;
      String shown = s.length() > 25 ? s.substring(s.length() - 25) : s;  // tail of long URLs
      ui(prompt, TFT_CYAN, shown + "_", hint, "Enter=OK Esc=back Del=del", "auto in " + String(left) + "s");
      dirty = false;
      lastDraw = millis();
    }
    delay(10);
  }
  return s;
}

// Date and time are separate fields, prefilled with the current values: Enter
// keeps them, so usually only the clock time needs typing.
static void setTimeManually() {
  time_t now = time(nullptr);
  struct tm cur;
  localtime_r(&now, &cur);
  char d0[12], t0[8];
  strftime(d0, sizeof(d0), "%d%m%Y", &cur);
  strftime(t0, sizeof(t0), "%H%M", &cur);
  bool esc = false;
  String d = textInput("Date", 60000, Input::Digits, 8, timeValid() ? d0 : "", "format DDMMYYYY (17102026)", &esc);
  if (esc) return;
  String hm = textInput("Time", 60000, Input::Digits, 4, timeValid() ? t0 : "", "format HHMM (2200 = 22:00)", &esc);
  if (esc) return;
  if (d.length() != 8 || hm.length() != 4) {
    ui("TIME", TFT_RED, "need DDMMYYYY and HHMM", "unchanged");
    delay(1500);
    return;
  }
  struct tm t = {};
  t.tm_mday = d.substring(0, 2).toInt();
  t.tm_mon = d.substring(2, 4).toInt() - 1;
  t.tm_year = d.substring(4, 8).toInt() - 1900;
  t.tm_hour = hm.substring(0, 2).toInt();
  t.tm_min = hm.substring(2, 4).toInt();
  t.tm_isdst = -1;
  if (t.tm_mday < 1 || t.tm_mday > 31 || t.tm_mon < 0 || t.tm_mon > 11 || t.tm_hour > 23 || t.tm_min > 59) {
    ui("TIME", TFT_RED, "invalid date/time", "unchanged");
    delay(1500);
    return;
  }
  struct timeval tv = {mktime(&t), 0};
  settimeofday(&tv, nullptr);
  rtcTimeExact = true;
  saveClock();
  logEvent("time_set_manual");
}

// ================================================================ recording
// USB-only on both boards. Always probe: battery/charging and USB CDC "plugged"
// status do NOT establish the role of an externally powered OTG receiver.
static bool micStartAny() {
  micUsb = false;
  setCpuFrequencyMhz(160);
  if (usbMicStart(freeQ, filledQ, BUF_SAMPLES, 5000)) { micUsb = true; return true; }
  setCpuFrequencyMhz(80);
  return false;
}
static void micStop() {
  usbMicStop(); micUsb = false; setCpuFrequencyMhz(80);
}
static String micSource() {
  const UsbMicInfo& m = usbMicInfo();
  char b[96];
  snprintf(b, sizeof(b), "usb %04x:%04x %s %luHz %uch %ubit", m.vid, m.pid, m.product, (unsigned long)m.rate,
           m.channels, m.bits);
  return b;
}

struct CardFile {
  String name;
  uint32_t bytes;
};

// Everything in /rec (finished, still to upload, and unfinished .part files).
static std::vector<CardFile> cardFiles() {
  std::vector<CardFile> out;
  File dir = SD.open(REC_DIR);
  if (!dir) return out;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory() && (String(f.name()).endsWith(".wav") || String(f.name()).endsWith(".wav.part")))
      out.push_back({f.name(), (uint32_t)f.size()});
    f.close();
  }
  dir.close();
  std::sort(out.begin(), out.end(), [](const CardFile& a, const CardFile& b) { return a.name < b.name; });
  return out;
}

static String durationOf(uint32_t bytes) {
  uint32_t s = bytes > 44 ? gencord::durationMs(bytes - 44) / 1000 : 0;
  char b[12];
  snprintf(b, sizeof(b), "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  return b;
}

// USB serial 'w': checks that a file can be reopened with "r+" and patched at offset 0
// while keeping its length (what the recording save points rely on).
static void serialSdTest() {
  const char* p = "/gencord-adv/recordings/_sdtest.tmp";
  File f = SD.open(p, FILE_WRITE);
  if (!f) { Serial.println("sdtest: create FAILED"); return; }
  for (int i = 0; i < 1000; i++) f.write((uint8_t)'a');
  f.close();
  f = SD.open(p, "r+");
  if (!f) { Serial.println("sdtest: reopen r+ FAILED"); SD.remove(p); return; }
  f.seek(0);
  f.write((const uint8_t*)"HEAD", 4);
  f.seek(1000);
  f.write((const uint8_t*)"TAIL", 4);
  f.close();
  f = SD.open(p, FILE_READ);
  char head[5] = {0}, tail[5] = {0};
  f.read((uint8_t*)head, 4);
  f.seek(1000);
  f.read((uint8_t*)tail, 4);
  size_t sz = f.size();
  f.close();
  SD.remove(p);
  Serial.printf("sdtest: size %u (want 1004) head %s tail %s -> %s\n", (unsigned)sz, head, tail,
                (sz == 1004 && !strcmp(head, "HEAD") && !strcmp(tail, "TAIL")) ? "OK" : "FAILED");
}

// USB serial 'l': list the card for diagnostics.
static void serialListCard() {
  auto files = cardFiles();
  Serial.printf("%u file(s) in %s (all not yet uploaded):\n", (unsigned)files.size(), REC_DIR);
  for (auto& f : files) Serial.printf("  %-48s %9lu B  %s\n", f.name.c_str(), (unsigned long)f.bytes, durationOf(f.bytes).c_str());
}

// Menu 'l': which recordings are still on the card, i.e. not uploaded yet.
// ; and . scroll (the arrow keys), Esc/Enter/G0 back.
static void listScreen() {
  auto files = cardFiles();
  size_t top = 0;
  uint32_t last = millis();
  bool dirty = true;
  for (;;) {
    if (dirty) {
      String l[4];
      for (int i = 0; i < 4; i++) {
        size_t k = top + i;
        if (k < files.size()) {
          String n = files[k].name;
          n.replace(".wav", "");
          if (n.length() > 19) n = n.substring(0, 19);
          l[i] = n + " " + durationOf(files[k].bytes);
        }
      }
      if (files.empty()) l[0] = "all uploaded";
      ui(String(files.size()) + " on card", files.empty() ? TFT_GREEN : TFT_YELLOW, l[0], l[1], l[2], l[3]);
      dirty = false;
    }
    M5Cardputer.update();
    auto& kb = M5Cardputer.Keyboard;
    if (kb.isChange() && kb.isPressed()) {
      auto& ks = kb.keysState();
      last = millis();
      char c = ks.word.empty() ? 0 : ks.word[0];
      if (ks.enter || c == '`') return;
      if (c == ';' && top > 0) { top--; dirty = true; }
      if (c == '.' && top + 4 < files.size()) { top++; dirty = true; }
    }
    if (M5.BtnA.wasClicked() || millis() - last > 30000) return;
    delay(10);
  }
}

// Menu 'a': independent channel levels, unity gain. No internal mic diagnostics.
static void stereoTest() {
  xQueueReset(filledQ); xQueueReset(freeQ);
  for (int i = 0; i < NBUF; ++i) { int16_t* p = bufs[i]; xQueueSend(freeQ, &p, 0); }
  screenOn();
  ui("USB PROBE", TFT_CYAN, "Need 2ch receiver", "Receiver: STEREO, not mono");
  if (!micStartAny()) { ui("USB ERROR", TFT_RED, usbMicError(), "No internal fallback"); delay(3000); return; }
  for (;;) {
    AudioBlock block;
    if (xQueueReceive(filledQ, &block, pdMS_TO_TICKS(20)) == pdTRUE) {
      measureLevels(block.data, block.samples); xQueueSend(freeQ, &block.data, 0);
      ui("USB L / R", TFT_CYAN, "L peak " + String(levelPeak[0]), "R peak " + String(levelPeak[1]), "Unity gain; no mixing", "Any key = back");
    }
    M5Cardputer.update();
    if (usbMicLost() || M5.BtnA.wasClicked() || (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())) break;
  }
  const bool error = usbMicLost(); micStop();
  if (error) { ui("USB ERROR", TFT_RED, usbMicError()); delay(3000); }
}

struct RecResult {
  bool ok = false;
  bool lowBatt = false;
  bool audioError = false;
};

static RecResult recordSession() {
  RecResult r;
  setCpuFrequencyMhz(80);
  screenOn();
  if (!sdOk) {
    ui("NO SD", TFT_RED, "insert microSD card", "(FAT32)");
    delay(3000);
    return r;
  }
  if (battMv() > 1000 && battMv() < LOW_BATT_MV) {
    ui("BATTERY", TFT_RED, "too low to record", String(battMv()) + " mV");
    delay(3000);
    r.lowBatt = true;
    return r;
  }

  char base[48];
  if (timeValid()) strncpy(base, timeStr("%Y-%m-%d_%H-%M-%S").c_str(), sizeof(base));
  else snprintf(base, sizeof(base), "notime_%lu_%lu", (unsigned long)rtcBoots, (unsigned long)millis());
  String stem = base;
  for (unsigned n = 1; SD.exists(String(REC_DIR) + "/" + stem + ".wav") ||
                         SD.exists(String(REC_DIR) + "/" + stem + ".wav.part"); ++n) stem = String(base) + "_" + String(n);
  String partPath = String(REC_DIR) + "/" + stem + ".wav.part";

  File f = SD.open(partPath, FILE_WRITE);
  if (!f) {
    ui("SD ERROR", TFT_RED, "cannot create file");
    delay(3000);
    return r;
  }
  bool writeErr = !writeWavHeader(f, 0);
  uint32_t dataBytes = 0;

  xQueueReset(filledQ);
  xQueueReset(freeQ);
  for (int i = 0; i < NBUF; i++) {
    int16_t* p = bufs[i];
    xQueueSend(freeQ, &p, 0);
  }
  recPeak = 0;
  ui("USB PROBE", TFT_CYAN, "Need 2ch receiver", "Receiver: STEREO, not mono");

  if (!micStartAny()) {
    f.close();
    SD.remove(partPath);
    ui("USB ERROR", TFT_RED, usbMicError(), "No internal fallback");
    delay(3000);
    return r;
  }
  logEvent("rec_start", String(base) + " " + micSource());

  const uint32_t t0 = millis();
  uint32_t lastHdr = t0, lastBatt = t0, lastBlink = 0, lastDraw = 0, screenUntil = t0 + 2500;
  uint32_t gaps = 0, lastWarn = 0;
  bool ledOn = false, limitReached = false;
  auto saveBlock = [&](const AudioBlock& block) {
    const size_t requested = min(block.samples * sizeof(int16_t), (size_t)(gencord::MaxDataBytes - dataBytes));
    if (!writeErr && requested) {
      measureLevels(block.data, requested / sizeof(int16_t));
      const size_t w = f.write((const uint8_t*)block.data, requested);
      dataBytes += (w / gencord::FrameBytes) * gencord::FrameBytes;
      if (w != requested) writeErr = true;
    }
    if (dataBytes >= gencord::MaxDataBytes) limitReached = true;
    xQueueSend(freeQ, &block.data, 0);
  };
  String recName;
  const uint32_t ignoreBtnUntil = t0 + 800;  // the press that woke us must not count as a click

  for (;;) {
    AudioBlock block;
    while (!writeErr && !limitReached && xQueueReceive(filledQ, &block, 0) == pdTRUE) saveBlock(block);
    if (usbMicLost()) { r.audioError = true; logEvent("usb_error", usbMicError()); break; }
    gaps = usbMicDrops();
    if (limitReached) break;


    M5Cardputer.update();
    const uint32_t now = millis();
    while (Serial.available()) {  // a flash request must not cut the recording
      if (Serial.read() == 'F') Serial.println("BUSY-RECORDING");
    }
    if (M5.BtnA.wasDoubleClicked() && now > ignoreBtnUntil) break;
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      // Typing while recording sets the file name; the mic keeps running meanwhile.
      // With the screen off, the first key only wakes the screen.
      auto& ks = M5Cardputer.Keyboard.keysState();
      if (screenIsOn) {
        if (ks.del && recName.length()) recName.remove(recName.length() - 1);
        for (char c : ks.word) {
          if (c == ' ') c = '_';
          if (recName.length() < 40 && accepts(Input::Name, c)) recName += c;
        }
      }
      screenOn();
      screenUntil = now + 5000;
      lastDraw = 0;
    }

    if (screenIsOn) {
      if (now > screenUntil) {
        screenOff();
      } else if (now - lastDraw > 120) {  // ~8 fps: enough for turning reels
        recScreen(gencord::durationMs(dataBytes), recName.length() ? recName + "_" : String(base), dataBytes / 1048576.0f, gaps);
        lastDraw = now;
      }
    }

    // Save point 2 s after the start, then every 10 s, so a reset or empty battery
    // loses at most the last 10 s. flush() alone does not update the FAT directory
    // entry (the file length stays 44 bytes until close), so the file is closed and
    // reopened; the mic keeps filling its buffers meanwhile.
    if (now - lastHdr > (dataBytes < 100000 ? 2000u : 10000u)) {
      if (!writeWavHeader(f, dataBytes)) writeErr = true;
      f.close();
      f = SD.open(partPath, "r+");
      if (!f) writeErr = true;
      else if (!f.seek(44 + dataBytes)) writeErr = true;
      lastHdr = now;
    }
    if (now - lastBlink > 5000) {  // short dim red blink = "still recording"
      M5.Led.setAllColor(24, 0, 0);
      ledOn = true;
      lastBlink = now;
    } else if (ledOn && now - lastBlink > 40) {
      M5.Led.setAllColor(0, 0, 0);
      ledOn = false;
    }
    if (now - lastBatt > 10000) {
      int mv = battMv();
      if (mv > 1000 && mv < LOW_BATT_MV) {  // close the file cleanly well before the cell is empty
        r.lowBatt = true;
        break;
      }
      if (mv > 1000 && mv < LOW_BATT_WARN_MV && now - lastWarn > 60000) {
        // Visual warning only: never reconfigure codec/USB while capturing.
        screenOn();
        screenUntil = millis() + 3000;
        lastWarn = now;
      }
      lastBatt = now;
    }
    if (writeErr) break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  // Stop producer, then drain all completed and partial output blocks before header finalization.
  micStop();
  if (usbMicLost()) r.audioError = true;
  M5.Led.setAllColor(0, 0, 0);
  AudioBlock block;
  while (xQueueReceive(filledQ, &block, 0) == pdTRUE) saveBlock(block);
  if (!writeWavHeader(f, dataBytes)) writeErr = true;
  const bool needsTrim = f && f.size() != 44 + dataBytes;
  f.close();
  if (needsTrim && !trimFile(partPath, 44 + dataBytes)) writeErr = true;
  const uint32_t secs = gencord::durationMs(dataBytes) / 1000;
  screenOn();
  if (!dataBytes) {
    logEvent("rec_empty", stem);
    ui("NOT SAVED", TFT_RED, r.audioError ? usbMicError() : "No complete audio frames", "Empty .part retained on SD");
    delay(3000); return r;
  }
  String name = recName;
  if (!r.lowBatt && !writeErr && !r.audioError && !limitReached)
    name = textInput("Name?", NAME_TIMEOUT_S * 1000UL, Input::Name, 40, recName);
  String finalName = stem + (name.length() ? "_" + name : "") + (r.audioError ? "_usb_error" : "") + ".wav";
  const String finalPath = String(REC_DIR) + "/" + finalName;
  if (writeErr || SD.exists(finalPath) || !SD.rename(partPath, finalPath)) {
    writeErr = true; finalName = stem + ".wav.part";
  }
  logEvent(writeErr ? "rec_sd_error" : (r.audioError ? "rec_usb_error" : "rec_stop"),
           finalName + " " + String(secs) + "s src=usb2 peak=" + String(recPeak));
  rtcPending = listRecordings(false).size();
  ui(writeErr ? "SD ERROR" : (r.audioError ? "USB ERROR" : (limitReached ? "60 MIN LIMIT" : "SAVED")),
     writeErr || r.audioError || r.lowBatt ? TFT_RED : TFT_GREEN,
     writeErr ? "SD ERROR" : (r.audioError ? usbMicError() : (r.lowBatt ? "Stopped: low battery" : finalName.substring(0, 26))),
     String(secs / 60) + " min " + String(secs % 60) + " s stereo", "Original kept on SD");
  delay(r.audioError ? 5000 : 1500);
  r.ok = !writeErr;
  return r;
}

// ================================================================ WebDAV upload
struct DavTarget {
  bool tls = false;
  String host;
  uint16_t port = 80;
  String path;  // without trailing slash
  String auth;
} dav;

static void parseDavUrl() {
  String u = S.davUrl;
  dav.tls = u.startsWith("https://");
  u = u.substring(u.indexOf("://") + 3);
  int slash = u.indexOf('/');
  String hostport = slash < 0 ? u : u.substring(0, slash);
  dav.path = slash < 0 ? "" : u.substring(slash);
  while (dav.path.endsWith("/")) dav.path.remove(dav.path.length() - 1);
  int colon = hostport.indexOf(':');
  dav.host = colon < 0 ? hostport : hostport.substring(0, colon);
  dav.port = colon < 0 ? (dav.tls ? 443 : 80) : hostport.substring(colon + 1).toInt();
  dav.auth = "Basic " + base64::encode(S.davUser + ":" + S.davPass);
}

static String urlEncodePath(const String& p) {
  static const char* hex = "0123456789ABCDEF";
  String o;
  for (size_t i = 0; i < p.length(); i++) {
    char c = p[i];
    if (isalnum((unsigned char)c) || strchr("-_.~/", c)) o += c;
    else { o += '%'; o += hex[(uint8_t)c >> 4]; o += hex[(uint8_t)c & 15]; }
  }
  return o;
}

static String absUrl(const String& path) {
  return String(dav.tls ? "https://" : "http://") + dav.host + ":" + dav.port + urlEncodePath(path);
}

static String readLine(NetworkClient& c, uint32_t timeoutMs) {
  String s;
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    while (c.available()) {
      char ch = c.read();
      if (ch == '\n') return s;
      if (ch != '\r') s += ch;
    }
    if (!c.connected() && !c.available()) break;
    delay(2);
  }
  return s;
}

using Progress = void (*)(size_t sent, size_t total);
static volatile bool uploadAbort = false;  // G0 during an upload: stop it, the user wants to record

// One WebDAV request on a fresh connection. Returns the HTTP status or -1.
static int davRequest(const char* method, const String& path, const String& extraHeaders, File* body,
                      int64_t* contentLength = nullptr, String* respBody = nullptr, Progress prog = nullptr) {
  NetworkClient plain;
  NetworkClientSecure secure;
  NetworkClient& c = dav.tls ? (NetworkClient&)secure : plain;
  if (dav.tls) secure.setInsecure();  // Synology default certs are self-signed
  if (!c.connect(dav.host.c_str(), dav.port, 8000)) return -1;

  size_t len = body ? body->size() : 0;
  String req = String(method) + " " + urlEncodePath(path) + " HTTP/1.1\r\n" + "Host: " + dav.host + ":" + dav.port +
               "\r\n" + "Authorization: " + dav.auth + "\r\n" + "User-Agent: gencord\r\n" +
               "Connection: close\r\n" + "Content-Length: " + String((unsigned long)len) + "\r\n" + extraHeaders +
               "\r\n";
  c.print(req);

  if (body) {
    static constexpr size_t CHUNK = 8192;
    uint8_t* buf = (uint8_t*)malloc(CHUNK);
    if (!buf) return -1;
    body->seek(0);
    size_t sent = 0;
    bool fail = false;
    while (sent < len && !fail) {
      int n = body->read(buf, min(CHUNK, len - sent));
      if (n <= 0) { fail = true; break; }
      size_t off = 0;
      uint32_t tw = millis();
      while (off < (size_t)n) {
        size_t w = c.write(buf + off, n - off);
        if (w > 0) { off += w; tw = millis(); }
        else if (!c.connected() || millis() - tw > 20000) { fail = true; break; }
        else delay(2);
      }
      sent += off;
      if (prog) prog(sent, len);
      if (uploadAbort) fail = true;
    }
    free(buf);
    if (fail) { c.stop(); return -1; }
  }

  String status = readLine(c, 30000);  // the NAS may need a moment after a big upload
  int sp = status.indexOf(' ');
  int code = (status.startsWith("HTTP/") && sp > 0) ? status.substring(sp + 1).toInt() : -1;
  int64_t clen = -1;
  for (;;) {
    String h = readLine(c, 5000);
    if (h.length() == 0) break;
    String hl = h;
    hl.toLowerCase();
    if (hl.startsWith("content-length:")) clen = atoll(h.c_str() + 15);
  }
  if (contentLength) *contentLength = clen;
  if (respBody && strcmp(method, "HEAD") != 0) {
    uint32_t t0 = millis();
    while ((c.connected() || c.available()) && millis() - t0 < 5000 && respBody->length() < 8192) {
      while (c.available() && respBody->length() < 8192) *respBody += (char)c.read();
      delay(2);
    }
  }
  c.stop();
  return code;
}

// Size of a remote file: HEAD first, PROPFIND as fallback. -1 if unknown.
static int64_t remoteSize(const String& path) {
  int64_t len = -1;
  if (davRequest("HEAD", path, "", nullptr, &len) == 200 && len >= 0) return len;
  String body;
  int code = davRequest("PROPFIND", path, "Depth: 0\r\n", nullptr, nullptr, &body);
  if (code != 207) return -1;
  int i = body.indexOf("getcontentlength");
  if (i < 0) return -1;
  i = body.indexOf('>', i);
  if (i < 0) return -1;
  return atoll(body.c_str() + i + 1);
}

static String upName;
static int upIdx = 0, upCount = 0;
static uint32_t upLastDraw = 0;
static void upProgress(size_t sent, size_t total) {
  M5.update();
  if (M5.BtnA.wasPressed()) uploadAbort = true;
  if (!screenIsOn || (millis() - upLastDraw < 150 && sent < total)) return;
  upLastDraw = millis();
  uploadScreen(upName, sent, total, upIdx, upCount);
}

// Upload -> verify size -> rename -> verify again. Only then may the caller delete the local file.
static bool uploadOne(const String& name, String& err) {
  String local = String(REC_DIR) + "/" + name;
  File f = SD.open(local, FILE_READ);
  if (!f) { err = "open"; return false; }
  const int64_t size = f.size();
  const String remote = dav.path + "/" + name;
  const String tmp = remote + ".part";
  upName = name;
  if (screenIsOn) uploadScreen(name, 0, size, upIdx, upCount);
  int code = davRequest("PUT", tmp, "Content-Type: audio/wav\r\n", &f, nullptr, nullptr, upProgress);
  f.close();
  if (code != 200 && code != 201 && code != 204) { err = "PUT " + String(code); return false; }
  if (remoteSize(tmp) != size) { err = "verify"; return false; }
  code = davRequest("MOVE", tmp, "Destination: " + absUrl(remote) + "\r\nOverwrite: T\r\n", nullptr);
  if (code != 201 && code != 204) { err = "MOVE " + String(code); return false; }
  if (remoteSize(remote) != size) { err = "verify2"; return false; }
  return true;
}

static void timeSynced() {
  rtcTimeExact = true;
  saveClock();
  logEvent("ntp_ok");
}

// sntp_get_sync_status() never reports COMPLETED on this core even though the clock
// gets set, so the time server's answer is caught with the notification callback.
static volatile bool ntpAnswered = false;
static void onNtpSync(struct timeval*) { ntpAnswered = true; }

static void startNtp() {
  ntpAnswered = false;
  configTzTime(S.tz.c_str(), S.ntp.c_str(), "pool.ntp.org");
  sntp_set_time_sync_notification_cb(onNtpSync);
}

static void syncTime() {
  startNtp();
  uint32_t t0 = millis();
  while (!ntpAnswered && millis() - t0 < 10000) delay(50);
  if (ntpAnswered) timeSynced();
}

static void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// Cheap presence check: scan only the last known channel (~0.2 s); every 4th
// miss scan all channels in case the router changed channel.
static uint8_t scanChannel() { return (rtcWifiCh == 0 || rtcScanMiss % 4 == 3) ? 0 : rtcWifiCh; }

// Joins the home network found by the last scan (n = scan result count).
static bool wifiJoin(int n) {
  if (n <= 0) {
    rtcScanMiss++;
    WiFi.scanDelete();
    wifiOff();
    return false;
  }
  rtcScanMiss = 0;
  rtcWifiCh = WiFi.channel(0);
  WiFi.BSSID((uint8_t)0, rtcWifiBssid);
  WiFi.scanDelete();
  WiFi.begin(S.ssid.c_str(), S.pass.c_str(), rtcWifiCh, rtcWifiBssid);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(50);
  if (WiFi.status() != WL_CONNECTED) {
    rtcWifiCh = 0;  // force a full scan next time
    wifiOff();
    return false;
  }
  WiFi.setSleep(false);
  return true;
}

static bool wifiHome(bool fullScan) {
  WiFi.mode(WIFI_STA);
  return wifiJoin(WiFi.scanNetworks(false, false, false, 150, fullScan ? 0 : scanChannel(), S.ssid.c_str()));
}

// ---- background network job in the menu: scan -> join -> NTP -> upload pending.
// Every step is polled, so G0 starts a recording at any moment (it cancels the job).
// Away from home only the ~0.2 s scan happens, then the radio is switched off.
enum class Bg { Idle, Scanning, Joining, Ntp };
static Bg bg = Bg::Idle;
static uint32_t bgSince = 0;
static String netStatus;

static bool configured();
static void uploadConnected(bool interactive, bool needTime);
static bool bgBusy() { return bg != Bg::Idle; }

static void startBackgroundCheck() {
  if (!configured() || bg != Bg::Idle) return;
  if (rtcPending == 0 && rtcTimeExact) return;  // nothing to upload, clock is fine
  const int mv = battMv();
  if (mv > 1000 && mv < LOW_BATT_WARN_MV) return;  // power saving: the radio stays off
  WiFi.mode(WIFI_STA);
  if (WiFi.scanNetworks(true, false, false, 150, scanChannel(), S.ssid.c_str()) != WIFI_SCAN_RUNNING) {
    wifiOff();
    return;
  }
  bg = Bg::Scanning;
  bgSince = millis();
  netStatus = "looking for home Wi-Fi...";
}

static void cancelBackgroundCheck() {
  if (bg == Bg::Idle) return;
  if (bg == Bg::Scanning) WiFi.scanDelete();
  wifiOff();
  bg = Bg::Idle;
  netStatus = "";
  setCpuFrequencyMhz(80);
}

static void pollBackgroundCheck() {
  switch (bg) {
    case Bg::Idle:
      return;
    case Bg::Scanning: {
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING) return;
      if (n <= 0) {
        rtcScanMiss++;
        WiFi.scanDelete();
        wifiOff();
        bg = Bg::Idle;
        netStatus = "no home Wi-Fi - Wi-Fi off";
        return;
      }
      rtcScanMiss = 0;
      rtcWifiCh = WiFi.channel(0);
      WiFi.BSSID((uint8_t)0, rtcWifiBssid);
      WiFi.scanDelete();
      setCpuFrequencyMhz(160);
      WiFi.begin(S.ssid.c_str(), S.pass.c_str(), rtcWifiCh, rtcWifiBssid);
      bg = Bg::Joining;
      bgSince = millis();
      netStatus = "connecting to " + S.ssid;
      return;
    }
    case Bg::Joining:
      if (WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(false);
        startNtp();
        bg = Bg::Ntp;
        bgSince = millis();
        netStatus = "getting time...";
      } else if (millis() - bgSince > 10000) {
        rtcWifiCh = 0;  // full scan next time
        cancelBackgroundCheck();
        netStatus = "Wi-Fi connect failed";
      }
      return;
    case Bg::Ntp:
      if (ntpAnswered || millis() - bgSince > 10000) {
        if (ntpAnswered) timeSynced();
        bg = Bg::Idle;
        netStatus = "";
        if (sdOk && rtcPending) uploadConnected(true, false);  // G0 aborts it (uploadAbort)
        else wifiOff();
        setCpuFrequencyMhz(80);
      }
      return;
  }
}

static void uploadAll(bool interactive) {
  if (!sdOk) return;
  if (!configured()) {
    if (interactive) {
      ui("NOT SET UP", TFT_ORANGE, "press c to enter", "Wi-Fi + Synology");
      delay(2000);
    }
    return;
  }
  rtcPending = listRecordings(false).size();
  if (rtcPending == 0 && rtcTimeExact) {  // nothing to send and the clock is fine: stay offline
    if (interactive) {
      ui("UPLOAD", TFT_GREEN, "nothing to upload", "all on the Synology");
      delay(1500);
    }
    return;
  }
  if (interactive) ui("WI-FI", TFT_YELLOW, "looking for", S.ssid);
  setCpuFrequencyMhz(160);
  if (!wifiHome(interactive)) {  // the user pressed 'u': scan all channels
    setCpuFrequencyMhz(80);
    if (interactive) {
      ui("WI-FI", TFT_DARKGREY, "home network", "not in range", String(rtcPending) + " pending");
      delay(1500);
    }
    return;
  }
  uploadConnected(interactive, true);
  setCpuFrequencyMhz(80);
}

static void uploadConnected(bool interactive, bool needTime) {
  std::vector<String> files = listRecordings(false);
  uploadAbort = false;
  if (needTime) syncTime();
  davRequest("MKCOL", dav.path, "", nullptr);  // 201 created / 405 exists: both fine

  int done = 0, failed = 0;
  String err;
  upCount = files.size();
  upIdx = 0;
  for (auto& name : files) {
    int mv = battMv();
    if (mv > 1000 && mv < LOW_BATT_MV + 100) { err = "low battery"; failed++; break; }
    upIdx++;
    upLastDraw = 0;
    if (uploadOne(name, err)) {
      File original = SD.open(String(REC_DIR) + "/" + name, FILE_READ);
      const uint32_t bytes = original ? original.size() : 0;
      original.close();
      File marker = SD.open(uploadedMarker(name), FILE_WRITE);
      if (bytes <= 44 || !marker || marker.println(String(bytes)) == 0) {
        marker.close(); err = "SD upload marker"; failed++; break;
      }
      marker.close();
      logEvent("upload_ok", name);
      done++;
    } else if (uploadAbort) {
      logEvent("upload_aborted", name);  // stays on the card, next sync retries
      break;
    } else {
      logEvent("upload_fail", name + " " + err);
      failed++;
      break;  // most likely a network problem: try again next time
    }
    if (uploadAbort) break;
  }
  // Private SD log is not uploaded: receiver accepts WAV only.

  wifiOff();
  setCpuFrequencyMhz(80);

  rtcPending = listRecordings(false).size();
  if (uploadAbort) return;  // the caller starts the recording
  rtcLastSync = time(nullptr);
  rtcLastSyncOk = failed == 0 ? 1 : 0;
  if (interactive) {
    ui(failed ? "UPLOAD ERR" : "UPLOADED", failed ? TFT_RED : TFT_GREEN, String(done) + " uploaded",
       failed ? err : "", String(rtcPending) + " pending");
    delay(2000);
  }
}

// ================================================================ settings file
static void loadSettings() {
  if (!sdOk) return;
  File f = SD.open(CFG_PATH, FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int eq = line.indexOf('=');
    if (line.length() == 0 || line[0] == '#' || eq < 0) continue;
    String k = line.substring(0, eq), v = line.substring(eq + 1);
    k.trim();
    v.trim();
    if (v.length() == 0) continue;  // empty entries keep the built-in default
#ifdef CFG_MIGRATE_DAV_URLS
    if (k == "dav_url") {  // outdated URLs from an earlier build keep the built-in default
      bool outdated = false;
      for (const char* old : {CFG_MIGRATE_DAV_URLS}) outdated |= v == old;
      if (outdated) continue;
    }
#endif
    if (k == "wifi_ssid") S.ssid = v;
    else if (k == "wifi_pass") S.pass = v;
    else if (k == "dav_url") S.davUrl = v;
    else if (k == "dav_user") S.davUser = v;
    else if (k == "dav_pass") S.davPass = v;
    else if (k == "tz") S.tz = v;
    else if (k == "ntp") S.ntp = v;
    else if (k == "wifi_check_min") S.wifiCheckMin = constrain(v.toInt(), 1, 1440);
  }
  f.close();
}

static bool saveSettings() {
  if (!sdOk) return false;
  File f = SD.open(CFG_PATH, FILE_WRITE);
  if (!f) return false;
  f.println("# gencord settings (also editable on the device: menu key 'c')");
  f.println("wifi_ssid=" + S.ssid);
  f.println("wifi_pass=" + S.pass);
  f.println("# Synology: http://<nas-ip>:5005/<SharedFolder>/<subfolder>  (https -> port 5006)");
  f.println("dav_url=" + S.davUrl);
  f.println("dav_user=" + S.davUser);
  f.println("dav_pass=" + S.davPass);
  f.println("tz=" + S.tz);
  f.println("ntp=" + S.ntp);
  f.println("wifi_check_min=" + String(S.wifiCheckMin));
  f.println("# USB-only stereo capture; no internal mic, no AGC");
  f.close();
  return true;
}

static bool configured() { return S.ssid.length() && S.davUrl.length(); }

static void setupWizard() {
  struct Field {
    const char* label;
    String* value;
  } fields[] = {{"Wi-Fi name", &S.ssid},   {"Wi-Fi password", &S.pass},  {"WebDAV URL", &S.davUrl},
                {"WebDAV user", &S.davUser}, {"WebDAV password", &S.davPass}};
  for (auto& fl : fields) *fl.value = textInput(fl.label, 180000, Input::Any, 120, *fl.value);
  bool ok = saveSettings();
  parseDavUrl();
  rtcWifiCh = 0;
  ui(ok ? "SAVED" : "NOT SAVED", ok ? TFT_GREEN : TFT_RED, ok ? CFG_PATH : "no SD card", "", "u = test upload now");
  delay(2000);
}

// ================================================================ sleep
// Light sleep, not deep sleep: G0 is the ESP32-S3 boot strap pin, and waking from
// deep sleep with it held low restarts the chip into the ROM download mode (black
// screen). Light sleep just pauses the CPU, so G0 starts a recording instantly.
enum class Wake { Button, Timer };

static Wake lightSleep() {
  saveClock();
  cancelBackgroundCheck();
  screenOff();
  M5.Led.setAllColor(0, 0, 0);
  wifiOff();
  if (sdOk) sdEnd();
  while (gpio_get_level(PIN_BTN) == 0) delay(10);  // a held button would wake us at once
  delay(50);
  gpio_wakeup_enable(PIN_BTN, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  if (rtcPending) esp_sleep_enable_timer_wakeup((uint64_t)S.wifiCheckMin * 60ULL * 1000000ULL);
  esp_light_sleep_start();
  const bool timer = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
  gpio_wakeup_disable(PIN_BTN);
  sdOk = sdBegin();
  return timer ? Wake::Timer : Wake::Button;
}

static void recordNow();

// Sleeps until G0 (-> record) ; timer wakes only run the background upload check.
static void sleepUntilButton() {
  for (;;) {
    if (lightSleep() == Wake::Button) {
      logEvent("wake_button");
      recordNow();
      return;
    }
    logEvent("wake_timer", String(rtcPending) + " pending");
    uploadAll(false);
    if (uploadAbort) {  // G0 pressed during that upload
      uploadAbort = false;
      recordNow();
      return;
    }
  }
}

[[noreturn]] static void deepSleep() {
  saveClock();  // the power switch would otherwise lose the time
  screenOff();
  M5.Led.setAllColor(0, 0, 0);

  if (sdOk) sdEnd();
  wifiOff();
  while (gpio_get_level(PIN_BTN) == 0) delay(10);  // a held button would wake us immediately
  delay(50);
  for (auto pin : HOLD_LOW_PINS) {
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
    gpio_hold_en(pin);
  }
  gpio_deep_sleep_hold_en();
  rtc_gpio_pullup_en(PIN_BTN);
  rtc_gpio_pulldown_dis(PIN_BTN);
  esp_sleep_enable_ext0_wakeup(PIN_BTN, 0);
  if (rtcPending) esp_sleep_enable_timer_wakeup((uint64_t)S.wifiCheckMin * 60ULL * 1000000ULL);
  esp_deep_sleep_start();
}

// ================================================================ menu
static void drawMenu() {
  String sync = rtcLastSyncOk < 0 ? "never" : (rtcLastSyncOk ? "ok " : "FAILED ");
  if (rtcLastSyncOk >= 0 && rtcLastSync > 1704067200) {
    struct tm t;
    localtime_r(&rtcLastSync, &t);
    char b[16];
    strftime(b, sizeof(b), "%d.%m %H:%M", &t);
    sync += b;
  }
  String label = !sdOk ? "NO SD CARD" : (!configured() ? "press c to set up" : "gencord " GENCORD_VERSION);
  String info = netStatus.length() ? netStatus
                                   : String(rtcPending) + " on card  |  sync " + (configured() ? sync : "not set up");
  // White = fetched from the internet (or set by hand) since power-on.
  // Red "~" = only the last saved time, may be off; red "TIME NOT SET" = nothing known.
  menuScreen(timeValid() ? (rtcTimeExact ? "" : "~") + timeStr("%H:%M %d.%m.") : "TIME NOT SET",
             timeValid() && rtcTimeExact ? TFT_WHITE : TFT_RED, label,
             info, "(G0)rec (u)p (l)ist (o)ff (t)ime (a)levels");
}

// After a recording: look for home Wi-Fi in the background; the menu stays usable.
static void afterRecording(const RecResult& r) {
  if (r.ok && !r.lowBatt && !r.audioError) startBackgroundCheck();
}

static void recordNow() {
  cancelBackgroundCheck();
  afterRecording(recordSession());
}

static void menuLoop() {
  screenOn();
  uint32_t lastInput = millis(), lastDraw = 0, statusSince = 0;
  for (;;) {
    M5Cardputer.update();
    const uint32_t now = millis();
    bool acted = false;
    pollBackgroundCheck();  // may run an upload; G0 there sets uploadAbort
    // isChange() consumes the change, so it must be read exactly once per loop
    const bool keyDown = M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed();
    if (Serial.available()) {  // USB diagnostics: 'm' = microphone self-test
      const int cmd = Serial.read();
      if (cmd == 'm') stereoTest();
      else if (cmd == 'l') serialListCard();
      else if (cmd == 'w') serialSdTest();
      else if (cmd == 'F') {  // the PC is about to flash: say so on the screen
        screenOn();
        // The PC will reset the chip into download mode within a few seconds. Stay
        // awake and do not start a recording meanwhile; G0 cancels.
        Serial.println("FLASH-OK");
        Serial.flush();
        ui("UPDATING", TFT_YELLOW, "new firmware via USB", "do not unplug", "G0 = cancel");
        const uint32_t until = millis() + 20000;
        while (millis() < until) {
          M5.update();
          if (M5.BtnA.wasClicked()) break;
          delay(10);
        }
      }
      acted = true;
    }
    if (uploadAbort) {
      uploadAbort = false;
      recordNow();
      acted = true;
    } else if (M5.BtnA.wasClicked()) {
      recordNow();
      acted = true;
    } else if (keyDown && !screenIsOn) {
      acted = true;  // screen was off: this key only wakes it
    } else if (keyDown) {
      auto& ks = M5Cardputer.Keyboard.keysState();
      char k = ks.word.empty() ? 0 : tolower(ks.word[0]);
      if (k == 'r' || ks.enter) recordNow();
      else if (k == 'u') { cancelBackgroundCheck(); uploadAll(true); }
      else if (k == 't') setTimeManually();
      else if (k == 'c') setupWizard();
      else if (k == 'l') listScreen();
      else if (k == 'a') stereoTest();
      else if (k == 'o' || k == 's') sleepUntilButton();
      acted = true;
    }
    if (acted) {
      screenOn();
      lastInput = millis();
      lastDraw = 0;
    }
    if (netStatus.length() && !bgBusy()) {  // "no home Wi-Fi" stays visible for 5 s
      if (!statusSince) statusSince = now;
      else if (now - statusSince > 5000) { netStatus = ""; statusSince = 0; lastDraw = 0; }
    }
    // 20 s without input: screen off (device stays awake, any key wakes it);
    // IDLE_SLEEP_S without input: light sleep, then only G0 wakes.
    if (screenIsOn && !bgBusy() && millis() - lastInput > MENU_SCREEN_S * 1000UL) screenOff();
    if (screenIsOn && now - lastDraw > (bgBusy() ? 250u : 1000u)) {
      drawMenu();
      lastDraw = now;
    }
    if (!bgBusy() && millis() - lastInput > IDLE_SLEEP_S * 1000UL) {
      sleepUntilButton();
      screenOn();
      lastInput = millis();
      lastDraw = 0;
    }
    delay(10);
  }
}

// ================================================================ entry
void setup() {
  for (auto pin : HOLD_LOW_PINS) gpio_hold_dis(pin);
  gpio_deep_sleep_hold_dis();
  rtc_gpio_deinit(PIN_BTN);

  auto cfg = M5.config();
  cfg.internal_spk = false;  // ADV codec is not part of USB capture
  cfg.internal_mic = false;  // USB only; do not configure original PDM or ADV codec
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  // M5Unified detects original vs ADV; M5Cardputer 1.1.1 selects IOMatrix vs TCA8418.
  M5Cardputer.begin(cfg, true);
  if (M5.getBoard() != m5::board_t::board_M5CardputerADV && M5.getBoard() != m5::board_t::board_M5Cardputer) {
    screenOn(); ui("BOARD ERROR", TFT_RED, "Cardputer / ADV required");
    for (;;) delay(1000);
  }
  Serial.begin(115200);  // USB CDC: diagnostics only
  M5.Display.setBrightness(0);
  M5.Display.setRotation(1);
  screenIsOn = false;

  sdOk = sdBegin();
  loadSettings();
  setenv("TZ", S.tz.c_str(), 1);
  tzset();
  rtcBoots++;
  setCpuFrequencyMhz(80);

  filledQ = xQueueCreate(NBUF, sizeof(AudioBlock));
  freeQ = xQueueCreate(NBUF, sizeof(int16_t*));
  if (!filledQ || !freeQ) { screenOn(); ui("RAM ERROR", TFT_RED, "Audio queues unavailable"); for (;;) delay(1000); }
  parseDavUrl();
  const bool restoredClock = restoreClock();
  if (sdOk) rtcPending = listRecordings(true).size();

  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT0:  // G0 pressed while asleep -> record right away
      logEvent("wake_button");
      afterRecording(recordSession());
      menuLoop();
      break;
    case ESP_SLEEP_WAKEUP_TIMER:  // background check for home Wi-Fi
      logEvent("wake_timer", String(rtcPending) + " pending");
      if (rtcPending) uploadAll(false);
      deepSleep();
      break;
    default:  // power switch / reset
      logEvent(restoredClock ? "boot_time_restored" : "boot", GENCORD_VERSION " " __DATE__ " " __TIME__);
      startBackgroundCheck();  // NTP (and pending uploads) if the home Wi-Fi is around
      menuLoop();
  }
}

void loop() {}
