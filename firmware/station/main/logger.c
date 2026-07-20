/* ESP32AntTest — Station logger: CSV row format + LittleFS durable fallback.
 * See logger.h for the contract; docs/SPEC.md §3.6/§4 for the schema.
 */
#include "logger.h"
#include "config.h"
#include "serial.h"
#include "session.h"   /* session_datetime_now for the datetime column */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>     /* unlink */
#include <dirent.h>
#include <sys/stat.h>
#include <esp_log.h>
#include <esp_littlefs.h>

static const char *TAG = "logger";

/* LittleFS is mounted at ANT_LOG_MOUNT_POINT ("/logs"). The partition is
 * declared in partitions.csv as subtype 0x82 (LittleFS), 1 MB. */
#define LOG_PARTITION_LABEL "logs"

static char  s_open_path[80];
static FILE *s_log_fp = NULL;
static bool  s_mounted = false;

/* Build "/logs/<name>" into `path` (must be >= sizeof(ANT_LOG_MOUNT_POINT)+NAME_MAX+2).
 * Returns the number of bytes written, or -1 on truncation. */
static int build_path(char *path, int pathbufsz, const char *name)
{
    return snprintf(path, pathbufsz, "%s/%s", ANT_LOG_MOUNT_POINT, name);
}

/* rssi is an int8 that may be "empty" (SPEC §4: empty when that direction
 * wasn't measured on this row). We use the sentinel INT8_MIN to mean empty. */
#define RSSI_EMPTY ((int8_t)-128)

static const char *mode_str(ant_mode_t m)
{
    return (m == ANT_MODE_WIFI) ? "WIFI" : "ESPNOW";
}

static const char *source_str(log_source_t s)
{
    return (s == LOG_SRC_STA) ? "STA" : "MOB";
}

static const char *status_str(log_status_t s)
{
    return (s == LOG_STATUS_OK) ? "OK" : "ERR_DECODE";
}

static void fmt_rssi(char *buf, int bufsz, int8_t r)
{
    if (r == RSSI_EMPTY)
        snprintf(buf, bufsz, "%s", "");
    else
        snprintf(buf, bufsz, "%d", r);
}

/* Format one row as CSV (no trailing newline, no `>` prefix). */
static int fmt_row(const log_row_t *row, char *out, int outsz)
{
    char dt[24] = {0};
    session_datetime_now(dt, sizeof(dt));   /* empty if time unset — SPEC §4 */
    char rssi_mob[8], rssi_sta[8];
    fmt_rssi(rssi_mob, sizeof(rssi_mob),
             (row->source == LOG_SRC_STA) ? row->rssi_mob : RSSI_EMPTY);
    fmt_rssi(rssi_sta, sizeof(rssi_sta),
             (row->source == LOG_SRC_MOB) ? RSSI_EMPTY : row->rssi_sta);
    return snprintf(out, outsz,
        "%s,%u,%u,%llu,%s,%s,%d,%d,%s,%s,%s,%s",
        row->session_id ? row->session_id : "",
        (unsigned)row->step_id,
        (unsigned)row->seq,
        (unsigned long long)row->timestamp_ms,
        dt,
        mode_str(row->mode),
        row->tx_mob,
        row->tx_sta,
        rssi_mob,
        rssi_sta,
        source_str(row->source),
        status_str(row->status));
}

int logger_init(void)
{
    if (s_mounted) return 0;

    esp_vfs_littlefs_conf_t conf = {
        .partition_label = LOG_PARTITION_LABEL,
        .base_path       = ANT_LOG_MOUNT_POINT,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS mount failed (%s); logs fall back to serial-only",
                 esp_err_to_name(err));
        return -1;
    }
    s_mounted = true;
    size_t total = 0, used = 0;
    esp_littlefs_info(LOG_PARTITION_LABEL, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted at %s: %u/%u bytes used",
             ANT_LOG_MOUNT_POINT, (unsigned)used, (unsigned)total);
    return 0;
}

/* Evict the oldest .csv file in /logs if there are ANT_LOG_MAX_FILES or more. */
static void maybe_evict_oldest(void)
{
    if (!s_mounted) return;
    DIR *d = opendir(ANT_LOG_MOUNT_POINT);
    if (!d) return;
    char oldest_name[280] = {0};
    time_t oldest_mtime = 0;
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *dot = strrchr(de->d_name, '.');
        if (!dot || strcmp(dot, ".csv") != 0) continue;
        /* Don't trust de->d_type — LittleFS/VFS often leaves it DT_UNKNOWN.
         * stat() instead to confirm it's a regular file + get mtime. */
        char path[300];
        if (build_path(path, sizeof(path), de->d_name) < 0) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;
        count++;
        if (oldest_name[0] == 0 || st.st_mtime < oldest_mtime) {
            oldest_mtime = st.st_mtime;
            snprintf(oldest_name, sizeof(oldest_name), "%s", de->d_name);
        }
    }
    closedir(d);
    if (count >= ANT_LOG_MAX_FILES && oldest_name[0]) {
        char path[300];
        build_path(path, sizeof(path), oldest_name);
        if (unlink(path) == 0)
            ESP_LOGI(TAG, "evicted oldest log (count=%d): %s", count, oldest_name);
        else
            ESP_LOGW(TAG, "evict failed: %s", oldest_name);
    }
}

int logger_open(const char *session_id)
{
    if (s_log_fp) { ESP_LOGW(TAG, "open: already open (%s)", s_open_path); return -1; }
    if (!session_id || !session_id[0]) return -1;
    if (!s_mounted) { ESP_LOGW(TAG, "open: not mounted"); return -1; }
    maybe_evict_oldest();
    snprintf(s_open_path, sizeof(s_open_path), "%s/%s.csv",
             ANT_LOG_MOUNT_POINT, session_id);
    ESP_LOGI(TAG, "opening %s", s_open_path);
    s_log_fp = fopen(s_open_path, "w");
    if (!s_log_fp) {
        ESP_LOGE(TAG, "open failed: %s (errno=%d)", s_open_path, errno);
        return -1;
    }
    /* CSV header (SPEC §4) — matches the locked schema column order. */
    int wrote = fprintf(s_log_fp, "session_id,step_id,seq,timestamp_ms,datetime,mode,"
                      "tx_mob,tx_sta,rssi_mob,rssi_sta,source,status\n");
    fflush(s_log_fp);   /* durable after every beacon (SPEC §2.3) */
    ESP_LOGI(TAG, "opened %s (header %d bytes)", s_open_path, wrote);
    return 0;
}

int logger_emit_row(const log_row_t *row)
{
    if (!s_log_fp) return 0;   /* no log open: silent no-op */
    char line[160];
    fmt_row(row, line, sizeof(line));
    /* Live `>` stream (best-effort) + durable mirror. */
    serial_emit_log(line);
    fprintf(s_log_fp, "%s\n", line);
    fflush(s_log_fp);   /* flush per-row: durable after every beacon (SPEC §2.3) */
    return 0;
}

int logger_close(void)
{
    if (!s_log_fp) return -1;
    fflush(s_log_fp);
    fclose(s_log_fp);
    s_log_fp = NULL;
    ESP_LOGI(TAG, "closed %s", s_open_path);
    s_open_path[0] = 0;
    return 0;
}

int logger_list_files(char *out, int outsz)
{
    if (!s_mounted) { return -1; }
    DIR *d = opendir(ANT_LOG_MOUNT_POINT);
    if (!d) return -1;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *dot = strrchr(de->d_name, '.');
        if (!dot || strcmp(dot, ".csv") != 0) continue;
        char path[300];
        if (build_path(path, sizeof(path), de->d_name) < 0) continue;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        n += snprintf(out + n, outsz - n, "%s\n", de->d_name);
        if (n >= outsz - 1) break;
    }
    closedir(d);
    return n;
}

int logger_fetch_file(const char *session, uint32_t *bytes_out)
{
    if (!s_mounted) return -1;
    /* Accept either a bare session_id or a full filename; build the path. */
    char path[300];
    if (strstr(session, ".csv"))
        build_path(path, sizeof(path), session);
    else {
        char name[280];
        snprintf(name, sizeof(name), "%s.csv", session);
        build_path(path, sizeof(path), name);
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "fetch: no such file %s", path);
        return -1;
    }
    uint32_t total = 0;
    char line[200];
    while (fgets(line, sizeof(line), f)) {
        /* Strip the trailing newline; serial_emit_log adds the `>` prefix. */
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        serial_emit_log(line);
        total += L + 1;   /* +1 for the newline the host will see */
    }
    fclose(f);
    if (bytes_out) *bytes_out = total;
    ESP_LOGI(TAG, "fetch %s: %u bytes", path, (unsigned)total);
    return (int)total;
}

int logger_delete_file(const char *session)
{
    if (!s_mounted) return -1;
    char path[300];
    if (strstr(session, ".csv"))
        build_path(path, sizeof(path), session);
    else {
        char name[280];
        snprintf(name, sizeof(name), "%s.csv", session);
        build_path(path, sizeof(path), name);
    }
    if (unlink(path) != 0) {
        ESP_LOGW(TAG, "delete: %s: %s", path, strerror(errno));
        return -1;
    }
    ESP_LOGI(TAG, "deleted %s", path);
    return 0;
}
