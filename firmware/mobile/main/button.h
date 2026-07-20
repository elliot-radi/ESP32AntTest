#pragma once
/* Single tactile button: short / long / double gestures (SPEC §3.4). */
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BTN_EVT_NONE = 0,
    BTN_EVT_SHORT,
    BTN_EVT_LONG,
    BTN_EVT_DOUBLE,
} btn_evt_t;

/* Configure GPIO (active-low, pull-up). Call once from app_main. */
void button_init(int gpio_pin);

/* Poll + debounce. Call often (~20 ms). Returns a gesture when recognized
 * (edge-triggered: one event per recognition). */
btn_evt_t button_poll(void);
