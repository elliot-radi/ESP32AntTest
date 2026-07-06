#pragma once

// ESP32AntTest — tuneable constants
// See docs/SPEC.md §6 for descriptions

// WiFi
#define ANT_WIFI_CHANNEL        1
#define ANT_SOFTAP_SSID_PREFIX  "AntTest"
#define ANT_SOFTAP_PASS         ""        // open network
#define ANT_STATION_IP          "192.168.4.1"
#define ANT_UDP_PORT            5432

// Measurement
#define ANT_BURST_SIZE          10        // pings per Manual burst
#define ANT_AUTO_INTERVAL_MS    5000      // Auto mode sample period
#define ANT_PING_TIMEOUT_MS     2000      // ms before a ping is TIMEOUT
#define ANT_LOSS_THRESHOLD      5         // consecutive timeouts = link lost

// TX power options (dBm) — must be supported by esp_wifi_set_max_tx_power
#define ANT_TX_POWER_LOW        8         // ~2 dBm (unit is 0.25 dBm steps)
#define ANT_TX_POWER_MED        40        // ~10 dBm
#define ANT_TX_POWER_HIGH       68        // ~17 dBm
#define ANT_TX_POWER_MAX        80        // ~20 dBm
#define ANT_DEFAULT_TX_POWER    68        // 17 dBm default

// Button gestures (ms)
#define ANT_BTN_SHORT_MAX_MS    500
#define ANT_BTN_LONG_MIN_MS     1500
#define ANT_BTN_DOUBLE_GAP_MS   400
#define ANT_BTN_DEBOUNCE_MS     20

// OLED + button — Mobile-side pins. Override in board_config.h.
// Defaults below are for ESP32-WROOM-32 as Mobile (Config A).
// For ESP32-C3 as Mobile (Config B): SDA=8, SCL=9, Button=5.
#ifndef ANT_OLED_SDA_PIN
#define ANT_OLED_SDA_PIN        21
#endif
#ifndef ANT_OLED_SCL_PIN
#define ANT_OLED_SCL_PIN        22
#endif
#ifndef ANT_BUTTON_PIN
#define ANT_BUTTON_PIN          17
#endif

// LittleFS
#define ANT_LOG_MOUNT_POINT     "/logs"
#define ANT_LOG_MAX_FILES       5
