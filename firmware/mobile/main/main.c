/* ESP32AntTest — Mobile main.
 *
 * Config A (default): ESP32-C3 + OLED + button.
 * Quick-Check on boot: join Station SoftAP, beacon @ 5 Hz, live RSSI.
 *
 * See docs/SPEC.md §3.3–§3.5, ADR-004.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "board_config.h"
#include "config.h"
#include "button.h"
#include "buffer.h"
#include "ui.h"
#include "mrf.h"

static const char *TAG = "main";

void app_main(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    printf("# ESP32AntTest Mobile fw 0.3.0\n");
    printf("# MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("# pins SDA=%d SCL=%d BTN=%d\n",
           ANT_OLED_SDA_PIN, ANT_OLED_SCL_PIN, ANT_BUTTON_PIN);
    fflush(stdout);

    button_init(ANT_BUTTON_PIN);
    ui_init();
    ant_mrf_init();
    ant_mrf_start();

    ESP_LOGI(TAG, "Mobile up (Quick-Check)");
}
