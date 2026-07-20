/* ESP32AntTest — Mobile guided-protocol step list (from PKT_PROTOCOL JSON). */
#include "guide.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "guide";

static guide_step_t s_steps[GUIDE_MAX_STEPS];
static int          s_n = 0;
static int          s_cur = -1;           /* -1 = not begun / cleared */
static char         s_proto_id[48] = {0};
static bool         s_loaded = false;

static guide_type_t parse_type(const char *s)
{
    if (!s) return GUIDE_TYPE_FREE;
    if (strcmp(s, "distance") == 0)    return GUIDE_TYPE_DISTANCE;
    if (strcmp(s, "orientation") == 0) return GUIDE_TYPE_ORIENTATION;
    if (strcmp(s, "soak") == 0)        return GUIDE_TYPE_SOAK;
    return GUIDE_TYPE_FREE;
}

void guide_clear(void)
{
    memset(s_steps, 0, sizeof(s_steps));
    s_n = 0;
    s_cur = -1;
    s_proto_id[0] = 0;
    s_loaded = false;
}

int guide_load_json(const char *json)
{
    guide_clear();
    if (!json || !json[0]) return -1;

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "protocol JSON parse failed");
        return -1;
    }

    cJSON *pid = cJSON_GetObjectItem(root, "protocol_id");
    if (!pid) pid = cJSON_GetObjectItem(root, "id");
    if (pid && cJSON_IsString(pid)) {
        snprintf(s_proto_id, sizeof(s_proto_id), "%s", pid->valuestring);
    }

    cJSON *steps = cJSON_GetObjectItem(root, "steps");
    if (!steps || !cJSON_IsArray(steps)) {
        ESP_LOGW(TAG, "protocol.steps missing or not array");
        cJSON_Delete(root);
        return -1;
    }

    int n = cJSON_GetArraySize(steps);
    if (n > GUIDE_MAX_STEPS) n = GUIDE_MAX_STEPS;
    int kept = 0;
    for (int i = 0; i < n; i++) {
        cJSON *st = cJSON_GetArrayItem(steps, i);
        if (!st || !cJSON_IsObject(st)) continue;

        guide_step_t *g = &s_steps[kept];
        memset(g, 0, sizeof(*g));

        cJSON *sid = cJSON_GetObjectItem(st, "step_id");
        g->step_id = (sid && cJSON_IsNumber(sid))
                         ? (uint16_t)sid->valuedouble
                         : (uint16_t)(kept + 1);

        cJSON *ty = cJSON_GetObjectItem(st, "type");
        g->type = parse_type(cJSON_IsString(ty) ? ty->valuestring : NULL);

        cJSON *pr = cJSON_GetObjectItem(st, "prompt");
        if (pr && cJSON_IsString(pr)) {
            snprintf(g->prompt, sizeof(g->prompt), "%s", pr->valuestring);
        } else {
            /* Synthesize a bare prompt from the type + condition. */
            switch (g->type) {
            case GUIDE_TYPE_DISTANCE: {
                cJSON *d = cJSON_GetObjectItem(st, "distance_m");
                float m = (d && cJSON_IsNumber(d)) ? (float)d->valuedouble : 0;
                snprintf(g->prompt, sizeof(g->prompt),
                         "Go to %.0fm, press ready", (double)m);
                break;
            }
            case GUIDE_TYPE_ORIENTATION: {
                cJSON *ax = cJSON_GetObjectItem(st, "axis");
                cJSON *ang = cJSON_GetObjectItem(st, "angle_deg");
                char a = (ax && cJSON_IsString(ax) && ax->valuestring[0])
                             ? ax->valuestring[0] : '?';
                int deg = (ang && cJSON_IsNumber(ang)) ? (int)ang->valuedouble : 0;
                snprintf(g->prompt, sizeof(g->prompt),
                         "Rotate %c to %d deg", a, deg);
                break;
            }
            case GUIDE_TYPE_SOAK: {
                cJSON *dur = cJSON_GetObjectItem(st, "duration_s");
                int sec = (dur && cJSON_IsNumber(dur)) ? (int)dur->valuedouble : 0;
                snprintf(g->prompt, sizeof(g->prompt),
                         "Soak %ds, press ready", sec);
                break;
            }
            default:
                snprintf(g->prompt, sizeof(g->prompt), "Step %u, press ready",
                         (unsigned)g->step_id);
                break;
            }
        }

        cJSON *dm = cJSON_GetObjectItem(st, "distance_m");
        if (dm && cJSON_IsNumber(dm)) g->distance_m = (float)dm->valuedouble;

        cJSON *ax = cJSON_GetObjectItem(st, "axis");
        if (ax && cJSON_IsString(ax) && ax->valuestring[0])
            g->axis = ax->valuestring[0];

        cJSON *ang = cJSON_GetObjectItem(st, "angle_deg");
        if (ang && cJSON_IsNumber(ang)) g->angle_deg = (int16_t)ang->valuedouble;

        cJSON *dur = cJSON_GetObjectItem(st, "duration_s");
        if (dur && cJSON_IsNumber(dur)) g->duration_s = (uint16_t)dur->valuedouble;

        kept++;
    }
    cJSON_Delete(root);

    s_n = kept;
    s_loaded = (kept > 0);
    s_cur = -1;
    ESP_LOGI(TAG, "loaded protocol id=%s steps=%d",
             s_proto_id[0] ? s_proto_id : "(none)", s_n);
    return s_loaded ? 0 : -1;
}

bool guide_is_loaded(void) { return s_loaded; }
int  guide_step_count(void) { return s_n; }
int  guide_cursor(void) { return s_cur; }
const char *guide_protocol_id(void) { return s_proto_id; }

const guide_step_t *guide_current(void)
{
    if (!s_loaded || s_cur < 0 || s_cur >= s_n) return NULL;
    return &s_steps[s_cur];
}

int guide_begin(void)
{
    if (!s_loaded || s_n == 0) return -1;
    s_cur = 0;
    return 0;
}

int guide_goto_step_id(uint16_t step_id)
{
    if (!s_loaded) return -1;
    for (int i = 0; i < s_n; i++) {
        if (s_steps[i].step_id == step_id) {
            s_cur = i;
            return 0;
        }
    }
    return -1;
}

const guide_step_t *guide_advance(void)
{
    if (!s_loaded) return NULL;
    if (s_cur < 0) {
        s_cur = 0;
        return &s_steps[0];
    }
    if (s_cur + 1 >= s_n) {
        s_cur = s_n;   /* past end */
        return NULL;
    }
    s_cur++;
    return &s_steps[s_cur];
}

bool guide_done(void)
{
    return s_loaded && s_cur >= s_n;
}
