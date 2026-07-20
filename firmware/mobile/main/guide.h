#pragma once
/* Guided protocol: step list from host-forwarded PKT_PROTOCOL JSON.
 * Mobile walks the operator through each step prompt; short-press =
 * "ready" then overs to live session for that step; next short-press
 * advances to the next step. See SPEC §3.2/§3.3. */
#include <stdbool.h>
#include <stdint.h>

#define GUIDE_MAX_STEPS     32
#define GUIDE_PROMPT_MAX    80

typedef enum {
    GUIDE_TYPE_DISTANCE    = 0,
    GUIDE_TYPE_ORIENTATION = 1,
    GUIDE_TYPE_SOAK        = 2,
    GUIDE_TYPE_FREE        = 3,
} guide_type_t;

typedef struct {
    uint16_t     step_id;
    guide_type_t type;
    char         prompt[GUIDE_PROMPT_MAX];
    /* Optional condition fields (0 / empty if unused). */
    float        distance_m;
    char         axis;          /* 'X'/'Y'/'Z' or 0 */
    int16_t      angle_deg;
    uint16_t     duration_s;
} guide_step_t;

/* Parse protocol JSON into the step list. Accepts either
 *   {"id"|"protocol_id":"...","steps":[...]}
 * Replaces any previously loaded protocol. Returns 0 on success,
 * -1 if no usable steps. */
int  guide_load_json(const char *json);

void guide_clear(void);
bool guide_is_loaded(void);
int  guide_step_count(void);

/* Current step cursor (0-based into the loaded list). -1 if none/done. */
int  guide_cursor(void);
const guide_step_t *guide_current(void);

/* Jump to first step (or a specific step_id). Returns 0 if found. */
int  guide_begin(void);
int  guide_goto_step_id(uint16_t step_id);

/* Advance cursor to next step. Returns the new step, or NULL if past end
 * (protocol complete — caller should end session / show done). */
const guide_step_t *guide_advance(void);

/* True if cursor is past the last step. */
bool guide_done(void);

const char *guide_protocol_id(void);
