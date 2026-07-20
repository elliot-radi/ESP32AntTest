#pragma once
/* ESP32AntTest — Station session state machine + wall-clock time baseline.
 *
 * See docs/SERIAL_PROTOCOL.md §6/§7 and docs/SPEC.md §3.2/§3.3 (Time Source).
 *
 * Responsibilities:
 *   - Track the Station's top-level operating state: quickcheck (default
 *     power-up, beaconing, no log) vs session (logged run).
 *   - Hold the active session metadata (session_id, mode, tx_mob, tx_sta,
 *     active step_id) set by start_session and read by the logger + RF layer.
 *   - Hold the wall-clock baseline injected over serial via settime, and
 *     render a local datetime string from it + esp_timer (no RTC/SNTP).
 *   - Hold the loaded protocol (set by load_protocol, forwarded to Mobile
 *     at start_session time over RF in a later increment).
 *
 * This module is single-threaded by convention: the serial command task is
 * the only writer; the beacon/log path is a reader. No locking yet — if the
 * RF increment introduces concurrent access, add a mutex around the session
 * struct.
 */
#include <stdbool.h>
#include <stdint.h>
#include "protocol.h"   /* ant_mode_t */

/* Session states (per SERIAL_PROTOCOL.md §7). */
typedef enum {
    STATION_STATE_QUICKCHECK = 0,   /* default power-up: beaconing, no log */
    STATION_STATE_SESSION    = 1,   /* logged session active */
} station_state_t;

/* Active session descriptor. Populated by session_begin(), cleared by
 * session_end(). Read-only to other modules. */
typedef struct {
    bool             active;
    char             session_id[16];   /* "YYYYMMDD_HHMMSS\0" (15 chars + NUL) */
    ant_mode_t       mode;             /* ANT_MODE_WIFI / ANT_MODE_ESPNOW */
    int8_t           tx_mob;           /* dBm */
    int8_t           tx_sta;           /* dBm */
    uint16_t         step_id;          /* active protocol step (0 if none) */
    char             protocol_id[48];  /* id of loaded protocol, "" if none */
} station_session_t;

/* Initialize the session module (call once from app_main before any other
 * session_* call). Seeds the wall-clock baseline to "unset". */
void session_init(void);

/* ---- wall-clock time baseline (SPEC "Time Source") ---- */

/* Inject wall-clock time from the host. Accepts either an ISO-8601-ish
 * datetime "YYYY-MM-DDTHH:MM:SS" or a unix epoch (seconds). Returns 0 on
 * success, -1 on parse error. On success, emits no event — the caller
 * (serial.c) emits {"evt":"time_ok","datetime":...}. */
int session_set_time_iso(const char *iso);
int session_set_time_epoch(int64_t epoch_s);

/* True if a wall-clock baseline has been set since boot. */
bool session_time_is_set(void);

/* Render the current wall-clock datetime into the given buffer as
 * "YYYY-MM-DDTHH:MM:SS". If no baseline is set, writes an empty string and
 * returns 0 (the log datetime column is empty when unset — SPEC §4). */
int session_datetime_now(char *out, int outsz);

/* Generate a new session_id "YYYYMMDD_HHMMSS" from the current wall clock.
 * If time is unset, falls back to "UNKNOWN_<bootcounter>" (SPEC Time Source
 * step 4). Writes into out (16 bytes). Returns 0. */
int session_make_session_id(char *out, int outsz);

/* ---- session lifecycle ---- */

/* Begin a logged session. Generates a session_id, sets mode + tx powers +
 * protocol_id, and transitions to STATION_STATE_SESSION. Returns 0 on
 * success, -1 if a session is already active (caller emits the error event). */
int session_begin(ant_mode_t mode, int8_t tx_mob, int8_t tx_sta,
                  const char *protocol_id);

/* End the active session and transition back to STATION_STATE_QUICKCHECK.
 * Returns 0 on success, -1 if no session is active. Does NOT close the log
 * file (logger.c owns that) — caller calls logger_close() around this. */
int session_end(void);

/* Set the active step_id (advanced by a Mobile marker in a later increment).
 * No-op if no session active. */
void session_set_step(uint16_t step_id);

/* ---- accessors (read-only snapshots) ---- */

station_state_t  session_get_state(void);
const station_session_t *session_get(void);   /* NULL if no active session */
