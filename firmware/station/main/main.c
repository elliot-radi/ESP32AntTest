/* ESP32AntTest — Station main.
 *
 * Increment 2: RF + beacon logging on top of Increment 1's serial/session/
 * LittleFS surface.
 *   - SoftAP (Mode A) + UDP beacons at ANT_BEACON_HZ
 *   - ESP-NOW (Mode B) peer beacons
 *   - Promiscuous per-beacon RSSI (ADR-001 addendum)
 *   - Protocol forward to Mobile (PKT_PROTOCOL chunks)
 *   - Serena: `>` rows from decoded Mobile beacons while session active
 *
 * See docs/SERIAL_PROTOCOL.md, docs/SPEC.md §3.1/§3.3/§5, ADR-004.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "config.h"
#include "protocol.h"
#include "session.h"
#include "logger.h"
#include "serial.h"
#include "rf.h"

static const char *TAG = "main";

void app_main(void)
{
    /* ---- init serial emitter (mutex) before any emit ---- */
    serial_init();

    /* ---- banner (real MAC) ---- */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    serial_emit_banner("ESP32AntTest Station fw 0.3.0");
    serial_emit_banner("MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    serial_emit_banner("Built: " __DATE__);
    serial_emit_banner("IDF: %s", esp_get_idf_version());

    /* ---- init subsystems ---- */
    session_init();
    logger_init();   /* LittleFS mount; non-fatal if it fails (serial-only) */
    ant_rf_init();
    ant_rf_start();      /* SoftAP + UDP + promiscuous + beacon task (Quick-Check) */

    serial_emit_banner("Quick-Check: SoftAP up, beaconing @ %d Hz (Mode A)",
                       ANT_BEACON_HZ);

    /* ---- prompt the host for wall-clock time ---- */
    serial_emit_evt("time_prompt");

    ESP_LOGI(TAG, "Station up; RF Quick-Check + serial command reader");

    /* ---- start the serial command reader ---- */
    serial_start();
}
