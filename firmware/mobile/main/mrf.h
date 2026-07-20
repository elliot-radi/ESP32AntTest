#pragma once
/* Mobile RF: STA join SoftAP, UDP/ESP-NOW beacons, protocol RX, outage fwd.
 * API prefixed ant_mrf_* (avoids ESP-PHY rf_* symbol clash). */
#include <stdbool.h>
#include <stdint.h>
#include "protocol.h"

void ant_mrf_init(void);
void ant_mrf_start(void);                 /* Mode A STA + beacon (Quick-Check) */

int  ant_mrf_set_mode(ant_mode_t mode);
int  ant_mrf_set_tx_power(int8_t dbm);
int8_t ant_mrf_get_tx_power_dbm(void);

/* Ad-hoc Manual session (no host protocol): start logging/markers locally. */
void ant_mrf_start_adhoc_manual(void);
void ant_mrf_end_session(void);

/* Button short-press in guided/ad-hoc session:
 *   - On GUIDE screen (or first press of a step): mark ready → live view.
 *   - On SESSION live view: advance to next protocol step (or bump run
 *     counter if ad-hoc) and emit PKT_MARKER with the new step_id.
 * Returns true if a step advanced / marker was sent. */
bool ant_mrf_on_short_press(void);

/* Legacy alias used by older UI paths — same as ant_mrf_on_short_press. */
void ant_mrf_advance_step(void);

bool ant_mrf_is_linked(void);
bool ant_mrf_is_recording(void);
bool ant_mrf_is_guided(void);
uint16_t ant_mrf_step_id(void);
int  ant_mrf_step_index(void);   /* 0-based; -1 if n/a */
int  ant_mrf_step_count(void);
