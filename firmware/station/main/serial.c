/* ESP32AntTest — Station serial protocol: host <-> Station line demux +
 * JSON command dispatch. See serial.h / docs/SERIAL_PROTOCOL.md.
 */
#include "serial.h"
#include "config.h"
#include "session.h"
#include "logger.h"
#include "protocol.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>

static const char *TAG = "serial";

/* stdout is shared between the reader task (writing `$`/`>`) and any other
 * task that emits log rows. A recursive FreeRTOS mutex (not a spinlock)
 * keeps lines atomic AND composes with printf's own internal stdio lock —
 * portENTER_CRITICAL around printf deadlocks/aborts on the ESP32. */
static SemaphoreHandle_t s_print_mu = NULL;

static void print_lock(void)   { if (s_print_mu) xSemaphoreTakeRecursive(s_print_mu, portMAX_DELAY); }
static void print_unlock(void) { if (s_print_mu) xSemaphoreGiveRecursive(s_print_mu); }

/* ---- emitters ---- */

void serial_emit_banner(const char *fmt, ...)
{
    char buf[200];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    /* A `#` line may contain multiple lines; prefix each with `# `. */
    print_lock();
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        printf("# %s\n", line);
        if (!nl) break;
        line = nl + 1;
    }
    fflush(stdout);
    print_unlock();
}

void serial_emit_evt_obj(void *obj)
{
    cJSON *j = (cJSON *)obj;
    char *s = cJSON_PrintUnformatted(j);
    if (s) {
        print_lock();
        printf("$ %s\n", s);
        fflush(stdout);
        print_unlock();
        free(s);
    }
    cJSON_Delete(j);
}

void serial_emit_evt(const char *evt_name)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "evt", evt_name);
    serial_emit_evt_obj(j);
}

static void emit_error(const char *reason)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "evt", "error");
    cJSON_AddStringToObject(j, "reason", reason);
    serial_emit_evt_obj(j);
}

void serial_emit_log(const char *csv_line)
{
    print_lock();
    printf(">%s\n", csv_line);
    fflush(stdout);
    print_unlock();
}

/* ---- command handlers ---- */

static void cmd_hello(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "evt", "hello");
    cJSON_AddStringToObject(j, "board", "station");
    cJSON_AddStringToObject(j, "fw", "0.3.0");
    /* MAC filled at app_main boot; read back from a static here would need
     * nvs/netif — for v1 we leave it to the banner line which already prints
     * the MAC. The host gets board+fw from here. */
    serial_emit_evt_obj(j);
}

static void cmd_settime(cJSON *root)
{
    cJSON *iso = cJSON_GetObjectItem(root, "iso");
    cJSON *ep  = cJSON_GetObjectItem(root, "epoch");
    int rc = -1;
    if (iso && cJSON_IsString(iso)) {
        rc = session_set_time_iso(iso->valuestring);
    } else if (ep && cJSON_IsNumber(ep)) {
        rc = session_set_time_epoch((int64_t)ep->valuedouble);
    }
    if (rc != 0) {
        emit_error("settime: need \"iso\" or \"epoch\"");
        return;
    }
    char dt[24];
    session_datetime_now(dt, sizeof(dt));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "evt", "time_ok");
    cJSON_AddStringToObject(j, "datetime", dt);
    serial_emit_evt_obj(j);
}

static void cmd_load_protocol(cJSON *root)
{
    cJSON *p = cJSON_GetObjectItem(root, "protocol");
    if (!p || !cJSON_IsObject(p)) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "evt", "protocol_error");
        cJSON_AddStringToObject(j, "reason", "missing \"protocol\" object");
        serial_emit_evt_obj(j);
        return;
    }
    /* Minimal structural validation: a protocol is an array of steps under
     * "steps", each with a "type". We accept + remember the id for logging.
     * Full validation + Mobile forward arrives with the RF increment. */
    cJSON *steps = cJSON_GetObjectItem(p, "steps");
    cJSON *pid = cJSON_GetObjectItem(p, "id");
    if (!steps || !cJSON_IsArray(steps)) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "evt", "protocol_error");
        cJSON_AddStringToObject(j, "reason", "protocol.steps not an array");
        serial_emit_evt_obj(j);
        return;
    }
    /* Stash the protocol id in the session module (empty if absent). */
    const char *id_str = (pid && cJSON_IsString(pid)) ? pid->valuestring : "";
    /* We can't store the full protocol JSON yet (no Mobile forward in this
     * increment) — session_begin takes the id only. */
    session_set_step(0);   /* will be advanced by markers later */
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "evt", "protocol_loaded");
    cJSON_AddStringToObject(j, "protocol_id", id_str);
    serial_emit_evt_obj(j);
    ESP_LOGI(TAG, "protocol loaded: id=%s, %d steps",
             id_str, cJSON_GetArraySize(steps));
}

static ant_mode_t parse_mode(const char *s)
{
    if (s && strcmp(s, "ESPNOW") == 0) return ANT_MODE_ESPNOW;
    return ANT_MODE_WIFI;   /* default */
}

static void cmd_start_session(cJSON *root)
{
    if (session_get_state() == STATION_STATE_SESSION) {
        emit_error("start_session: a session is already active");
        return;
    }
    cJSON *m   = cJSON_GetObjectItem(root, "mode");
    cJSON *tm  = cJSON_GetObjectItem(root, "tx_mob");
    cJSON *ts  = cJSON_GetObjectItem(root, "tx_sta");
    ant_mode_t mode = parse_mode(cJSON_IsString(m) ? m->valuestring : NULL);
    int8_t tx_mob = (tm && cJSON_IsNumber(tm)) ? (int8_t)tm->valuedouble : ANT_DEFAULT_TX_POWER;
    int8_t tx_sta = (ts && cJSON_IsNumber(ts)) ? (int8_t)ts->valuedouble : ANT_DEFAULT_TX_POWER;

    if (session_begin(mode, tx_mob, tx_sta, "") != 0) {
        emit_error("start_session: session_begin failed");
        return;
    }
    const station_session_t *s = session_get();

    cJSON *j1 = cJSON_CreateObject();
    cJSON_AddStringToObject(j1, "evt", "session_started");
    cJSON_AddStringToObject(j1, "session_id", s->session_id);
    serial_emit_evt_obj(j1);

    if (logger_open(s->session_id) == 0) {
        cJSON *j2 = cJSON_CreateObject();
        cJSON_AddStringToObject(j2, "evt", "log_opened");
        char fname[40];
        snprintf(fname, sizeof(fname), "%s.csv", s->session_id);
        cJSON_AddStringToObject(j2, "file", fname);
        serial_emit_evt_obj(j2);
    } else {
        emit_error("start_session: log_open failed (serial-only)");
    }
    /* RF: begin beaconing in `mode`, forward protocol to Mobile (PKT_PROTOCOL).
     * Arrives with the RF increment. */
}

static void cmd_end_session(void)
{
    if (session_get_state() != STATION_STATE_SESSION) {
        emit_error("end_session: no active session");
        return;
    }
    const station_session_t *s = session_get();
    char sid[16];
    snprintf(sid, sizeof(sid), "%s", s->session_id);
    /* Close log first, then end session. */
    logger_close();
    cJSON *j2 = cJSON_CreateObject();
    cJSON_AddStringToObject(j2, "evt", "log_closed");
    char fname[40];
    snprintf(fname, sizeof(fname), "%s.csv", sid);
    cJSON_AddStringToObject(j2, "file", fname);
    serial_emit_evt_obj(j2);

    session_end();
    cJSON *j3 = cJSON_CreateObject();
    cJSON_AddStringToObject(j3, "evt", "session_ended");
    cJSON_AddStringToObject(j3, "session_id", sid);
    serial_emit_evt_obj(j3);
}

static void cmd_status(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "evt", "status");
    const char *st = (session_get_state() == STATION_STATE_SESSION) ? "session" : "quickcheck";
    cJSON_AddStringToObject(j, "state", st);
    const station_session_t *s = session_get();
    if (s) {
        cJSON_AddStringToObject(j, "session_id", s->session_id);
        cJSON_AddNumberToObject(j, "step_id", s->step_id);
    }
    serial_emit_evt_obj(j);
}

static void cmd_list_logs(void)
{
    char buf[1024];
    int n = logger_list_files(buf, sizeof(buf));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "evt", "logs");
    cJSON *arr = cJSON_CreateArray();
    if (n > 0) {
        char *line = buf;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (*line) cJSON_AddItemToArray(arr, cJSON_CreateString(line));
            if (!nl) break;
            line = nl + 1;
        }
    }
    cJSON_AddItemToObject(j, "files", arr);
    serial_emit_evt_obj(j);
}

static void cmd_fetch_log(cJSON *root)
{
    cJSON *s = cJSON_GetObjectItem(root, "session");
    if (!s || !cJSON_IsString(s)) {
        emit_error("fetch_log: need \"session\"");
        return;
    }
    uint32_t bytes = 0;
    if (logger_fetch_file(s->valuestring, &bytes) < 0) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "evt", "fetch_error");
        cJSON_AddStringToObject(j, "reason", "no such file");
        serial_emit_evt_obj(j);
        return;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "evt", "fetch_done");
    cJSON_AddNumberToObject(j, "bytes", bytes);
    serial_emit_evt_obj(j);
}

static void cmd_delete_log(cJSON *root)
{
    cJSON *s = cJSON_GetObjectItem(root, "session");
    if (!s || !cJSON_IsString(s)) {
        emit_error("delete_log: need \"session\"");
        return;
    }
    if (logger_delete_file(s->valuestring) != 0) {
        emit_error("delete_log: no such file");
        return;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "evt", "log_deleted");
    char fname[40];
    snprintf(fname, sizeof(fname), "%s", s->valuestring);
    cJSON_AddStringToObject(j, "file", fname);
    serial_emit_evt_obj(j);
}

/* ---- dispatch + line reader ---- */

static void dispatch_command(cJSON *root)
{
    if (!root || !cJSON_IsObject(root)) { emit_error("not an object"); return; }
    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cmd || !cJSON_IsString(cmd)) { emit_error("missing \"cmd\""); return; }
    const char *c = cmd->valuestring;
    if      (strcmp(c, "hello") == 0)        cmd_hello();
    else if (strcmp(c, "settime") == 0)      cmd_settime(root);
    else if (strcmp(c, "load_protocol") == 0) cmd_load_protocol(root);
    else if (strcmp(c, "start_session") == 0) cmd_start_session(root);
    else if (strcmp(c, "end_session") == 0)   cmd_end_session();
    else if (strcmp(c, "status") == 0)        cmd_status();
    else if (strcmp(c, "list_logs") == 0)      cmd_list_logs();
    else if (strcmp(c, "fetch_log") == 0)      cmd_fetch_log(root);
    else if (strcmp(c, "delete_log") == 0)     cmd_delete_log(root);
    else {
        char reason[48];
        snprintf(reason, sizeof(reason), "unknown cmd: %s", c);
        emit_error(reason);
    }
}

/* Process one line from the host. Lines are `#`/`$`/`>` on output; input is
 * `$`-prefixed JSON commands (we ignore other prefixes on input). */
static void handle_line(char *line)
{
    /* Strip trailing CR/LF. */
    size_t L = strlen(line);
    while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
    if (L == 0) return;
    /* Only `$` lines are commands from the host. */
    if (line[0] != '$') return;
    char *json = line + 1;
    while (*json == ' ') json++;   /* tolerate "$ {...}" */
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        emit_error("malformed JSON");
        return;
    }
    dispatch_command(root);
    cJSON_Delete(root);
}

/* Reader task: reads USB-CDC lines from stdin and dispatches complete lines.
 *
 * IMPORTANT: ESP-IDF's USB-CDC VFS does NOT line-buffer on input — fgets/fread
 * return as soon as ANY bytes are available, not when a '\n' arrives. So a
 * command that crosses a USB packet boundary (or arrives in two chunks) is
 * returned as a partial line, and cJSON then sees truncated JSON. We work
 * around this by accumulating bytes into a line buffer until we see '\n',
 * then dispatching the complete line. (Serial input is slow; per-byte fgetc
 * overhead is irrelevant.)
 *
 * Max control line length is 4 KB (SERIAL_PROTOCOL.md §1). */
#define SERIAL_LINE_MAX 4096

static void reader_task(void *arg)
{
    static char line[SERIAL_LINE_MAX];
    int len = 0;
    while (1) {
        int c = fgetc(stdin);
        if (c == EOF) {
            /* No input available right now; back off briefly. */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (c == '\r') continue;   /* ignore CR; we split on \n only */
        if (len < SERIAL_LINE_MAX - 1) {
            line[len++] = (char)c;
        }
        if (c == '\n') {
            line[len] = 0;
            handle_line(line);
            len = 0;
        }
    }
}

void serial_init(void)
{
    s_print_mu = xSemaphoreCreateRecursiveMutex();
}

void serial_start(void)
{
    /* reader_task: needs a generous stack — fopen/LittleFS VFS + cJSON parse
     * + printf formatting all stack up. 8 KB was too small; 12 KB is safe. */
    xTaskCreate(reader_task, "serial_rx", 12288, NULL, 5, NULL);
}
