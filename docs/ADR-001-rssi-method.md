# ADR-001: RSSI Measurement Method

**Status:** Accepted  
**Date:** 2026-07-03  
**Deciders:** Project architect

---

## Context

The core measurement this tool provides is RSSI — specifically, the signal strength of packets received *at each board from the other board*. This is directional and asymmetric: RSSI_MOB and RSSI_STA are independent values that can differ substantially depending on each board's antenna, orientation, and receiver sensitivity.

The challenge is that the correct API call to obtain this value differs between WiFi SoftAP/STA mode and ESP-NOW mode, and between the AP-role board and the STA-role board.

---

## Decision

### Mode A (WiFi Peer)

Use native ESP-IDF WiFi API calls — no promiscuous mode required.

- **Mobile (STA role):** `esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info)` returns `ap_info.rssi` — the RSSI of the SoftAP beacon/data frames received at Mobile. This is RSSI_MOB.
- **Station (SoftAP role):** `esp_wifi_ap_get_sta_list(wifi_sta_list_t *sta_list)` returns a list of connected STAs, each with `wifi_sta_info_t.rssi` — the RSSI of the STA's frames received at Station. This is RSSI_STA.

Both are signed `int8_t` values in dBm. Station includes RSSI_STA in the `PKT_PONG` payload. Mobile reads RSSI_MOB locally and displays both.

### Mode B (ESP-NOW)

Use promiscuous mode callback on both boards, filtered by peer MAC address.

```c
esp_wifi_set_promiscuous(true);
esp_wifi_set_promiscuous_rx_cb(&promiscuous_rx_cb);

void promiscuous_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    const wifi_promiscuous_pkt_t *ppkt = (wifi_promiscuous_pkt_t *)buf;
    // Filter by source MAC matching peer
    // ppkt->rx_ctrl.rssi is the RSSI of this received frame
}
```

Each board stores the most recently received RSSI from the peer and includes it in its next outbound packet's `rssi_local` field. The receiver then has both values after one round trip.

---

## Alternatives Considered

### Use promiscuous mode for both modes
Would work, but adds unnecessary complexity in WiFi mode when native API calls suffice. The native `wifi_sta_info_t.rssi` is also an averaged value, which is arguably more stable than single-packet promiscuous reads.

### Have Mobile report Station's RSSI only (single-ended)
Simpler, but loses half the measurement. Antenna asymmetry is one of the most interesting things to measure — a board might receive better than it transmits, so both directions matter.

### Use packet delivery callbacks in ESP-NOW (`esp_now_register_recv_cb`)
The standard ESP-NOW receive callback does not expose RSSI. The `esp_now_recv_info_t` struct added in ESP-IDF v5.0+ does include a `rx_ctrl` pointer, but promiscuous mode is more portable across IDF versions and chip variants.

---

## Consequences

- Both boards must enable promiscuous mode during ESP-NOW operation. This means the radio is receiving all 802.11 frames on the channel, which may slightly increase power consumption.
- In WiFi mode, `esp_wifi_ap_get_sta_list()` must be called at Station after each received ping, before composing the pong. This is a lightweight call.
- The `rssi_local` field in `ant_packet_t` carries a one-exchange-old RSSI value (what the sender measured last time it received from the peer). For the purposes of this tool — aggregate metrics, not sub-millisecond precision — this lag is acceptable.
- RSSI values are known to fluctuate ±5–10 dBm even at a fixed position. Burst averaging (N=10 pings per burst) is the primary mitigation.

---

## Addendum — 2026-07-09 (ADR-004)

The sampling model changed from request-response to **autonomous beacons** (see ADR-004). The RSSI acquisition discussion above is revised accordingly:

- **Both modes** now acquire RSSI on **per-beacon reception**, not via averaged native APIs polled per exchange. The promiscuous-mode callback (`wifi_promiscuous_pkt_t.rx_ctrl.rssi`), filtered by peer MAC, is the preferred path on both boards because it yields a true per-beacon RSSI and is portable across both WiFi and ESP-NOW.
- The native averaged APIs (`esp_wifi_sta_get_ap_info` / `esp_wifi_ap_get_sta_list`) remain a valid *implementation alternative* for a smoothed per-step estimate if per-frame promiscuous setup proves costly on a given target, but they do not give true per-beacon RSSI.
- The exact API choice is a firmware-implementation detail; this ADR commits to the **measurement semantics** (RSSI of the most recently decoded beacon, piggybacked in the next outbound beacon via `rssi_local`), not the specific call.
- The directional split (RSSI_MOB vs RSSI_STA, each measured by the board that received the beacon) is unchanged — beacon mode preserves it via the piggyback.
