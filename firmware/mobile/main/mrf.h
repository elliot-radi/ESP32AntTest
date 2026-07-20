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

/* Button short-press in session: bump step_id + emit PKT_MARKER. */
void ant_mrf_advance_step(void);

bool ant_mrf_is_linked(void);
bool ant_mrf_is_recording(void);
uint16_t ant_mrf_step_id(void);
