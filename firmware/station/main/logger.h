#pragma once
/* ESP32AntTest — Station logger: CSV row format + LittleFS durable fallback.
 *
 * See docs/SPEC.md §3.6/§4 (log schema) and docs/SERIAL_PROTOCOL.md §5
 * (log channel).
 *
 * Every logged row is emitted on the serial `>` channel (live, best-effort)
 * AND mirrored to a LittleFS file (durable). The LittleFS file is the
 * recovery source the host fetches with fetch_log if the serial stream was
 * interrupted (SERIAL_PROTOCOL.md §5).
 *
 * One LittleFS file per session: /logs/<session_id>.csv  (ANT_LOG_MOUNT_POINT).
 * At most ANT_LOG_MAX_FILES files retained (oldest evicted on open — SPEC §8).
 *
 * Schema (SPEC §4), one row per beacon:
 *   session_id,step_id,seq,timestamp_ms,datetime,mode,tx_mob,tx_sta,
 *   rssi_mob,rssi_sta,source,status
 */
#include <stdbool.h>
#include <stdint.h>
#include "protocol.h"   /* ant_mode_t */

/* Log row provenance (SPEC §4 `source`). */
typedef enum {
    LOG_SRC_STA = 0,   /* Station decoded Mobile's beacon; both RSSI via piggyback */
    LOG_SRC_MOB = 1,   /* Mobile buffered during uplink outage; rssi_sta empty */
} log_source_t;

/* Log row status (SPEC §4 `status`; no TIMEOUT under beacon mode). */
typedef enum {
    LOG_STATUS_OK        = 0,
    LOG_STATUS_ERR_DECODE = 1,
} log_status_t;

/* A complete log row (one decoded beacon = one row). */
typedef struct {
    const char   *session_id;   /* points into session struct; not copied */
    uint16_t      step_id;
    uint32_t      seq;
    uint64_t      timestamp_ms; /* boot-relative (esp_timer) */
    ant_mode_t    mode;
    int8_t        tx_mob;
    int8_t        tx_sta;
    int8_t        rssi_mob;     /* RSSI at Mobile of Station's beacon (may be empty) */
    int8_t        rssi_sta;     /* RSSI at Station of Mobile's beacon (may be empty) */
    log_source_t  source;
    log_status_t  status;
} log_row_t;

/* Mount the LittleFS logs partition. Call once from app_main. Returns 0 on
 * success, -1 on failure (logs fall back to serial-only; non-fatal). */
int logger_init(void);

/* Open a new session log file: /logs/<session_id>.csv and write the CSV
 * header line. Evicts the oldest file if at ANT_LOG_MAX_FILES. Returns 0 on
 * success, -1 on failure. */
int logger_open(const char *session_id);

/* Emit one log row: format the CSV line, write it to the `>` serial channel
 * (live, best-effort) AND append to the open LittleFS file (durable).
 * No-op (returns 0) if no log is open. Returns 0 on success, -1 on error. */
int logger_emit_row(const log_row_t *row);

/* Close the current session's LittleFS file (flush + close). Returns 0 on
 * success, -1 if no log open or on error. */
int logger_close(void);

/* ---- LittleFS file management (for serial.c fetch_log/list_logs/delete_log) ---- */

/* Fill `out` with a newline-separated list of .csv filenames in /logs
 * (basename only, no path). `outsz` is the buffer size. Returns the number
 * of bytes written (excluding NUL), or -1 on error. */
int logger_list_files(char *out, int outsz);

/* Stream the contents of /logs/<session>.csv (or any matching filename)
 * on the `>` channel, then return. Caller emits fetch_done/fetch_error on
 * the `$` channel. Returns bytes streamed on success, -1 if no such file. */
int logger_fetch_file(const char *session, uint32_t *bytes_out);

/* Delete /logs/<session>.csv. Returns 0 on success, -1 if not found/error. */
int logger_delete_file(const char *session);
