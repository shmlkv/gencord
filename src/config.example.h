// Optional private config.h (gitignored). Prefer SD /gencord-adv/recorder.cfg.
// Never put real credentials in tracked source or a distributed app image.
#pragma once
#define WIFI_SSID ""
#define WIFI_PASS ""
#define DAV_URL ""  // WebDAV folder, no trailing slash; configured privately
#define DAV_USER ""
#define DAV_PASS ""
#define SAMPLE_RATE 16000 // fixed output: signed PCM16, two independent channels
// No AGC_MAX / usb_mic option: USB stereo is mandatory; no built-in fallback.
#define WIFI_CHECK_MIN 20
#define IDLE_SLEEP_S 60
#define NAME_TIMEOUT_S 30
#define LOW_BATT_MV 3400
#define SCREEN_BRIGHTNESS 60
