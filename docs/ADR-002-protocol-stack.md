# ADR-002: Protocol Stack and Transport

**Status:** Accepted  
**Date:** 2026-07-03  
**Deciders:** Project architect

---

## Context

The project requires board-to-board communication in two distinct RF modes. The choice of transport within each mode affects complexity, the ability to capture RSSI, and how we handle connection state.

---

## Decision

### Mode A (WiFi): Raw UDP over SoftAP/STA

Station runs as SoftAP. Mobile connects as STA. Communication uses UDP datagrams on port 5432 carrying `ant_packet_t` structs.

Rationale:
- UDP requires no connection handshake after WiFi association, reducing per-ping overhead
- Packet loss is already a meaningful signal (logged as TIMEOUT), so TCP's retransmit would hide real link failures
- SoftAP/STA is the only router-free WiFi topology available on ESP32 — one board must be AP
- Station is the SoftAP because the AP role provides RSSI of connected STAs via `esp_wifi_ap_get_sta_list()` (see ADR-001)

### Mode B (ESP-NOW): Native ESP-NOW with promiscuous RSSI

Standard ESP-NOW peer registration and `esp_now_send()` / receive callback. Promiscuous callback runs in parallel for RSSI capture (ADR-001).

Rationale:
- ESP-NOW is connectionless and lower-overhead than WiFi association
- Operates on the same channel as the WiFi driver — no channel conflict
- No AP role needed; truly symmetric peer relationship
- Expected longer range than WiFi at same TX power, providing a useful upper bound for antenna testing

### Mode Switching

Mode switching is initiated from Mobile's menu. The sequence is:

1. Mobile sends a `PKT_MODE_SWITCH` command in the current mode's transport
2. Both boards reinitialize their radio stack for the new mode
3. Mobile waits up to 5 s for Station's `PKT_MODE_ACK` in the new mode
4. If ACK received, session continues; if timeout, Mobile displays "MODE SWITCH FAILED" and reverts

Both boards always start in Mode A (WiFi) on power-up, providing a known baseline transport for initial setup and mode negotiation.

---

## Alternatives Considered

### TCP over SoftAP/STA
Rejected — TCP retransmits mask real link quality. A timeout *is* data in antenna testing.

### Both modes use ESP-NOW only
Rejected — the project goal is explicitly to compare WiFi and ESP-NOW performance for the same antenna configuration. Both modes are required.

### Use existing router for WiFi mode
Rejected — introduces an uncontrolled third party (the router's antenna, location, and congestion) into the measurement. The point is board-to-board characterization.

### MQTT or HTTP over SoftAP
Rejected — far too heavyweight for 20-byte control packets. Unnecessary dependencies.

---

## Consequences

- The firmware must handle two completely different radio initialization paths. These are isolated in `mobile/wifi_mode.c`, `mobile/espnow_mode.c`, and equivalents in `station/`.
- Mode A requires Mobile to know Station's IP address. Since Station is always SoftAP with a fixed IP (192.168.4.1 by default in ESP-IDF), this is a constant, not a discovery problem.
- Mode B requires both boards to know each other's MAC address. MAC addresses are exchanged during Mode A initialization and stored in NVS for the session.
- The `shared/protocol.h` packet definition must be valid for both transports (it is, since it's just a byte struct).

---

## Addendum — 2026-07-09 (ADR-004)

The **transport** decisions in this ADR stand: UDP over SoftAP/STA for Mode A, native ESP-NOW for Mode B, both starting in Mode A, Station as SoftAP. Only the **sampling model on top** changes: request-response (ping/pong) is **superseded by autonomous beacons** (`PKT_BEACON`) with piggybacked `rssi_local`. See ADR-004 for the full rationale (asymmetric-RSSI / null-floor capture, Mobile RAM buffering, loss inferred from count). `PKT_PING`/`PKT_PONG` are dropped from the packet enum.
