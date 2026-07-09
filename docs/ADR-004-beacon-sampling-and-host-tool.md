# ADR-004: Beacon Sampling Model and Host-Side Tool

**Status:** Accepted  
**Date:** 2026-07-09  
**Deciders:** Project architect  
**Supersedes (in part):** ADR-002 (sampling model), ADR-001 (RSSI acquisition per frame)

---

## Context

ADR-002 chose a request-response (ping/pong) sampling model on top of UDP-over-SoftAP/STA (Mode A) and native ESP-NOW (Mode B). During schema validation via a synthetic-data mockup (`tools/mock_session.py`, `tools/analyze.py`), two properties of request-response proved unacceptable for the antenna-comparison use case:

1. **Structural blindness to TX/RX asymmetry.** A dropped exchange presents identically whether Mobile's TX was too weak to reach Station, or Station's TX was too weak to reach Mobile — both are bare `TIMEOUT`. Antenna asymmetry (a board that receives better than it transmits, or vice versa) is one of the most interesting things to measure (per ADR-001) and is invisible under request-response.

2. **No RSSI inside a null.** When the link degrades, request-response logs `TIMEOUT` with no signal-strength value. The *depth* of a null (not just its *location*) is the discriminator between antennas during orientation sweeps and range walks.

The mockup confirmed that an analysis that only sees null *location* (loss rate by angle/distance) can compare antennas, but one that also sees null *floor* (RSSI inside the null) recovers the asymmetric radiation pattern — the project's central value proposition.

A second, independent realization during design: the project's *value* is the comparison plots, not the raw CSV. A user who flashes working firmware but has no analysis tool has nothing. So the host-side tool is a co-equal deliverable, not a follow-on.

---

## Decision

### Sampling: autonomous beacons with piggybacked RSSI

Both boards transmit a `PKT_BEACON` at a fixed rate (default 5 Hz). Each board independently logs every beacon it decodes from the other. Each beacon **piggybacks `rssi_local`** — the RSSI the sender measured from the peer's most recent decoded beacon — so a single received beacon yields **both** directional RSSI values (receiver's local measurement + the piggybacked peer measurement). The `rssi_local` field already exists in `ant_packet_t`; no schema change beyond adding `PKT_BEACON`.

Consequences:
- **In range (common case):** Station receives Mobile's beacons and logs both `rssi_mob` (piggybacked) and `rssi_sta` (measured locally). Station's log is complete from beacons alone — Mobile need not forward anything.
- **Asymmetric outage (Mobile TX weak, Station TX still reaches Mobile):** Mobile keeps decoding Station's beacons, measures `rssi_mob`, and buffers it in RAM along with any button markers. On reconnect, Mobile forwards the backlog; the host merges by `step_id` (no clock sync — both boards know the active protocol). These `source=MOB` rows carry the **null-floor RSSI** that request-response would have lost.
- **Symmetric outage:** Mobile buffers button markers only (no RSSI to log — received nothing). The event sequence is still recovered.
- **Loss is inferred** from sample count vs. expected (`beacon_hz × step_duration`), not recorded per-row. There is no `TIMEOUT` status; `status` is `OK` or `ERR_DECODE` only.

### Mobile storage: RAM, not LittleFS

Mobile holds outage data in a bounded RAM ring buffer (`ANT_MOB_BUFFER_MAX`) and forwards on reconnect. Mobile has **no LittleFS** (consistent with it being the simple UI device). Power-failure data loss is accepted — the user must walk back to the PC/Station eventually, and in all cases other than power failure Mobile comes back into range and forwards.

### LittleFS demoted to Station durable fallback

Station streams every logged row over serial to the host (canonical store) **and** mirrors it to LittleFS. LittleFS survives host/serial drops and brownouts. The host reads the LittleFS CSV off Station as a recovery source if the serial stream was interrupted.

### Host-side tool: first-class deliverable

A local web server on the user's PC (FastAPI + matplotlib + vanilla JS, no build step, no cloud) is a co-equal deliverable alongside the firmware. It: authors/selects protocols, executes sessions (sends protocol JSON to Station over serial), shows a live view, and produces the analysis plots. The `analyze.py` functions are shared between the CLI mockup and the web backend.

### Default power-up: Quick-Check mode

Both boards auto-connect via WiFi (Mode A) and beacon; Mobile displays live RSSI at 2 Hz. No session, no log. This is the "both boards start in Mode A" baseline from ADR-002, elevated to a visible go/no-go feature for winnowing antennas. Guided sessions are entered via the browser; ad-hoc Manual/Auto remain as a protocol-free fallback.

### Orientation test type

Protocols gain an `orientation` step type (`axis` + `angle_deg`) for single-axis radiation-pattern sweeps, analyzed as a polar RSSI-vs-angle plot (both directions to expose asymmetry). Multi-axis (roll/pitch/yaw) is deferred — single-axis sweeps suffice for v1.

---

## What carries over from ADR-002

The **transport** decisions in ADR-002 stand: UDP over SoftAP/STA for Mode A, native ESP-NOW for Mode B, both starting in Mode A, Station as SoftAP. Only the **sampling model on top** changes (request-response → beacon).

---

## Alternatives Considered

### Keep request-response, accept null-location-only
Simpler firmware (no Mobile buffer/sync), Mobile filesystem-free. Rejected: loses the asymmetric-RSSI / null-floor measurement that is the project's central value.

### Hybrid: beacons + ping/pong on button press
Intended to guarantee a paired sample at the marker. Rejected as redundant: pure beacon at 5 Hz already yields ~5 samples in the 1 s window around a press, sufficient for a per-step mean. The ping/pong state machine would reintroduce request-response's blindness with no extra signal.

### Beacon without piggyback ("just shouting Hi")
Each board logs only its own measurement; Mobile would have to stream `rssi_mob` separately to fill Station's missing column. Rejected: a constant forwarding stream just to complete the log. The piggyback makes each board's log complete from beacons alone.

### Serve the protocol configurator from Station's SoftAP (`esp_http_server`)
Rejected: puts a web server + JSON parser + static-asset serving in firmware — complexity in the most expensive place. The host PC is already required for viewing results; serving the UI from it consolidates onto the already-present device and keeps firmware honest about being an RF instrument, not a web server.

### Noise-floor / SNR capture
Considered (Army RF background). Rejected for v1: RSSI + loss rate cleanly discriminates antennas at matched TX power; the ESP32 has no clean noise-floor API. Deferred/out-of-scope.

---

## Consequences

- `ant_packet_t` gains `PKT_BEACON`, `PKT_MARKER`, `PKT_PROTOCOL`; `PKT_PING`/`PKT_PONG` are dropped.
- Mobile firmware needs a RAM ring buffer + a backlog-forward protocol over the beacon link.
- Host merge logic keys on `step_id` (not clock) — both boards know the active protocol.
- The log schema gains a `source` (STA/MOB) provenance column; `status` loses `TIMEOUT`.
- ADR-001's per-mode RSSI API discussion shifts toward per-frame promiscuous reads (needed for true per-beacon RSSI); the averaged native APIs remain an implementation alternative. See ADR-001 addendum.
- The project deliverable scope grows to include `tools/` (host webserver + analysis); SPEC §1 and §3.7 reflect this.
