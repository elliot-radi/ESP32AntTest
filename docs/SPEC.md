# ESP32AntTest — System Specification

**Version:** 0.1.0-draft  
**Status:** Pre-implementation  
**Last Updated:** 2026-07-03

---

## 1. Purpose and Scope

ESP32AntTest is a portable, self-contained RF link characterization instrument. It measures board-to-board received signal strength (RSSI) between two ESP32 devices in two RF modes — WiFi SoftAP/STA and ESP-NOW — without any external router or access point.

The primary use cases are:

- Comparing antenna types (e.g. ceramic PCB vs. FPC vs. quarter-wave monopole)
- Evaluating antenna orientations at fixed distance
- Mapping link quality vs. physical distance (range walk)
- Observing signal variation over time under real-world interference (time soak)
- Comparing transmit power settings

### Out of Scope

- Throughput or latency measurement
- Multi-board topologies (>2 boards)
- 5 GHz band
- Integration with any external network or cloud service

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
| Display | 0.96" SSD1306 OLED, 128×64 px | I2C |
| Input | Single tactile push-button | GPIO, active-low, internal pull-up |
| Power | LiPo battery + regulator (user-supplied) | — |

Default Mobile pins by board (configurable in `board_config.h`):

| Signal | ESP32-C3 | ESP32-WROOM-32 |
|--------|----------|----------------|
| OLED SDA | GPIO 8 | GPIO 21 |
| OLED SCL | GPIO 9 | GPIO 22 |
| Button | GPIO 5 | GPIO 17 |

(GPIO 0 is avoided as the button pin on the WROOM-32 because it is a strapping pin.)

### 2.3 Station Peripherals

| Peripheral | Purpose |
|-----------|---------|
| USB-Serial (built-in) | Live data output to host PC terminal |
| LittleFS (onboard flash) | Persistent session log storage |

### 2.4 RF Notes

- Both boards operate on 2.4 GHz only
- WiFi channel: fixed to channel 1 by default (configurable in `shared/config.h`)
- ESP-NOW operates on the same channel as the WiFi driver
- TX power is configurable per session via Mobile menu (ESP-IDF `esp_wifi_set_max_tx_power()`)

---

## 3. Functional Requirements

### 3.1 RF Modes

#### Mode A: WiFi Peer (SoftAP/STA)

- Station board runs as SoftAP (SSID: `AntTest-<MAC4>`, open auth)
- Mobile board connects as STA
- RSSI acquisition:
  - **Mobile (STA) downlink RSSI:** `esp_wifi_sta_get_ap_info()` → `wifi_ap_record_t.rssi`
  - **Station (AP) uplink RSSI:** `esp_wifi_ap_get_sta_list()` → `wifi_sta_info_t.rssi`
- Station sends its RSSI value to Mobile in each ping response payload
- Mobile displays both values

#### Mode B: ESP-NOW Peer

- Both boards register as ESP-NOW peers (broadcast MAC initially, then unicast by MAC exchange at pairing)
- RSSI acquisition on both boards: promiscuous mode callback (`esp_wifi_set_promiscuous_rx_cb`), filtered by peer MAC, reading `wifi_promiscuous_pkt_t.rx_ctrl.rssi`
- Each board piggybacks its locally-measured RSSI into the next outbound packet payload
- Station sends its RSSI in each ping response; Mobile displays both

### 3.2 Session Model

A **session** is a named collection of test runs initiated and terminated by the user. Each session has:

- A session ID (timestamp-based, e.g. `20260703_143022`)
- RF mode (WiFi / ESP-NOW)
- TX power setting for each board
- Zero or more **runs**

A **run** is a single burst or interval of RSSI samples collected at one position/configuration. Each sample is one ping-response exchange.

### 3.3 Test Modes

#### Manual (Range Walk)

- User presses button to trigger one burst
- A burst = N rapid-fire ping-response exchanges (default N=10, configurable)
- Each burst is logged as one run entry
- Intended use: stop at a new position, press button, move, repeat

#### Auto (Time Soak)

- User starts recording; system auto-samples every T seconds (default T=5)
- Recording continues until user presses button again to stop
- All samples within one start/stop are one run entry
- Intended use: leave in place for extended period to observe temporal variation

### 3.4 Mobile UI

Single button with three gesture types:

| Gesture | Definition | Action |
|---------|-----------|--------|
| Short press | < 500 ms | Scroll / Next |
| Long press | ≥ 1500 ms | Select / Confirm |
| Double press | Two presses < 400 ms apart | Back / Cancel |

#### Menu Tree

```
[HOME]
  ├── Mode ──────────────► [WiFi] | [ESP-NOW]   (long press to confirm)
  ├── TX Power ──────────► [2 dBm] | [10 dBm] | [17 dBm] | [20 dBm]
  ├── Test Type ─────────► [Manual] | [Auto (5s)]
  ├── Start / Stop ──────► begins or ends current run
  └── Session ───────────► [New Session] | [End Session]
```

Scroll wraps at ends. Long press on `Start` begins a run; long press on `Stop` (same position, label changes) ends it.

### 3.5 Mobile Display Layout (128×64)

```
┌────────────────────────┐
│ Mode: ESP-NOW  Pwr:17  │  row 0 — status bar
│ Mobile RSSI: -67 dBm   │  row 1
│ Station RSSI: -71 dBm  │  row 2
│ Samples:  42  Run: 003 │  row 3
│ [RECORDING ●]          │  row 4 — state indicator
└────────────────────────┘
```

When navigating menus, rows 1–4 become menu items. Status bar remains visible at all times.

### 3.6 Station Output

#### Serial (live, human-readable)

```
[20260703_143022][RUN-003][ESP-NOW][SEQ:0042] MOB:-67 STA:-71 dBm | TX_MOB:17 TX_STA:17
```

#### LittleFS log (CSV, one file per session)

```
session_id,run_id,seq,timestamp_ms,mode,tx_mob,tx_sta,rssi_mob,rssi_sta,status
20260703_143022,003,0042,847293,ESPNOW,17,17,-67,-71,OK
```

`status` values: `OK`, `TIMEOUT`, `ERR_DECODE`

---

## 4. Data Record Schema

| Field | Type | Description |
|-------|------|-------------|
| `session_id` | string | `YYYYMMDD_HHMMSS` of session start |
| `run_id` | uint16 | Incrementing within session |
| `seq` | uint32 | Packet sequence number within run |
| `timestamp_ms` | uint64 | ms since Station boot |
| `mode` | enum | `WIFI` or `ESPNOW` |
| `tx_mob` | int8 | Mobile TX power (dBm) |
| `tx_sta` | int8 | Station TX power (dBm) |
| `rssi_mob` | int8 | RSSI at Mobile of Station's packet (dBm) |
| `rssi_sta` | int8 | RSSI at Station of Mobile's packet (dBm) |
| `status` | enum | `OK`, `TIMEOUT`, `ERR_DECODE` |

---

## 5. Communication Protocol

### 5.1 Packet Structure (shared)

```c
typedef struct {
    uint8_t  magic[2];      // 0xAE, 0x32
    uint8_t  version;       // protocol version, currently 0x01
    uint8_t  type;          // PKT_PING=0x01, PKT_PONG=0x02
    uint32_t seq;           // sequence number
    uint32_t session_id;    // lower 32 bits of session timestamp
    uint16_t run_id;
    int8_t   rssi_local;    // RSSI measured by the sender of this packet
    int8_t   tx_power;      // sender's current TX power
    uint8_t  reserved[4];
} ant_packet_t;             // 16 bytes total
```

Mobile sends `PKT_PING` with its locally-measured Station RSSI (from previous pong) and its TX power.  
Station replies with `PKT_PONG` with its locally-measured Mobile RSSI and its TX power.

### 5.2 Timeout and Retry

- Ping timeout: 2000 ms
- Retries: 0 (a missed ping is logged as `TIMEOUT`, not retried — retries would bias range data)
- Connection loss declared after 5 consecutive timeouts

---

## 6. Configuration

`shared/config.h` contains all tuneable defaults:

```c
#define ANT_WIFI_CHANNEL       1
#define ANT_BURST_SIZE         10       // pings per Manual burst
#define ANT_AUTO_INTERVAL_MS   5000     // Auto mode sample interval
#define ANT_PING_TIMEOUT_MS    2000
#define ANT_LOSS_THRESHOLD     5        // consecutive timeouts = link lost
#define ANT_DEFAULT_TX_POWER   17       // dBm
#define ANT_OLED_SDA_PIN       21       // WROOM default (C3: 8) — see board_config.h
#define ANT_OLED_SCL_PIN       22       // WROOM default (C3: 9)
#define ANT_BUTTON_PIN         17       // WROOM default (C3: 5)
#define ANT_BTN_SHORT_MAX_MS   500
#define ANT_BTN_LONG_MIN_MS    1500
#define ANT_BTN_DOUBLE_GAP_MS  400
```

---

## 7. Build

### Requirements

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
idf.py -p /dev/ttyUSB1 flash monitor
```

---

## 8. Non-Functional Requirements

| Property | Requirement |
|----------|-------------|
| Ping round-trip latency | < 200 ms at full signal |
| OLED refresh rate | ≥ 2 Hz during active recording |
| LittleFS capacity | ≥ 10,000 records per session (well within 1 MB flash partition) |
| Button debounce | Hardware RC or software 20 ms debounce |
| Portability | Firmware compiles for esp32, esp32c3, esp32s3 targets with only `board_config.h` changes |

---

## 9. Open Items

| ID | Item | Owner |
|----|------|-------|
| OI-01 | Confirm SSD1306 I2C address (0x3C vs 0x3D) on user's module | Hardware bring-up |
| OI-02 | Decide whether Station clock is wall-time (SNTP) or boot-relative ms | Design |
| OI-03 | Define LittleFS partition size in `partitions.csv` | Firmware |
| OI-04 | Battery circuit for Mobile (voltage divider + ADC for battery indicator?) | Hardware |
