/* ESP32AntTest — Station main.
 *
 * Increment 1: serial protocol + boot + LittleFS (no RF yet).
 *   - Real boot banner (real MAC) + $ time_prompt
 *   - LittleFS mounted
 *   - serial reader task dispatching host commands (hello/settime/
 *     load_protocol/start_session/end_session/status/list_logs/fetch_log/
 *     delete_log)
 *   - session state machine + wall-clock baseline
 * Beacon RX, RSSI logging, protocol forward to Mobile, and ESP-NOW arrive
 * with the RF increment.
 *
 * See docs/SERIAL_PROTOCOL.md (contract), docs/SPEC.md §3.3/§3.6.
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
    serial_emit_banner("Quick-Check beaconing not yet implemented (Increment 1)");

    /* ---- init subsystems ---- */
    session_init();
    logger_init();   /* LittleFS mount; non-fatal if it fails (serial-only) */

    /* ---- prompt the host for wall-clock time ---- */
    serial_emit_evt("time_prompt");

    ESP_LOGI(TAG, "Station up; waiting for host commands on serial");

    /* ---- start the serial command reader ---- */
    serial_start();

    /* app_main returning is fine; the reader task keeps running. Quick-Check
     * beaconing (when implemented) will be its own task, started here. */
}
