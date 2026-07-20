#pragma once
/* Mobile outage RAM ring buffer (ADR-004 / SPEC §2.2).
 * Stores beacon RSSI samples + markers while Station isn't hearing us
 * (or while RF is down), drained on reconnect via forward. */
#include <stdbool.h>
#include <stdint.h>
#include "protocol.h"

typedef enum {
    BUF_KIND_RSSI   = 1,  /* Mobile decoded Station beacon during uplink outage */
    BUF_KIND_MARKER = 2,  /* button step-advance */
} buf_kind_t;

typedef struct {
    buf_kind_t kind;
    uint32_t   seq;
    uint16_t   step_id;
    int8_t     rssi_mob;   /* Mobile's measurement of Station */
    int8_t     rssi_sta;   /* piggybacked from Station (may be -128) */
    int8_t     tx_mob;
    int8_t     tx_sta;
    ant_mode_t mode;
    uint32_t   timestamp_ms; /* boot-relative ms when captured */
} buf_entry_t;

void buffer_init(void);

/* Push; overwrites oldest if full. Returns false if dropped (shouldn't). */
bool buffer_push(const buf_entry_t *e);

/* Pop oldest into *out. Returns false if empty. */
bool buffer_pop(buf_entry_t *out);

int  buffer_count(void);
void buffer_clear(void);
