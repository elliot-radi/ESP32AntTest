#pragma once
/* OLED live view + test-guide + simple menu (SPEC §3.4/§3.5). */
#include <stdbool.h>
#include <stdint.h>
#include "protocol.h"
#include "button.h"

typedef enum {
    UI_SCREEN_QUICKCHECK = 0,
    UI_SCREEN_MENU,
    UI_SCREEN_GUIDE,     /* guided step prompt */
    UI_SCREEN_SESSION,   /* live RSSI while logging */
} ui_screen_t;

typedef struct {
    ant_mode_t mode;
    int8_t     tx_dbm;
    int8_t     rssi_mob;     /* our measurement of Station (-128 empty) */
    int8_t     rssi_sta;     /* piggybacked from Station */
    uint32_t   samples;
    uint16_t   step_id;
    bool       recording;
    bool       linked;       /* recently heard Station */
    char       guide_prompt[64];
    char       status_line[22];
} ui_state_t;

void ui_init(void);

/* Update model (thread-safe enough for single writer). */
void ui_set_rssi(int8_t rssi_mob, int8_t rssi_sta);
void ui_set_mode(ant_mode_t mode);
void ui_set_tx(int8_t dbm);
void ui_set_recording(bool on);
void ui_set_step(uint16_t step_id);
void ui_set_linked(bool on);
void ui_bump_samples(void);
void ui_set_guide(const char *prompt);
void ui_show(ui_screen_t screen);

ui_screen_t ui_current_screen(void);
const ui_state_t *ui_get(void);

/* Handle a button gesture for menu navigation. Returns true if consumed. */
bool ui_handle_button(btn_evt_t evt);

/* Render at ANT_DISPLAY_HZ. Call from a task. */
void ui_render(void);
