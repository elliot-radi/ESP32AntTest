#include "button.h"
#include "board_config.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static int      s_pin = ANT_BUTTON_PIN;
static bool     s_stable_down = false;
static bool     s_raw_down    = false;
static int64_t  s_debounce_deadline_us = 0;
static int64_t  s_down_at_us  = 0;
static int64_t  s_up_at_us    = 0;
static bool     s_long_fired  = false;
static int      s_pending_short = 0;  /* waiting for possible double */

void button_init(int gpio_pin)
{
    s_pin = gpio_pin;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << s_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

btn_evt_t button_poll(void)
{
    int64_t now = esp_timer_get_time();
    bool raw = (gpio_get_level(s_pin) == 0);

    if (raw != s_raw_down) {
        s_raw_down = raw;
        s_debounce_deadline_us = now + (int64_t)ANT_BTN_DEBOUNCE_MS * 1000;
    }
    if (now < s_debounce_deadline_us) {
        /* Still reconciling bounce; but check double-press timer. */
    } else if (s_stable_down != s_raw_down) {
        s_stable_down = s_raw_down;
        if (s_stable_down) {
            s_down_at_us = now;
            s_long_fired = false;
        } else {
            /* Release. */
            int64_t held_ms = (now - s_down_at_us) / 1000;
            s_up_at_us = now;
            if (!s_long_fired && held_ms < ANT_BTN_SHORT_MAX_MS) {
                if (s_pending_short) {
                    s_pending_short = 0;
                    return BTN_EVT_DOUBLE;
                }
                s_pending_short = 1;
            }
        }
    }

    /* Long press: fire once while still held. */
    if (s_stable_down && !s_long_fired) {
        int64_t held_ms = (now - s_down_at_us) / 1000;
        if (held_ms >= ANT_BTN_LONG_MIN_MS) {
            s_long_fired = true;
            s_pending_short = 0;
            return BTN_EVT_LONG;
        }
    }

    /* Commit a short press after the double-gap window expires. */
    if (s_pending_short && !s_stable_down) {
        int64_t since_up_ms = (now - s_up_at_us) / 1000;
        if (since_up_ms >= ANT_BTN_DOUBLE_GAP_MS) {
            s_pending_short = 0;
            return BTN_EVT_SHORT;
        }
    }
    return BTN_EVT_NONE;
}
