# AGENTS.md

> **This is a navigation aid, not a source of truth.**
> Canonical truth lives in `docs/` and the code. If anything here disagrees
> with the docs, the docs are right — fix this file.

A condensed orientation for AI agents (and humans) joining the project.
Designed to get a fresh context to "ready to work" without re-reading every
doc. Last updated: 2026-07-09.

---

## What this is

ESP32AntTest is a two-board RF link characterization instrument: measure
board-to-board RSSI between two ESP32 dev boards (ESP32-WROOM-32 and
ESP32-C3) in two RF modes — WiFi SoftAP/STA and ESP-NOW — to compare
antennas, **orientations**, and TX power. No external router. **Two co-equal
deliverables:** (1) firmware (Mobile + Station, the sensor head) and (2) a
host-side web tool (protocol authoring, session execution, live view, and
the analysis plots that are the actual value). Full spec:
[docs/SPEC.md](docs/SPEC.md).

## Repo map

```
README.md                  # overview, points to SPEC/HARDWARE
docs/
  SPEC.md                  # THE spec (v0.3) — beacon model, schema, build, NFRs, resolved items
  HARDWARE.md              # board pin maps, wiring diagrams, power notes
  GLOSSARY.md              # terminology (RSSI_MOB/STA, burst/run/step/session, ...)
  ADR-001-rssi-method.md   # RSSI capture (per-beacon; addendum points to ADR-004)
  ADR-002-protocol-stack.md# UDP SoftAP/STA + ESP-NOW transport (addendum: sampling now beacon)
  ADR-003-toolchain.md     # ESP-IDF v5.2+, plain C, idf.py CLI
  ADR-004-beacon-sampling-and-host-tool.md  # beacon model + host webserver + quick-check (the architecture revision)
protocols/
  range_walk.json          # example protocol: distance steps
  orientation_z.json       # example protocol: Z-axis 45° orientation sweep
profiles/
  good_antenna.json        # synthetic antenna profile (FPC, good)
  bad_antenna.json        # synthetic antenna profile (ceramic, deep null)
firmware/
  mobile/                  # PLANNED — empty (no CMakeLists, no main/, no board_config.h)
  station/
    partitions.csv         # IMPLEMENTED — 4 MB flash, no OTA, 1 MB LittleFS logs (subtype 0x82)
  shared/
    include/
      config.h            # IMPLEMENTED — tuneables (dBm units, OLED addr, pins, beacon Hz, buffer)
      protocol.h          # IMPLEMENTED — ant_packet_t (16 bytes) + encode/decode decls (needs PKT_BEACON etc.)
    src/                   # PLANNED — protocol.c (encode/decode) not yet written
tools/
  mock_session.py          # IMPLEMENTED — synthetic beacon-mode log generator (design artifact)
  analyze.py               # IMPLEMENTED — per-step stats + range/orientation/time plots (design artifact)
  requirements.txt         # matplotlib (project venv)
  README.md                # what the mockup is and why
  server.py                # PLANNED — production FastAPI webserver (wraps analyze.py + serial bridge)
tests/                     # PLANNED — empty
logs/, plots/              # generated synthetic data + plots (design-process artifacts, committed)
refs/                      # datasheets + pinout images (read-only reference)
```

**Implementation status:** firmware pre-implementation (only the two shared
headers + Station `partitions.csv` exist). **Log schema and analysis
validated** via the `tools/` synthetic-data mockup (see ADR-004) — the schema
is locked; `protocol.c` and the Station/Mobile firmware are the next code.

## Key decisions (pointers, not re-statements)

- **Sampling model — beacon mode** (both boards beacon at 5 Hz, piggyback
  `rssi_local`; request-response dropped). Captures asymmetric/null-floor
  RSSI. Mobile RAM-buffers outage data, forwards on reconnect, host merges by
  `step_id`. See [ADR-004](docs/ADR-004-beacon-sampling-and-host-tool.md).
- **Host tool is a first-class deliverable** — local FastAPI + matplotlib +
  vanilla JS webserver; no cloud, no build step. See ADR-004 + [SPEC §3.7](docs/SPEC.md).
- **Quick-Check is the default power-up** — auto-connect WiFi, beacon,
  display live RSSI, no log. Guided session via browser; ad-hoc Manual/Auto
  fallback. See [SPEC §3.3](docs/SPEC.md).
- **RSSI method** — per-beacon RSSI via promiscuous callback (both modes),
  piggybacked. See [ADR-001](docs/ADR-001-rssi-method.md).
- **Transport** — UDP over SoftAP/STA (Mode A), native ESP-NOW (Mode B).
  See [ADR-002](docs/ADR-002-protocol-stack.md).
- **Toolchain** — ESP-IDF v5.2+, plain C, `idf.py` CLI. See [ADR-003](docs/ADR-003-toolchain.md).
- **Roles/boards** — `ROLE_MOBILE`/`ROLE_STATION` flags; Config A (default):
  Mobile=WROOM-32, Station=C3. Pin maps board-keyed in `board_config.h`.
  See [SPEC §2.1](docs/SPEC.md).
- **TX power units** — dBm everywhere; convert to 0.25 dBm only at the
  `esp_wifi_set_max_tx_power()` call via `ANT_DBM_TO_IDF()`. C3 clamps 2→~3.
  See [SPEC §6](docs/SPEC.md) + [config.h](firmware/shared/include/config.h).
- **Time source** — `timestamp_ms` boot-relative; wall-clock `datetime` via
  serial `SETTIME` injection (no RTC/SNTP). See [SPEC Time Source](docs/SPEC.md).
- **LittleFS** — Station durable fallback only (streams over serial *and*
  mirrors to flash); Mobile has none. See [SPEC §2.3](docs/SPEC.md).
- **Noise floor / retries** — both out of scope; loss inferred from count.
- **Orientation** — single-axis sweeps (`axis`+`angle_deg`); polar plot.
  See [SPEC §3.2](docs/SPEC.md).

## Conventions

- **Language:** C (not C++). ESP-IDF native APIs only.
- **Shared code** in `firmware/shared/`, referenced as a local component
  (`components/ant_shared -> ../../shared` symlink, per ADR-003).
- **Packet** — `ant_packet_t`, 16 bytes, `__attribute__((packed))`. Magic
  `0xAE 0x32`, version `0x01`. Types (v0.3): `PKT_BEACON`, `PKT_MARKER`,
  `PKT_PROTOCOL`, `PKT_MODE_ACK` (PING/PONG dropped). See
  [protocol.h](firmware/shared/include/protocol.h) + [SPEC §5](docs/SPEC.md).
- **Log schema** — [SPEC §3.6 / §4](docs/SPEC.md). Columns:
  `session_id,step_id,seq,timestamp_ms,datetime,mode,tx_mob,tx_sta,rssi_mob,rssi_sta,source,status`.
  `source` = STA (complete row) or MOB (Mobile-buffered during uplink outage).
- **Config** — tuneables in `config.h`; board overrides in `board_config.h`.
- **Build (firmware)** — `idf.py set-target <esp32|esp32c3>` per firmware dir.
- **Build (host tool)** — `python3 -m venv .venv && pip install -r tools/requirements.txt`.
  Mockup CLI: `tools/mock_session.py` + `tools/analyze.py`.

## Out of scope

Throughput/latency, >2 boards, 5 GHz, external network/cloud, OTA, noise-floor/SNR,
battery/RTC hardware, sleep modes, multi-axis orientation. See [SPEC §1](docs/SPEC.md).

## Current next step

1. `firmware/shared/src/protocol.c` — implement `ant_packet_encode`/
   `ant_packet_decode` (declared in `protocol.h`); add `PKT_BEACON`/
   `PKT_MARKER`/`PKT_PROTOCOL` to the enum in `protocol.h`.
2. `tests/` — unit test for encode/decode round-trip + magic/version validation.
3. `firmware/shared/CMakeLists.txt` as a local component.
4. Station firmware skeleton (serial protocol receive + forward to Mobile +
   log + stream to host), then Mobile (guided OLED mode + RAM buffer).
5. `tools/server.py` — FastAPI webserver wrapping `analyze.py` + serial bridge.

Update the "Implementation status" line above and this list as work lands.
