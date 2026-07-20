#include "ui.h"
#include "board_config.h"
#include "config.h"
#include "oled_text.h"
#include "mrf.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "ui";
static ui_state_t  s_ui;
static ui_screen_t s_screen = UI_SCREEN_QUICKCHECK;
static int         s_menu_idx = 0;
static bool        s_oled_ok = false;

/* Menu items (top-level). Logged sessions are host-started (SPEC §3.3/DI-11);
 * no ad-hoc Manual/Auto entry. */
enum {
    MENU_MODE = 0,
    MENU_TX,
    MENU_END_SESSION,
    MENU_BACK,
    MENU_N
};
static const char *s_menu_labels[MENU_N] = {
    "Mode",
    "TX Power",
    "End Session",
    "Back",
};

static const int8_t s_tx_choices[] = {
    ANT_TX_POWER_LOW, ANT_TX_POWER_MED, ANT_TX_POWER_HIGH, ANT_TX_POWER_MAX
};
#define N_TX ((int)(sizeof(s_tx_choices)/sizeof(s_tx_choices[0])))

void ui_init(void)
{
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.mode = ANT_MODE_WIFI;
    s_ui.tx_dbm = ANT_DEFAULT_TX_POWER;
    s_ui.rssi_mob = (int8_t)-128;
    s_ui.rssi_sta = (int8_t)-128;
    s_screen = UI_SCREEN_QUICKCHECK;

    esp_err_t e = oled_text_init(ANT_OLED_SDA_PIN, ANT_OLED_SCL_PIN,
                                 ANT_OLED_I2C_ADDR, 400000);
    s_oled_ok = (e == ESP_OK);
    if (!s_oled_ok) {
        ESP_LOGW(TAG, "OLED init failed: %s", esp_err_to_name(e));
    } else {
        oled_text_clear();
        oled_text_puts(0, 0, "ESP32AntTest");
        oled_text_puts(1, 0, "Mobile boot...");
        oled_text_refresh();
    }
}

void ui_set_rssi(int8_t mob, int8_t sta)
{
    s_ui.rssi_mob = mob;
    s_ui.rssi_sta = sta;
}
void ui_set_mode(ant_mode_t m) { s_ui.mode = m; }
void ui_set_tx(int8_t d) { s_ui.tx_dbm = d; }
void ui_set_recording(bool on) { s_ui.recording = on; }
void ui_set_step(uint16_t id) { s_ui.step_id = id; }
void ui_set_linked(bool on) { s_ui.linked = on; }
void ui_bump_samples(void) { s_ui.samples++; }
void ui_set_guide(const char *p)
{
    if (!p) { s_ui.guide_prompt[0] = 0; return; }
    snprintf(s_ui.guide_prompt, sizeof(s_ui.guide_prompt), "%s", p);
}
void ui_show(ui_screen_t s) { s_screen = s; }
ui_screen_t ui_current_screen(void) { return s_screen; }
const ui_state_t *ui_get(void) { return &s_ui; }

static void cycle_tx(void)
{
    int i;
    for (i = 0; i < N_TX; i++) if (s_tx_choices[i] == s_ui.tx_dbm) break;
    i = (i + 1) % N_TX;
    s_ui.tx_dbm = s_tx_choices[i];
    ant_mrf_set_tx_power(s_ui.tx_dbm);
}

static void toggle_mode(void)
{
    s_ui.mode = (s_ui.mode == ANT_MODE_WIFI) ? ANT_MODE_ESPNOW : ANT_MODE_WIFI;
    ant_mrf_set_mode(s_ui.mode);
}

bool ui_handle_button(btn_evt_t evt)
{
    if (evt == BTN_EVT_NONE) return false;

    if (s_screen == UI_SCREEN_QUICKCHECK) {
        if (evt == BTN_EVT_LONG) {
            s_screen = UI_SCREEN_MENU;
            s_menu_idx = 0;
            return true;
        }
        return false;
    }

    if (s_screen == UI_SCREEN_SESSION) {
        if (evt == BTN_EVT_LONG) {
            s_screen = UI_SCREEN_MENU;
            s_menu_idx = 0;
            return true;
        }
        if (evt == BTN_EVT_SHORT) {
            /* Ready / next guided step. */
            ant_mrf_on_short_press();
            return true;
        }
        return false;
    }

    if (s_screen == UI_SCREEN_GUIDE) {
        if (evt == BTN_EVT_SHORT) {
            /* Ready on this step → live session (or mark protocol done). */
            ant_mrf_on_short_press();
            return true;
        }
        if (evt == BTN_EVT_LONG) {
            s_screen = UI_SCREEN_MENU;
            s_menu_idx = 0;
            return true;
        }
        return true;
    }

    if (s_screen == UI_SCREEN_MENU) {
        if (evt == BTN_EVT_SHORT) {
            s_menu_idx = (s_menu_idx + 1) % MENU_N;
            return true;
        }
        if (evt == BTN_EVT_DOUBLE || (evt == BTN_EVT_LONG && s_menu_idx == MENU_BACK)) {
            s_screen = s_ui.recording ? UI_SCREEN_SESSION : UI_SCREEN_QUICKCHECK;
            return true;
        }
        if (evt == BTN_EVT_LONG) {
            switch (s_menu_idx) {
            case MENU_MODE:
                toggle_mode();
                break;
            case MENU_TX:
                cycle_tx();
                break;
            case MENU_END_SESSION:
                ant_mrf_end_session();
                s_screen = UI_SCREEN_QUICKCHECK;
                break;
            case MENU_BACK:
                s_screen = s_ui.recording ? UI_SCREEN_SESSION : UI_SCREEN_QUICKCHECK;
                break;
            }
            return true;
        }
    }
    return false;
}

static void fmt_rssi(char *b, int n, int8_t r)
{
    if (r == (int8_t)-128) snprintf(b, n, "---");
    else snprintf(b, n, "%d", (int)r);
}

void ui_render(void)
{
    if (!s_oled_ok) return;
    oled_text_clear();
    char line[24];

    if (s_screen == UI_SCREEN_MENU) {
        oled_text_puts(0, 0, "MENU");
        for (int i = 0; i < MENU_N && i < 6; i++) {
            char row[22];
            snprintf(row, sizeof(row), "%c%s",
                     (i == s_menu_idx) ? '>' : ' ', s_menu_labels[i]);
            /* append live value on mode/tx rows */
            if (i == MENU_MODE) {
                snprintf(row, sizeof(row), "%cMode: %s",
                         (i == s_menu_idx) ? '>' : ' ',
                         s_ui.mode == ANT_MODE_WIFI ? "WIFI" : "ESPNOW");
            } else if (i == MENU_TX) {
                snprintf(row, sizeof(row), "%cTX: %d dBm",
                         (i == s_menu_idx) ? '>' : ' ', s_ui.tx_dbm);
            }
            oled_text_puts(i + 1, 0, row);
        }
        oled_text_refresh();
        return;
    }

    if (s_screen == UI_SCREEN_GUIDE && s_ui.guide_prompt[0]) {
        int idx = ant_mrf_step_index();
        int nst = ant_mrf_step_count();
        if (nst > 0 && idx >= 0) {
            unsigned a = (unsigned)(idx + 1);
            unsigned b = (unsigned)nst;
            if (a > 99) a = 99;
            if (b > 99) b = 99;
            snprintf(line, sizeof(line), "Step %u/%u", a, b);
        }
        else
            snprintf(line, sizeof(line), "Mode:%s P:%d",
                     s_ui.mode == ANT_MODE_WIFI ? "WIFI" : "ENOW", s_ui.tx_dbm);
        oled_text_puts(0, 0, line);
        /* Word-wrap prompt into rows 2-5. */
        const char *p = s_ui.guide_prompt;
        int row = 2;
        while (*p && row < 6) {
            char chunk[22];
            int k = 0;
            while (*p && k < 21) chunk[k++] = *p++;
            chunk[k] = 0;
            oled_text_puts(row++, 0, chunk);
        }
        oled_text_puts(7, 0, "short=ready");
        oled_text_refresh();
        return;
    }

    /* Quick-Check / Session live view — SPEC §3.5 */
    snprintf(line, sizeof(line), "Mode:%s Pwr:%d",
             s_ui.mode == ANT_MODE_WIFI ? "WIFI" : "ENOW", s_ui.tx_dbm);
    oled_text_puts(0, 0, line);

    char rm[8], rs[8];
    fmt_rssi(rm, sizeof(rm), s_ui.rssi_mob);
    fmt_rssi(rs, sizeof(rs), s_ui.rssi_sta);
    snprintf(line, sizeof(line), "Mob RSSI: %s", rm);
    oled_text_puts(1, 0, line);
    snprintf(line, sizeof(line), "Sta RSSI: %s", rs);
    oled_text_puts(2, 0, line);
    snprintf(line, sizeof(line), "Smpl:%lu Step:%u",
             (unsigned long)s_ui.samples, (unsigned)s_ui.step_id);
    oled_text_puts(3, 0, line);

    if (s_ui.recording) {
        oled_text_puts(4, 0, "[RECORDING *]");
    } else {
        oled_text_puts(4, 0, s_ui.linked ? "[QUICK-CHECK]" : "[NO LINK]");
    }
    oled_text_puts(6, 0, "long=menu");
    oled_text_refresh();
}
