#pragma once

// ESP32AntTest — tuneable constants
// See docs/SPEC.md §6 for descriptions

// WiFi
#define ANT_WIFI_CHANNEL        1
#define ANT_SOFTAP_SSID_PREFIX  "AntTest"
#define ANT_SOFTAP_PASS         ""        // open network
#define ANT_STATION_IP          "192.168.26.1"
#define ANT_UDP_PORT            5432

// Sampling (beacon mode) — see ADR-004
#define ANT_BEACON_HZ           5         // both boards beacon at this rate
#define ANT_DISPLAY_HZ          2         // OLED live-RSSI refresh (Mobile quick-check/active)
#define ANT_AUTO_INTERVAL_MS    5000      // ad-hoc Auto sample period
#define ANT_LOSS_THRESHOLD      5         // consecutive seconds no decode = link lost

// Serial (host<->Station — see SERIAL_PROTOCOL.md §1)
#define ANT_SERIAL_BAUD         115200

// TX power options in dBm (user-facing / packet / log units).
// esp_wifi_set_max_tx_power() takes 0.25 dBm units — use ANT_DBM_TO_IDF().
// The chip clamps to its supported range (C3 min ~3 dBm); read back the
// actual power with esp_wifi_get_max_tx_power() before logging.
#define ANT_TX_POWER_LOW        2     // dBm
#define ANT_TX_POWER_MED        10    // dBm
#define ANT_TX_POWER_HIGH       17    // dBm
#define ANT_TX_POWER_MAX        20    // dBm
#define ANT_DEFAULT_TX_POWER    17    // dBm
#define ANT_DBM_TO_IDF(dbm)    ((int8_t)((dbm) * 4))

// Button gestures (ms)
#define ANT_BTN_SHORT_MAX_MS    500
#define ANT_BTN_LONG_MIN_MS     1500
#define ANT_BTN_DOUBLE_GAP_MS   400
#define ANT_BTN_DEBOUNCE_MS     20

// OLED + button — Mobile-side pins. Override in board_config.h.
// Config A (default) = C3 as Mobile; the #defines below are the C3 (Config A)
// defaults. Config B = WROOM-32 as Mobile; override in board_config.h:
// SDA=21, SCL=22, Button=17.
#ifndef ANT_OLED_I2C_ADDR
#define ANT_OLED_I2C_ADDR       0x3C    // SSD1306; some modules use 0x3D
#endif
#ifndef ANT_OLED_SDA_PIN
#define ANT_OLED_SDA_PIN        8     // C3 (Config A default); WROOM (Config B): 21
#endif
#ifndef ANT_OLED_SCL_PIN
#define ANT_OLED_SCL_PIN        9     // C3 (Config A default); WROOM (Config B): 22
#endif
#ifndef ANT_BUTTON_PIN
#define ANT_BUTTON_PIN          5     // C3 (Config A default); WROOM (Config B): 17
#endif

// Mobile outage buffer (RAM ring buffer; no LittleFS on Mobile — see ADR-004)
#define ANT_MOB_BUFFER_MAX      4096    // ~ worst-case long-walk backlog (markers + RSSI)

// Station LittleFS (durable fallback — streams over serial AND mirrors to flash)
#define ANT_LOG_MOUNT_POINT     "/logs"
#define ANT_LOG_MAX_FILES       5
