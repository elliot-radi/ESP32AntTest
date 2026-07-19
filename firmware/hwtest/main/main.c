/* ESP32AntTest — hwtest: C3 board bring-up (OLED + button)
 *
 * Validates the wiring described in docs/HARDWARE.md before building the real
 * Mobile firmware on top of it. Self-contained: no WiFi, no protocol, no logs.
 *
 * What it does:
 *   1. I2C bus scan (0x03..0x77), prints found addresses to serial + OLED.
 *      A bare ACK at 0x3C proves SDA/SCL/VCC/GND before we try to draw.
 *   2. SSD1306 init + render a known pattern (title, scan result, live button).
 *   3. Continuously read the button (active-low, internal pull-up), debounce
 *      20 ms, echo the live state to OLED row 3, and bump a press counter.
 *
 * Pin overrides live in board_config.h (C3 values: SDA=8, SCL=9, Button=5),
 * included below before config.h so its #ifndef guards pick them up.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_err.h"
#include "board_config.h"   /* C3 pin overrides — must precede config.h */
#include "config.h"
#include "oled_text.h"

/* C3 Mobile pins (from board_config.h via config.h / HARDWARE.md). */
#define I2C_SDA    ANT_OLED_SDA_PIN
#define I2C_SCL    ANT_OLED_SCL_PIN
#define BTN_PIN    ANT_BUTTON_PIN
#define OLED_ADDR  ANT_OLED_I2C_ADDR
#define I2C_HZ     400000

static int s_press_count = 0;

static void i2c_scan(i2c_master_bus_handle_t bus, char *out, int outsz)
{
    int n = 0;
    n += snprintf(out + n, outsz - n, "I2C:");
    for (uint16_t a = 0x03; a <= 0x77; a++) {
        if (i2c_master_probe(bus, a, 20) == ESP_OK)
            n += snprintf(out + n, outsz - n, " 0x%02X", a);
        /* Yield each iteration so the Task Watchdog doesn't reset during a
         * full bus scan (worst case ~117 probes with no devices responding). */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (n == (int)strlen("I2C:"))
        snprintf(out + n, outsz - n, " (none)");
}

void app_main(void)
{
    printf("# ESP32AntTest hwtest\n");
    printf("# C3 pins: SDA=%d SCL=%d BTN=%d OLED=0x%02X\n", I2C_SDA, I2C_SCL, BTN_PIN, OLED_ADDR);
    fflush(stdout);

    /* --- Button GPIO: input + internal pull-up (active-low, no external R) --- */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BTN_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    /* --- I2C bus for the scan (oled_text creates its own bus later; we
     *     delete this one first to avoid a pin conflict) --- */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = (gpio_num_t)I2C_SDA,
        .scl_io_num = (gpio_num_t)I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    char scan[96];
    i2c_scan(bus, scan, sizeof(scan));
    printf("# %s\n", scan);
    fflush(stdout);

    /* Free the scan bus so oled_text can claim the pins */
    i2c_del_master_bus(bus);

    /* --- OLED init + initial render --- */
    esp_err_t e = oled_text_init(I2C_SDA, I2C_SCL, OLED_ADDR, I2C_HZ);
    if (e != ESP_OK) {
        printf("$ {\"evt\":\"error\",\"reason\":\"oled_init %s\"}\n", esp_err_to_name(e));
        fflush(stdout);
    } else {
        oled_text_clear();
        oled_text_puts(0, 0, "ESP32AntTest");
        oled_text_puts(1, 0, "hwtest C3");
        oled_text_puts(7, 0, scan);
        oled_text_refresh();
    }

    /* --- Main loop: echo button + count presses --- */
    bool last_pressed = false;
    char line[40];
    while (1) {
        bool pressed = (gpio_get_level(BTN_PIN) == 0);
        /* 20 ms software debounce: re-read and only accept if stable */
        vTaskDelay(pdMS_TO_TICKS(ANT_BTN_DEBOUNCE_MS));
        if (pressed == (gpio_get_level(BTN_PIN) == 0)) {
            if (pressed && !last_pressed)
                s_press_count++;
            last_pressed = pressed;
        }

        /* OLED is 21 cols (OLED_TEXT_COLS); keep the line ≤ 21 chars so the
         * count doesn't run off the right edge. Both state tokens are 7 chars
         * ("PRESSED" / "-------") so the count column stays aligned. */
        snprintf(line, sizeof(line), "Btn: %s count=%d",
                 last_pressed ? "PRESSED" : "-------", s_press_count);
        if (e == ESP_OK) {
            oled_text_puts(3, 0, line);
            oled_text_refresh();
        }
        printf("$ {\"btn\":%s,\"count\":%d}\r\n",
               last_pressed ? "true " : "false", s_press_count);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
