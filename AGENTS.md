# AGENTS.md

> **This is a navigation aid, not a source of truth.**
> Canonical truth lives in `docs/` and the code. If anything here disagrees
> with the docs, the docs are right — fix this file.

A condensed orientation for AI agents (and humans) joining the project.
Designed to get a fresh context to "ready to work" without re-reading every
doc. Last updated: 2026-07-08.

---

## What this is

ESP32AntTest is a two-board RF link characterization instrument: measure
board-to-board RSSI between two ESP32 dev boards (ESP32-WROOM-32 and
ESP32-C3) in two RF modes — WiFi SoftAP/STA and ESP-NOW — to compare
antennas, orientations, and TX power. No external router. Mobile (OLED +
button, USB-powered) initiates pings; Station (USB-serial logging + LittleFS)
responds and logs. Full spec: [docs/SPEC.md](docs/SPEC.md).

## Repo map

```
README.md                  # overview, points to SPEC/HARDWARE
docs/
  SPEC.md                  # THE spec — protocol, schema, build, NFRs, resolved items
  HARDWARE.md              # board pin maps, wiring diagrams, power notes
  GLOSSARY.md              # terminology (RSSI_MOB/STA, burst/run/session, ...)
  ADR-001-rssi-method.md   # RSSI capture: native WiFi API (Mode A), promiscuous cb (Mode B)
  ADR-002-protocol-stack.md# UDP over SoftAP/STA (Mode A), native ESP-NOW (Mode B)
  ADR-003-toolchain.md     # ESP-IDF v5.2+, plain C, idf.py CLI
firmware/
  mobile/                  # PLANNED — empty (no CMakeLists, no main/, no board_config.h)
  station/
    partitions.csv         # IMPLEMENTED — 4 MB flash, no OTA, 1 MB LittleFS logs (subtype 0x82)
  shared/
    include/
      config.h            # IMPLEMENTED — all tuneable constants (dBm units, OLED addr, pins)
      protocol.h          # IMPLEMENTED — ant_packet_t struct (16 bytes) + encode/decode decls
    src/                   # PLANNED — protocol.c (encode/decode) not yet written
tests/                     # PLANNED — empty
tools/                     # PLANNED — empty
refs/                      # datasheets + pinout images (read-only reference)
```

**Implementation status: pre-implementation.** Only the two shared headers and
the Station partition table exist. No firmware logic, no CMakeLists anywhere,
no `protocol.c`, no tests.

## Key decisions (pointers, not re-statements)

- **RSSI method** — Mode A: native IDF WiFi API (`esp_wifi_sta_get_ap_info`,
  `esp_wifi_ap_get_sta_list`). Mode B: promiscuous callback, filter by peer MAC.
  See [ADR-001](docs/ADR-001-rssi-method.md).
- **Transport** — Mode A: UDP on port 5432 over SoftAP/STA. Mode B: native
  ESP-NOW. Both start in Mode A. See [ADR-002](docs/ADR-002-protocol-stack.md).
- **Toolchain** — ESP-IDF v5.2+, plain C, `idf.py` CLI, no Arduino/PIO.
  See [ADR-003](docs/ADR-003-toolchain.md).
- **Roles/boards** — Mobile and Station are role flags (`ROLE_MOBILE`/
  `ROLE_STATION`), decoupled from the physical board. Config A (default):
  Mobile=WROOM-32, Station=C3. Config B: swapped. Pin maps are board-keyed
  in `board_config.h` (not yet written). See [SPEC §2.1](docs/SPEC.md).
- **TX power units** — dBm everywhere user-facing (config, packet `tx_power`
  field, CSV). Convert to 0.25 dBm only at the `esp_wifi_set_max_tx_power()`
  call via `ANT_DBM_TO_IDF()`. C3 clamps 2 dBm up to ~3 dBm; read back actual.
  See [SPEC §6](docs/SPEC.md) and [config.h](firmware/shared/include/config.h).
- **Time source** — Per-sample `timestamp_ms` is boot-relative (monotonic).
  Wall-clock `datetime` column set via serial `SETTIME` injection at boot
  (no RTC, no SNTP — Station is a SoftAP with no uplink). See [SPEC Time Source](docs/SPEC.md).
- **Partition** — 4 MB flash, no OTA, 1 MB LittleFS `logs` partition.
  See [firmware/station/partitions.csv](firmware/station/partitions.csv).
- **Filesystem** — LittleFS (power-loss resilient, wear-leveled), not SPIFFS.
- **OLED I2C address** — 0x3C confirmed (`ANT_OLED_I2C_ADDR`); 0x3D on some modules.
- **Mobile power** — USB only, no battery circuit.

## Conventions

- **Language:** C (not C++). ESP-IDF native APIs only.
- **Shared code** lives in `firmware/shared/`, referenced as a local component
  (`components/ant_shared -> ../../shared` symlink, per ADR-003).
- **Packet** — `ant_packet_t`, 16 bytes, `__attribute__((packed))`. Magic
  `0xAE 0x32`, version `0x01`. Types: PING/PONG/MODE_SWITCH/MODE_ACK. See
  [protocol.h](firmware/shared/include/protocol.h).
- **Data record / CSV schema** — see [SPEC §3.6 / §4](docs/SPEC.md).
  Columns: `session_id,run_id,seq,timestamp_ms,datetime,mode,tx_mob,tx_sta,rssi_mob,rssi_sta,status`.
- **Config** — all tuneables in `config.h`; board-specific overrides in
  `board_config.h` (not yet written).
- **Build** — `idf.py set-target <esp32|esp32c3>` in each firmware dir, then `build`.

## Out of scope

Throughput/latency measurement, >2 boards, 5 GHz, external network/cloud,
OTA updates, battery/RTC hardware, sleep modes. See [SPEC §1](docs/SPEC.md).

## Current next step

1. `firmware/shared/src/protocol.c` — implement `ant_packet_encode`/
   `ant_packet_decode` (declared in `protocol.h`).
2. `tests/` — unit test for encode/decode round-trip + magic/version validation.
3. `firmware/shared/CMakeLists.txt` as a local component.
4. Station firmware skeleton (simpler role: no UI): `main.c`, `logger.c`,
   `wifi_mode.c`, `espnow_mode.c`, `board_config.h`, `CMakeLists.txt`.
5. Mobile firmware skeleton after Station is running.

Update the "Implementation status" line above and this list as work lands.
