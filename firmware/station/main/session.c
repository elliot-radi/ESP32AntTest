/* ESP32AntTest — Station session state + wall-clock baseline.
 * See session.h for the contract; docs/SPEC.md "Time Source" for the
 * wall-clock injection rationale (no RTC/SNTP — injected over serial).
 */
#include "session.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <esp_timer.h>
#include <esp_log.h>

static const char *TAG = "session";

/* ---- wall-clock baseline ----
 * Stored as the esp_timer (boot-relative) microsecond reading at the moment
 * the host injected a wall-clock time, plus that wall-clock as a unix epoch
 * second. datetime_now = epoch_s + (now_us - baseline_us) / 1e6. */
static bool     s_time_set = false;
static int64_t  s_baseline_us = 0;   /* esp_timer when time was set */
static int64_t  s_epoch_at_baseline = 0;

/* ---- session ---- */
static station_session_t s_session = {0};
static station_state_t   s_state = STATION_STATE_QUICKCHECK;
static char              s_loaded_protocol_id[48] = {0};

/* Monotonic boot counter for the UNKNOWN_<n> fallback (persisted in NVS in a
 * later increment; for now a boot-static counter). */
static int s_boot_counter = 0;

void session_init(void)
{
    s_time_set = false;
    s_baseline_us = 0;
    s_epoch_at_baseline = 0;
    memset(&s_session, 0, sizeof(s_session));
    s_state = STATION_STATE_QUICKCHECK;
    /* TODO: load s_boot_counter from NVS (persist across boots so
     * UNKNOWN_<n> files don't collide). For now, static 0 is fine — a
     * factory-fresh Station with no time set still produces a valid name. */
}

int session_set_time_epoch(int64_t epoch_s)
{
    s_baseline_us = esp_timer_get_time();
    s_epoch_at_baseline = epoch_s;
    s_time_set = true;
    ESP_LOGI(TAG, "wall clock set: epoch=%lld @ boot_us=%lld",
             (long long)epoch_s, (long long)s_baseline_us);
    return 0;
}

/* Parse "YYYY-MM-DDTHH:MM:SS" (or with a space, or trailing Z/offset) into a
 * unix epoch second. Returns 0 on success, -1 on parse failure.
 * Note: sscanf scansets (%[...]) don't skip leading whitespace and interact
 * poorly with the following %d, so we parse the separator char explicitly. */
static int parse_iso(const char *iso, int64_t *out_epoch)
{
    int y, mo, d, h, mi, se;
    char sep;
    int n = sscanf(iso, "%d-%d-%d%c%d:%d:%d",
                   &y, &mo, &d, &sep, &h, &mi, &se);
    if (n != 7) return -1;
    if (sep != 'T' && sep != ' ') return -1;
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
        h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 60) {
        return -1;
    }
    struct tm t = {0};
    t.tm_year = y - 1900;
    t.tm_mon  = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min  = mi;
    t.tm_sec  = se;
    /* mktime uses local time; we treat the injected datetime as local wall
     * clock (SPEC: "local wall-clock"). The host is the source of truth. */
    time_t e = mktime(&t);
    if (e == (time_t)-1) return -1;
    *out_epoch = (int64_t)e;
    return 0;
}

int session_set_time_iso(const char *iso)
{
    int64_t e;
    if (parse_iso(iso, &e) != 0) {
        ESP_LOGW(TAG, "settime: unparseable iso: %s", iso);
        return -1;
    }
    return session_set_time_epoch(e);
}

bool session_time_is_set(void) { return s_time_set; }

int session_datetime_now(char *out, int outsz)
{
    if (!s_time_set) {
        if (outsz > 0) out[0] = '\0';
        return 0;
    }
    int64_t now_epoch = s_epoch_at_baseline +
                        (esp_timer_get_time() - s_baseline_us) / 1000000;
    time_t t = (time_t)now_epoch;
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(out, outsz, "%04d-%02d-%02dT%02d:%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return 0;
}

int session_make_session_id(char *out, int outsz)
{
    if (s_time_set) {
        int64_t now_epoch = s_epoch_at_baseline +
                            (esp_timer_get_time() - s_baseline_us) / 1000000;
        time_t t = (time_t)now_epoch;
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(out, outsz, "%04d%02d%02d_%02d%02d%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        snprintf(out, outsz, "UNKNOWN_%d", s_boot_counter);
    }
    return 0;
}

/* ---- session lifecycle ---- */

int session_begin(ant_mode_t mode, int8_t tx_mob, int8_t tx_sta,
                  const char *protocol_id)
{
    if (s_session.active) return -1;
    memset(&s_session, 0, sizeof(s_session));
    session_make_session_id(s_session.session_id, sizeof(s_session.session_id));
    s_session.active = true;
    s_session.mode    = mode;
    s_session.tx_mob  = tx_mob;
    s_session.tx_sta  = tx_sta;
    s_session.step_id = 0;
    if (protocol_id && protocol_id[0])
        snprintf(s_session.protocol_id, sizeof(s_session.protocol_id), "%s", protocol_id);
    else if (s_loaded_protocol_id[0])
        snprintf(s_session.protocol_id, sizeof(s_session.protocol_id), "%s",
                 s_loaded_protocol_id);
    s_state = STATION_STATE_SESSION;
    ESP_LOGI(TAG, "session begin: %s mode=%d tx_mob=%d tx_sta=%d proto=%s",
             s_session.session_id, mode, tx_mob, tx_sta,
             s_session.protocol_id[0] ? s_session.protocol_id : "(none)");
    return 0;
}

int session_end(void)
{
    if (!s_session.active) return -1;
    ESP_LOGI(TAG, "session end: %s", s_session.session_id);
    memset(&s_session, 0, sizeof(s_session));
    s_state = STATION_STATE_QUICKCHECK;
    return 0;
}

void session_set_step(uint16_t step_id)
{
    if (s_session.active) s_session.step_id = step_id;
}

void session_set_protocol_id(const char *protocol_id)
{
    if (!protocol_id) {
        s_loaded_protocol_id[0] = 0;
        return;
    }
    snprintf(s_loaded_protocol_id, sizeof(s_loaded_protocol_id), "%s", protocol_id);
}

const char *session_get_protocol_id(void)
{
    return s_loaded_protocol_id;
}

station_state_t session_get_state(void) { return s_state; }

const station_session_t *session_get(void)
{
    return s_session.active ? &s_session : NULL;
}
