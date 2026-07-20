#pragma once
/* ESP32AntTest — Station RF layer: SoftAP + UDP beacons + ESP-NOW +
 * promiscuous per-beacon RSSI.
 *
 * See docs/SPEC.md §3.1/§5, ADR-001 addendum, ADR-002, ADR-004.
 *
 * Station is always beaconing after ant_rf_start() (Quick-Check). start_session
 * overlays logging; end_session returns to Quick-Check without stopping RF.
 */
#include <stdbool.h>
#include <stdint.h>
#include "protocol.h"

/* Init WiFi stack (nvs/netif/event loop) but do not start SoftAP yet.
 * Call once from app_main before ant_rf_start(). */
void ant_rf_init(void);

/* Bring up SoftAP (Mode A default), UDP socket, promiscuous RX, beacon
 * task. Idempotent. Always starts in ANT_MODE_WIFI (SPEC / ADR-002). */
void ant_rf_start(void);

/* Switch transport. If already running, tears down the current mode and
 * brings up the new one. Returns 0 on success. */
int ant_rf_set_mode(ant_mode_t mode);

/* Apply our TX power (dBm). Converts via ANT_DBM_TO_IDF and reads back the
 * clamped value. Logged tx_sta should use ant_rf_get_tx_power_dbm(). */
int ant_rf_set_tx_power(int8_t dbm);

/* Actual TX power after clamp (dBm). */
int8_t ant_rf_get_tx_power_dbm(void);

/* Notify RF that a logged session began/ended. RF already beacons in
 * Quick-Check; this just switches which session_id/step_id go into outbound
 * beacons and whether decoded peer beacons are written to the logger. */
void ant_rf_on_session_begin(void);
void ant_rf_on_session_end(void);

/* Stash the protocol JSON text for PKT_PROTOCOL forward to Mobile.
 * Copied internally (max ANT_PROTOCOL_JSON_MAX). Empty string clears. */
void ant_rf_set_protocol_json(const char *json);

/* Kick a protocol forward attempt (called from start_session). Best-effort:
 * if no peer is known yet, the next SoftAP STA-join will retry. */
void ant_rf_forward_protocol(void);

/* True if a Mobile peer MAC has been seen (associated STA or chanted beacon). */
bool ant_rf_peer_known(void);
