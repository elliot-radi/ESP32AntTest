# ESP32AntTest — System Specification

**Version:** 0.3.0-draft  
**Status:** Pre-implementation (design validated via synthetic-data mockup)  
**Last Updated:** 2026-07-09

---

## 1. Purpose and Scope

ESP32AntTest is a portable, self-contained RF link characterization instrument. It measures board-to-board received signal strength (RSSI) between two ESP32 devices in two RF modes — WiFi SoftAP/STA and ESP-NOW — without any external router or access point.

The system has two co-equal deliverables:

1. **Firmware** — the sensor head: two ESP32 boards (Mobile + Station) that beacon to each other, measure RSSI, and log samples.
2. **Host-side tool** — the instrument UI: a local web server (run on the user's PC) for authoring test protocols, executing sessions, viewing live data, and producing the analysis plots that are the actual value of the instrument.

The primary use cases are:

- Comparing antenna types (e.g. ceramic PCB vs. FPC vs. quarter-wave monopole)
- Evaluating antenna **orientations** at fixed distance (radiation-pattern / asymmetry measurement)
- Mapping link quality vs. physical distance (range walk)
- Observing signal variation over time under real-world interference (time soak)
- Comparing transmit power settings
- Quick go/no-go comparison of antennas/modules without a formal logged session

### Out of Scope

- Throughput or latency measurement
- Multi-board topologies (>2 boards)
- 5 GHz band
- Integration with any external network or cloud service
- **Noise-floor / SNR measurement** (RSSI + packet-loss rate is sufficient for antenna comparison at matched TX power; the ESP32 has no clean noise-floor API — see ADR-001 addendum)
- Battery circuitry / RTC hardware on Mobile (USB-powered; wall clock via host serial injection — see [Time Source](#time-source))
- Sleep modes between samples (future enhancement)

---

## 2. Hardware

### 2.1 Roles

The Mobile and Station roles are decoupled from the physical board — either the ESP32-C3 or the ESP32-WROOM-32 can fill either role. The assignment is a compile-time choice:

| Config | Mobile | Station |
|--------|--------|---------|
| **A** (current default) | ESP32-WROOM-32 | ESP32-C3 |
| **B** | ESP32-C3 | ESP32-WROOM-32 |

Role assignment is determined at compile time via a `#define ROLE_MOBILE` / `#define ROLE_STATION` flag in `shared/config.h`, making the firmware portable across board variants. Board-specific GPIO pin assignments are isolated in `mobile/board_config.h` and `station/board_config.h` (see [HARDWARE.md](HARDWARE.md) for both boards' pin maps).

### 2.2 Mobile Peripherals

| Peripheral | Type | Interface |
|-----------|------|-----------|
| Display | 0.96" SSD1306 OLED, 128×64 px | I2C (addr 0x3C) |
| Input | Single tactile push-button | GPIO, active-low, internal pull-up |
| Power | USB (power bank or USB charger/cable) | — |
| Storage | None (no LittleFS on Mobile) | — |

Default Mobile pins by board (configurable in `board_config.h`):

| Signal | ESP32-C3 | ESP32-WROOM-32 |
|--------|----------|----------------|
| OLED SDA | GPIO 8 | GPIO 21 |
| OLED SCL | GPIO 9 | GPIO 22 |
| Button | GPIO 5 | GPIO 17 |

(GPIO 0 is avoided as the button pin on the WROOM-32 because it is a strapping pin.)

The Mobile holds **outage data in RAM** (a bounded ring buffer of beacon-RSSI samples and button markers captured while out of range of Station) and forwards it to Station on reconnect. RAM, not flash — power-failure data loss is accepted (see ADR-004).

### 2.3 Station Peripherals

| Peripheral | Purpose |
|-----------|---------|
| USB-Serial (built-in) | Primary live data stream to the host PC (the host webserver consumes this) |
| LittleFS (onboard flash) | **Durable fallback** — Station mirrors every logged row to flash as well as streaming it over serial, so an in-flight session survives a host/serial drop or brownout |

The host PC is the canonical log store during a session; LittleFS is the resilient backup. Station streams every row over serial *and* writes it to LittleFS.

### 2.4 RF Notes

- Both boards operate on 2.4 GHz only
- WiFi channel: fixed to channel 1 by default (configurable in `shared/config.h`)
- ESP-NOW operates on the same channel as the WiFi driver
- TX power is configurable per session via Mobile menu / protocol (ESP-IDF `esp_wifi_set_max_tx_power()`, dBm units — see [§6](#6-configuration))

---

## 3. Functional Requirements

### 3.1 RF Modes and Sampling Model

Both boards operate as **autonomous beacons**: each transmits a small beacon packet at a fixed rate (default 5 Hz) and independently logs every beacon it can decode from the other. There is **no request-response / ping-pong** sampling — a missed beacon is simply not logged, and packet loss is inferred from sample count vs. expected (see ADR-004 for the rationale: beacon capture recovers asymmetric-RSSI / null-floor data that request-response loses as bare timeouts).

Each beacon **piggybacks `rssi_local`** — the RSSI the sender measured from the other board's most recent decoded beacon. So a single received beacon yields **both** directional RSSI values:

- The receiver measures the incoming beacon's RSSI locally (its own direction).
- The receiver reads the piggybacked `rssi_local` (the other direction).

This makes each board's log **complete from beacons alone** in the common (both-in-range) case — no separate forwarding stream is needed. In an asymmetric outage (one board can't hear the other, but the reverse leg still works), the listening board buffers its captured RSSI + any button markers in RAM and forwards on reconnect; the host merges by `step_id` (no clock sync required — both boards know the active protocol).

#### Mode A: WiFi Peer (SoftAP/STA)

- Station runs as SoftAP (SSID: `AntTest-<MAC4>`, open auth); Mobile connects as STA.
- Beacons are UDP datagrams on `ANT_UDP_PORT` carrying `ant_packet_t`.
- RSSI acquisition: per-beacon, via promiscuous-mode callback filtered by peer MAC (`wifi_promiscuous_pkt_t.rx_ctrl.rssi`), on both boards. (The native averaged APIs `esp_wifi_sta_get_ap_info` / `esp_wifi_ap_get_sta_list` are an implementation alternative for a smoothed estimate; the per-frame promiscuous read is preferred for true per-beacon RSSI. Exact API choice is a firmware-implementation detail; see ADR-001 addendum.)

#### Mode B: ESP-NOW Peer

- Both boards register as ESP-NOW peers (broadcast MAC initially, then unicast by MAC exchange).
- Beacons are native ESP-NOW packets carrying `ant_packet_t`.
- RSSI acquisition: promiscuous-mode callback, filtered by peer MAC, on both boards (as Mode A).

### 3.2 Session and Protocol Model

A **protocol** (a.k.a. test definition) is an ordered list of **steps** authored on the host webserver and transferred to the boards at session start. Each step carries its condition parameters and a prompt string:

| Step type | Params | Meaning |
|-----------|--------|---------|
| `distance` | `distance_m` | Range-walk point |
| `orientation` | `axis` (X/Y/Z), `angle_deg` | Single-axis rotation point (radiation-pattern sweep) |
| `soak` | `duration_s` | Time-soak interval |
| `free` | — | Ad-hoc marker (no condition) |

A **session** is an instance of a protocol executed between the two boards. Each session has:

- A session ID (timestamp-based, e.g. `20260703_143022`)
- The protocol being run (or null for ad-hoc Manual/Auto)
- RF mode (WiFi / ESP-NOW)
- TX power setting for each board

A **run** maps 1:1 to a protocol step (in guided mode). In the log, the join key is `step_id` (the protocol step's id). In ad-hoc Manual/Auto (no protocol), `step_id` is simply an incrementing run counter.

### 3.3 Test Modes

The firmware has two top-level operating modes:

#### Quick-Check (default power-up state)

- Both boards auto-connect via WiFi (Mode A) and begin beaconing.
- Mobile displays live RSSI at the OLED refresh rate (default 2 Hz from the latest beacon).
- **No session, no log, no protocol.** Pure go/no-go for winnowing antennas/modules.
- This is the "both boards start in Mode A on power-up" baseline (ADR-002) elevated to a visible feature.

#### Guided Session (formal logged test)

- Host webserver authors/loads a protocol → transferred to Station → forwarded to Mobile (once per session, out-of-band from beacons).
- Mobile walks the operator through each step on the OLED (the **test-guide**): e.g. *"Rotate Z +45°, press when ready: Z-135°"* or *"Increase distance 5m, press when ready: 15m"*.
- Button press advances to the next step; the press is logged as a **marker** (buffered on Mobile if out of range, forwarded on reconnect).
- Station logs every decoded beacon, tagged with the active `step_id`.
- Session can be ended from the host browser (always) or from Mobile (long-press) if in range.

#### Ad-hoc Manual / Auto (protocol-free fallback)

- **Manual (range walk):** button press starts/stops a run; samples accumulate while active.
- **Auto (time soak):** auto-sample at `ANT_AUTO_INTERVAL_MS`; one start/stop = one run.
- Used when no host/protocol is available; `step_id` is an incrementing run counter. No test-guide prompts.

### 3.4 Mobile UI

Single button with three gesture types:

| Gesture | Definition | Action |
|---------|-----------|--------|
| Short press | < 500 ms | Scroll / Next (in menus) or **advance to next protocol step** (in guided mode) |
| Long press | ≥ 1500 ms | Select / Confirm |
| Double press | Two presses < 400 ms apart | Back / Cancel |

#### Menu Tree (Quick-Check ↔ Guided ↔ Ad-hoc)

```
[QUICK-CHECK]  ──long press──►  [MENU]
  live RSSI, no log                ├── Session ──── [New (guided)] | [New (ad-hoc Manual)] | [New (ad-hoc Auto)]
                                   ├── Mode ─────── [WiFi] | [ESP-NOW]
                                   ├── TX Power ─── [2|10|17|20 dBm]
                                   └── End Session
```

In Guided mode the OLED shows the **test-guide** for the current step (prompt + condition); a short press advances. In Quick-Check the OLED shows live RSSI.

### 3.5 Mobile Display Layout (128×64)

Quick-Check / active-session live view:

```
┌────────────────────────┐
│ Mode: ESP-NOW  Pwr:17  │  row 0 — status bar
│ Mobile RSSI: -67 dBm   │  row 1
│ Station RSSI: -71 dBm  │  row 2
│ Samples:  42 Step: 5  │  row 3
│ [RECORDING ●]          │  row 4 — state indicator
└────────────────────────┘
```

In Guided mode, rows 1–4 show the current step's prompt + condition until the button is pressed, then revert to the live view. Status bar remains visible at all times.

### 3.6 Station Output and the Host Webserver

The host PC is the primary interface for setup, execution, and results.

#### Host webserver (primary)

- Served from the user's PC (FastAPI + matplotlib + vanilla JS, no build step, no cloud).
- **Author/select** a protocol; **execute** a session (sends protocol JSON to Station over serial, starts the log).
- **Live view** of the in-progress session (rows stream over serial/websocket).
- **Results**: per-step summary table + plots (RSSI vs distance, RSSI vs angle (polar, both directions), loss vs distance/angle, RSSI vs time), and config-comparison overlays.
- Pulls the LittleFS CSV off Station as a backup/recovery source if the serial stream was interrupted.

#### Serial (live, machine-readable stream to host)

Station streams every logged row as it's captured on the `>` log channel of the [Host ↔ Station Serial Protocol](SERIAL_PROTOCOL.md). The host webserver ingests this and writes the canonical CSV. Format is the same CSV schema as the LittleFS file (below), one row per line prefixed with `>`. Setup commands, session control, and events ride a separate `$`-prefixed control channel (JSON, one object per line); the two channels are multiplexed on the one serial line by prefix.

#### LittleFS log (CSV, durable fallback — one file per session)

```
session_id,step_id,seq,timestamp_ms,datetime,mode,tx_mob,tx_sta,rssi_mob,rssi_sta,source,status
20260703_143022,5,0042,847293,2026-07-03T14:30:22,ESPNOW,17,17,-67,-71,STA,OK
20260703_143022,5,0043,849293,2026-07-03T14:30:24,ESPNOW,17,17,-89,,MOB,OK
```

- `timestamp_ms` — ms since Station boot (monotonic, for inter-sample math).
- `datetime` — local wall-clock `YYYY-MM-DDTHH:MM:SS` of the sample, from the Station's time baseline (see [Time Source](#time-source)). Empty if no time set at boot.
- `source` — `STA` if Station received Mobile's beacon (complete row, both RSSI present via piggyback); `MOB` if Mobile received Station's beacon but Station did **not** receive Mobile's (uplink outage / null) — Mobile buffered `rssi_mob`, `rssi_sta` is empty. This is the **null-floor data** beacon mode recovers; under request-response it would be a bare timeout with no RSSI.
- `status` — `OK` for a cleanly decoded beacon; `ERR_DECODE` for a corrupt-but-attributed frame. There is **no `TIMEOUT`** value under beacon mode — loss is *inferred* from sample count vs. expected (`beacon_hz × step_duration`), not recorded per-row.

### 3.7 Host-side Tool

The `tools/` directory contains the host tooling. A synthetic-data **mockup** (`mock_session.py` + `analyze.py`) was used to validate the log schema and analysis before any firmware exists (see [tools/README.md](../tools/README.md) and ADR-004). The same analysis functions will be wrapped as HTTP endpoints by the production webserver (`tools/server.py`, to be built). Stack: FastAPI + matplotlib + vanilla JS, dependencies in `tools/requirements.txt`, run from a project venv.

---

## 4. Data Record Schema

| Field | Type | Description |
|-------|------|-------------|
| `session_id` | string | `YYYYMMDD_HHMMSS` of session start |
| `step_id` | uint16 | Protocol step index (guided) or incrementing run counter (ad-hoc). Join key to the protocol JSON. |
| `seq` | uint32 | Beacon sequence number |
| `timestamp_ms` | uint64 | ms since Station boot (monotonic) |
| `datetime` | string | Local wall-clock `YYYY-MM-DDTHH:MM:SS` of the sample (empty if no time set at boot) |
| `mode` | enum | `WIFI` or `ESPNOW` |
| `tx_mob` | int8 | Mobile TX power (dBm) |
| `tx_sta` | int8 | Station TX power (dBm) |
| `rssi_mob` | int8 | RSSI at Mobile of Station's beacon (dBm). Empty if Mobile did not decode (`source=STA` row) — present when Mobile measured it. |
| `rssi_sta` | int8 | RSSI at Station of Mobile's beacon (dBm). Empty if Station did not decode (`source=MOB` row). |
| `source` | enum | `STA` (Station decoded; both RSSI via piggyback) or `MOB` (Mobile decoded during uplink outage; `rssi_sta` empty) |
| `status` | enum | `OK`, `ERR_DECODE` (no `TIMEOUT` — loss is computed) |

---

## 5. Communication Protocol

### 5.1 Packet Structure (shared)

```c
typedef struct {
    uint8_t  magic[2];      // 0xAE, 0x32
    uint8_t  version;       // protocol version, currently 0x01
    uint8_t  type;          // ant_pkt_type_t
    uint32_t seq;           // beacon sequence number
    uint32_t session_id;    // lower 32 bits of session timestamp
    uint16_t step_id;       // active protocol step (run)
    int8_t   rssi_local;    // RSSI measured by sender (of peer's last beacon) — piggyback
    int8_t   tx_power;      // sender's current TX power (dBm)
    uint8_t  reserved[4];
} ant_packet_t;             // 20 bytes total
```

### 5.2 Packet Types

```c
typedef enum {
    PKT_BEACON    = 0x01,  // autonomous broadcast; carries piggybacked rssi_local
    PKT_MARKER    = 0x04,  // button-press marker (forwarded from Mobile during/after outage)
    PKT_PROTOCOL  = 0x10,  // session-setup: chunked protocol transfer (once per session, out-of-band)
    PKT_MODE_ACK  = 0x11,
} ant_pkt_type_t;
```

`PKT_BEACON` is the workhorse: both boards transmit it at `ANT_BEACON_HZ`. `rssi_local` piggybacks the sender's freshest measurement of the peer. `PKT_MARKER` carries button-press events (step advances) so they survive outages. `PKT_PROTOCOL` transfers the protocol JSON to Mobile at session start (chunked; out-of-band, not per-beacon).

> `PKT_PING`/`PKT_PONG` from earlier drafts are **dropped** — beacon mode has no request-response. Kept out of the enum to avoid dead code.

### 5.3 Loss, Retry, and Link-Loss

- **No retries.** A beacon that isn't decoded is simply not logged; loss is inferred at analysis time from sample count vs. expected.
- **Link loss** is declared after `ANT_LOSS_THRESHOLD` consecutive seconds with no decoded beacon in either direction; the current step/run is closed.

---

## 6. Configuration

`shared/config.h` contains all tuneable defaults:

```c
#define ANT_WIFI_CHANNEL       1
#define ANT_SOFTAP_SSID_PREFIX "AntTest"
#define ANT_SOFTAP_PASS         ""        // open network
#define ANT_STATION_IP          "192.168.26.1"
#define ANT_UDP_PORT            5432

// Sampling (beacon mode)
#define ANT_BEACON_HZ          5          // both boards beacon at this rate
#define ANT_DISPLAY_HZ         2          // OLED live-RSSI refresh (quick-check/active)
#define ANT_AUTO_INTERVAL_MS   5000       // ad-hoc Auto sample period
#define ANT_LOSS_THRESHOLD     5          // consecutive seconds no decode = link lost

// TX power options in dBm (user-facing / packet / log units).
// esp_wifi_set_max_tx_power() takes 0.25 dBm units — use ANT_DBM_TO_IDF().
#define ANT_TX_POWER_LOW        2       // dBm
#define ANT_TX_POWER_MED        10      // dBm
#define ANT_TX_POWER_HIGH       17      // dBm
#define ANT_TX_POWER_MAX        20      // dBm
#define ANT_DEFAULT_TX_POWER    17      // dBm
#define ANT_DBM_TO_IDF(dbm)    ((int8_t)((dbm) * 4))

// Button gestures (ms)
#define ANT_BTN_SHORT_MAX_MS    500
#define ANT_BTN_LONG_MIN_MS     1500
#define ANT_BTN_DOUBLE_GAP_MS   400
#define ANT_BTN_DEBOUNCE_MS     20

// OLED + button — Mobile-side pins. Override in board_config.h.
#define ANT_OLED_I2C_ADDR       0x3C    // SSD1306; some modules use 0x3D
#ifndef ANT_OLED_SDA_PIN
#define ANT_OLED_SDA_PIN        21      // WROOM default (C3: 8)
#endif
#ifndef ANT_OLED_SCL_PIN
#define ANT_OLED_SCL_PIN        22      // WROOM default (C3: 9)
#endif
#ifndef ANT_BUTTON_PIN
#define ANT_BUTTON_PIN          17      // WROOM default (C3: 5)
#endif

// Mobile outage buffer (RAM ring buffer, no LittleFS on Mobile)
#define ANT_MOB_BUFFER_MAX      4096    // ~ worst-case long-walk backlog (markers + RSSI)

// Station LittleFS (durable fallback)
#define ANT_LOG_MOUNT_POINT     "/logs"
#define ANT_LOG_MAX_FILES       5
```

> **Note on TX power clamping:** `esp_wifi_set_max_tx_power()` clamps to the chip's supported range. The ESP32-C3 minimum is ~3 dBm, so the `2 dBm` option may be clamped up to ~3 dBm on the C3; the WROOM-32 range is wider. Call `esp_wifi_get_max_tx_power()` after setting and log the *actual* achieved power so the `tx_*` columns reflect reality, not just the requested value.

### Time Source

The Station is a SoftAP with no uplink, so SNTP is unavailable, and the ESP32 has no battery-backed RTC. Wall-clock time is injected from the host PC over serial at boot, as part of the [Host ↔ Station Serial Protocol](SERIAL_PROTOCOL.md):

1. On boot, Station emits a banner (`# ...`) and a `$ {"evt":"time_prompt"}` control line on the serial console.
2. Host sends `$ {"cmd":"settime","iso":"YYYY-MM-DDTHH:MM:SS"}` (or `"epoch":<unix>`).
3. Station stores the offset from `esp_timer` boot time as the wall-clock baseline and replies `$ {"evt":"time_ok",...}`.
4. If no time is received within ~5 s, Station names the session file `UNKNOWN_<bootcounter>.csv` and leaves the `datetime` column empty; per-sample `timestamp_ms` remains boot-relative and self-consistent.

The Mobile does not require wall time. A real-time clock (e.g. DS3231 on I2C) is **out of scope** — serial injection is sufficient for USB-tethered Station operation.

---

## 7. Build

### Firmware Requirements

- ESP-IDF v5.2 or later
- `idf.py` in PATH
- Targets: `esp32` and `esp32c3` (role-to-board mapping is configurable; see §2.1)

### Build Mobile

```bash
cd firmware/mobile
idf.py set-target esp32        # Config A (WROOM); use esp32c3 for Config B
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### Build Station

```bash
cd firmware/station
idf.py set-target esp32c3      # Config A (C3); use esp32 for Config B
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

> **Serial device nodes:** the ESP32-WROOM-32 dev module exposes a USB-UART
> bridge (CP2102/CH340) and appears as `/dev/ttyUSB*`; the ESP32-C3-Zero /
> SuperMini exposes its **native USB JTAG/serial** unit and appears as
> `/dev/ttyACM*` (e.g. `/dev/ttyACM0`). Adjust the `-p` port to match the
> board actually plugged in. (`idf.py monitor` needs a TTY; in a
> non-interactive shell, read the UART directly with pyserial.)

Station uses a custom `partitions.csv` (4 MB flash, no OTA) with a 1 MB LittleFS `logs` partition (`firmware/station/partitions.csv`). Mobile uses a minimal partition table (nvs + phy + factory only — no `logs` partition, since Mobile has no LittleFS).

### Host Tool

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install -r tools/requirements.txt
python tools/mock_session.py --protocol protocols/range_walk.json \
    --profile profiles/good_antenna.json --out logs/
python tools/analyze.py --sessions "logs/*range_walk*.csv" \
    --protocol protocols/range_walk.json --out plots/
```

The production webserver (`tools/server.py`) will reuse the `analyze.py` logic and add a FastAPI endpoint layer + serial bridge to Station (to be built).

---

## 8. Non-Functional Requirements

| Property | Requirement |
|----------|-------------|
| Beacon rate | 5 Hz (both boards); OLED live refresh 2 Hz |
| LittleFS capacity (Station) | ≥ 10,000 records per session (1 MB `logs` partition; ~50 bytes/row → ~20k rows headroom) — durable fallback, not primary |
| Mobile outage buffer | Bounded RAM ring buffer (`ANT_MOB_BUFFER_MAX`); sized for worst-case long-walk backlog; power-loss data loss accepted |
| Button debounce | Software 20 ms debounce |
| Portability | Firmware compiles for esp32, esp32c3, esp32s3 targets with only `board_config.h` changes |
| Host tool | No build step; `pip install` + run; no cloud dependencies |

---

## 9. Resolved Items

### 9.1 Open Items (resolved 2026-07-08)

| ID | Item | Resolution | Date |
|----|------|-----------|------|
| OI-01 | SSD1306 I2C address (0x3C vs 0x3D) | Confirmed **0x3C**; default set in `config.h` (`ANT_OLED_I2C_ADDR`). | 2026-07-08 |
| OI-02 | Station clock: wall-time (SNTP) vs boot-relative ms | Per-sample `timestamp_ms` stays boot-relative; wall-clock `datetime` column via serial `SETTIME` injection (no RTC). See [Time Source](#time-source). | 2026-07-08 |
| OI-03 | LittleFS partition size | **1 MB** `logs` partition (subtype `0x82`), 4 MB flash, no OTA. `firmware/station/partitions.csv`. | 2026-07-08 |
| OI-04 | Mobile battery circuit | **No battery circuit.** Mobile USB-powered. | 2026-07-08 |

### 9.2 Design Decisions (resolved 2026-07-09, validated via synthetic-data mockup — see ADR-004)

| ID | Item | Resolution |
|----|------|-----------|
| DI-01 | Sampling model: request-response vs beacon | **Beacon mode** (both boards beacon at 5 Hz, piggyback `rssi_local`). Request-response dropped — it loses null-floor RSSI as bare timeouts and is blind to TX/RX asymmetry. |
| DI-02 | Null capture: location vs floor | **Null floor** — capture downlink RSSI inside the null via Mobile-buffered `source=MOB` samples. Required for antenna-asymmetry measurement. |
| DI-03 | Mobile storage during outage | **RAM ring buffer** (no LittleFS on Mobile); forward on reconnect; host merges by `step_id`. Power-loss data loss accepted. |
| DI-04 | LittleFS role | **Demoted to Station durable fallback.** Host is canonical log store; Station streams over serial *and* mirrors to flash. |
| DI-05 | Noise floor / SNR | **Out of scope.** RSSI + loss rate suffices for antenna comparison; no clean ESP32 noise-floor API. |
| DI-06 | Retries | **None.** Loss inferred from count vs expected; retries would bias range data. |
| DI-07 | Host-side tool | **First-class deliverable**, not a side-quest: local FastAPI + matplotlib + vanilla JS webserver for protocol authoring, session execution, live view, results/plots. No cloud. |
| DI-08 | Default power-up behavior | **Quick-Check mode**: auto-connect WiFi, beacon, display live RSSI, no log. Guided session via browser; ad-hoc Manual/Auto as protocol-free fallback. |
| DI-09 | Orientation test type | Added: single-axis sweeps (`axis` + `angle_deg`); polar RSSI-vs-angle plot exposes radiation-pattern asymmetry. Multi-axis (roll/pitch/yaw) deferred. |
| DI-10 | Protocol entity | Test definition (JSON) authored on host, transferred to boards at session start; `step_id` is the join key. Mobile shows step prompts (test-guide) on OLED. |

### 9.3 Open Items

(None currently open.)
