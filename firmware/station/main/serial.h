#pragma once
/* ESP32AntTest — Station serial protocol: host <-> Station line demux +
 * JSON command dispatch + `#`/`$`/`>` emitters.
 *
 * See docs/SERIAL_PROTOCOL.md (the contract). This module owns:
 *   - The serial line reader task: reads USB-CDC lines, demultiplexes by
 *     the first byte (`#` banner / `$` control / `>` log — see §2).
 *   - `$` JSON command parsing (cJSON) and dispatch to the handlers.
 *   - Emitters for `#`/`$`/`>` lines, all `fflush(stdout)`-ed and prefixed
 *     per the channel rules.
 *
 * Status: this increment implements the host-facing command surface
 * (hello/settime/load_protocol/start_session/end_session/status/list_logs/
 * fetch_log/delete_log). Beacon-RX row emission (the `>` rows themselves)
 * arrives with the RF increment; here logger_emit_row() is wired through so
 * a real session opens/closes cleanly and any rows the RF layer produces go
 * out on the right channel.
 */
#include <stdbool.h>

/* ---- emitters (all thread-safe via a stdout mutex) ---- */

/* Emit a `#` banner line (human-readable). */
void serial_emit_banner(const char *fmt, ...);

/* Emit a `$` control event. Takes a cJSON object; takes ownership of it
 * (frees after printing). Convenience: serial_emit_evt("evt_name", ...) builds
 * a {"evt":"..."} object with no other fields. */
void serial_emit_evt_obj(void *cjson_obj);   /* cJSON* */
void serial_emit_evt(const char *evt_name);

/* Emit a `>` log row (CSV line). Caller passes the pre-formatted CSV line
 * (without the `>` prefix); this prepends `>` and flushes. */
void serial_emit_log(const char *csv_line);

/* Initialize the serial emitter (create the stdout mutex). Call once from
 * app_main BEFORE the first serial_emit_* call. */
void serial_init(void);

/* Start the serial reader task (reads USB-CDC lines, demuxes, dispatches
 * `$` commands). Call once from app_main after logger_init/session_init. */
void serial_start(void);
