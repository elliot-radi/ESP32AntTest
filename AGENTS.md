# AGENTS.md

> **This is a navigation aid, not a source of truth.**
> Canonical truth lives in `docs/` and the code. If anything here disagrees
> with the docs, the docs are right — fix this file.

A condensed orientation for AI agents (and humans) joining the project.
Designed to get a fresh context to "ready to work" without re-reading every
doc. Last updated: 2026-07-20.

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
  HARDWARE.md              # board pin maps, wiring diagrams, power notes, USB-serial node notes
  DEVENV.md                # dev VM + host setup; USB passthrough re-enumeration trap & libvirt fix
  GLOSSARY.md              # terminology (RSSI_MOB/STA, burst/run/step/session, ...)
  ADR-001-rssi-method.md   # RSSI capture (per-beacon; addendum points to ADR-004)
  ADR-002-protocol-stack.md# UDP SoftAP/STA + ESP-NOW transport (addendum: sampling now beacon)
  ADR-003-toolchain.md     # ESP-IDF v5.2+, plain C, idf.py CLI
  ADR-004-beacon-sampling-and-host-tool.md  # beacon model + host webserver + quick-check (the architecture revision)
  SERIAL_PROTOCOL.md       # host<->Station serial contract (channels, commands, log stream)
protocols/
  range_walk.json          # example protocol: distance steps
  orientation_z.json       # example protocol: Z-axis 45° orientation sweep
profiles/
  good_antenna.json        # synthetic antenna profile (FPC, good)
  bad_antenna.json        # synthetic antenna profile (ceramic, deep null)
firmware/
  mobile/                  # IMPLEMENTED — C3 Mobile: Quick-Check STA, beacons, OLED UI, button,
                           #   RAM outage buffer; RF link verified on HW with Station
  station/                 # IMPLEMENTED — serial + session + LittleFS + SoftAP/UDP beacons +
                           #   promiscuous RSSI + session logging; verified on HW
                           #   see firmware/NOTES.md for build workflow + gotchas hit
  hwtest/                  # IMPLEMENTED — C3 board bring-up (I2C scan + OLED + button), verified on HW
  shared/
    CMakeLists.txt         # local component
    include/
      config.h            # tuneables (dBm units, OLED addr, pins, beacon Hz, buffer)
      protocol.h          # ant_packet_t (20 bytes) + PKT_BEACON/MARKER/PROTOCOL/MODE_ACK
    src/
      protocol.c          # encode/decode — implemented + host-C unit tested
tools/
  mock_session.py          # synthetic beacon-mode log generator (design artifact)
  analyze.py               # per-step stats + range/orientation/time plots (design artifact)
  requirements.txt         # fastapi, uvicorn, pyserial, matplotlib
  README.md                # mockup + host server run notes
  server.py                # FastAPI host UI: session control + live log (slice 1)
  serial_bridge.py         # Station serial client ($/>/#)
  static/                  # vanilla JS host UI
tests/                     # IMPLEMENTED — test_protocol.c + Makefile (host-C encode/decode round-trip)
logs/, plots/              # generated synthetic data + plots (design-process artifacts, committed)
refs/                      # datasheets + pinout images (read-only reference)
```

**Implementation status (2026-07-20):** Firmware sensor head is largely in
place (Station + Mobile Mode A, guided multi-step, TX via `PKT_PROTOCOL`,
ESP-NOW mode switch HW-checked, ESP-NOW dual-RSSI desk OK). Host tool slice 1
is in-tree and **HW-used**: connect Station, load `protocols/*.json`,
start/end session, live `>` tail, CSV mirror under `logs/host/`. Prefer
running `tools/server.py` on the **hypervisor host** with Station USB local
(shared repo); use the build VM for IDF flash/debug. Protocol **editor** is
deferred (edit JSON in `protocols/`). **Open:** asymmetric `source=MOB`
outage field proof; wrap `analyze.py` plots in the host UI (CLI works today).

## Key decisions (pointers, not re-statements)

- **Sampling model — beacon mode** (both boards beacon at 5 Hz, piggyback
  `rssi_local`; request-response dropped). Captures asymmetric/null-floor
  RSSI. Mobile RAM-buffers outage data, forwards on reconnect, host merges by
  `step_id`. See [ADR-004](docs/ADR-004-beacon-sampling-and-host-tool.md).
- **Host tool is a first-class deliverable** — local FastAPI + vanilla JS +
  analyze CLI (matplotlib); no cloud. Session UI = slice 1; protocol authoring
  UI deferred. See ADR-004 + [SPEC §3.7](docs/SPEC.md) + [tools/README.md](tools/README.md).
- **Quick-Check is the default power-up** — auto-connect WiFi, beacon,
  display live RSSI, no log. Logged characterization is host-guided only
  (protocol + session); ad-hoc Manual/Auto out of scope. See [SPEC §3.3](docs/SPEC.md).
- **RSSI method** — per-beacon RSSI via promiscuous callback (both modes),
  piggybacked. See [ADR-001](docs/ADR-001-rssi-method.md).
- **Transport** — UDP over SoftAP/STA (Mode A), native ESP-NOW (Mode B).
  See [ADR-002](docs/ADR-002-protocol-stack.md).
- **SoftAP addressing** — Station SoftAP SSID `AntTest-<MAC4>`, static IP
  `ANT_STATION_IP` (`192.168.26.1`), UDP `ANT_UDP_PORT` (5432). SoftAP IP
  must be applied **before** `esp_wifi_start()` or DHCPS keeps the IDF
  default `192.168.4.1`.
- **Toolchain** — ESP-IDF v5.2+, plain C, `idf.py` CLI. See [ADR-003](docs/ADR-003-toolchain.md).
- **Roles/boards** — Config A (default): Mobile=C3, Station=WROOM-32. Pin
  maps board-keyed in `board_config.h`. See [SPEC §2.1](docs/SPEC.md).
- **TX power units** — dBm everywhere; convert to 0.25 dBm only at the
  `esp_wifi_set_max_tx_power()` call via `ANT_DBM_TO_IDF()`. C3 clamps 2→~3.
  See [SPEC §6](docs/SPEC.md) + [config.h](firmware/shared/include/config.h).
- **Time source** — `timestamp_ms` boot-relative; wall-clock `datetime` via
  serial `settime` injection (no RTC/SNTP). See [SPEC Time Source](docs/SPEC.md).
- **LittleFS** — Station durable fallback only (streams over serial *and*
  mirrors to flash); Mobile has none. See [SPEC §2.3](docs/SPEC.md).
- **Noise floor / retries** — both out of scope; loss inferred from count.
- **Orientation** — single-axis sweeps (`axis`+`angle_deg`); polar plot.
  See [SPEC §3.2](docs/SPEC.md).

## Before touching firmware

Read [firmware/NOTES.md](firmware/NOTES.md) for this repo’s flash/capture
workflow, Config A ports, SoftAP/scan notes, and project-specific gotchas.
Load the `esp-idf-build` skill for shared ESP-IDF v5 traps (mutex-vs-printf,
stack sizes, LittleFS, C3 re-enum, `rf_init` clash, etc.). Don’t paste skill
or NOTES contents into this file — pointers only.

## Conventions

- **Language:** C (not C++). ESP-IDF native APIs only.
- **Shared code** in `firmware/shared/`, referenced as a local component
  via a per-project `components/ant_shared -> ../../shared` symlink (per
  ADR-003). Each firmware dir (`station/`, `mobile/`, `hwtest/`, …) has its
  own `components/` with the symlinks it needs.
- **RF API prefixes** — never name a public symbol `rf_init` (clashes with
  ESP-PHY). Station uses `ant_rf_*` (`rf.c`); Mobile uses `ant_mrf_*`
  (`mrf.c`). See [firmware/NOTES.md](firmware/NOTES.md).
- **oled_text** (SSD1306 text wrapper over the official `esp_lcd` driver,
  + a public-domain 5×7 font) is **vendored in-repo** under both
  `firmware/hwtest/components/oled_text/` and
  `firmware/mobile/components/oled_text/` (real files, not symlinks) so each
  project stays self-contained. Not an Arduino/Adafruit lib — ADR-003
  (plain C, native IDF). Future cleanup: one shared in-repo copy if drift
  becomes painful.
- **Packet** — `ant_packet_t`, 20 bytes, `__attribute__((packed))`. Magic
  `0xAE 0x32`, version `0x01`. Types (v0.3): `PKT_BEACON`, `PKT_MARKER`,
  `PKT_PROTOCOL`, `PKT_MODE_ACK` (PING/PONG dropped). See
  [protocol.h](firmware/shared/include/protocol.h) + [SPEC §5](docs/SPEC.md).
  `PKT_PROTOCOL` on the wire is extended: 20-byte header + `u16 total` +
  `u16 offset` + chunk bytes (Station↔Mobile only).
- **Log schema** — [SPEC §3.6 / §4](docs/SPEC.md). Columns:
  `session_id,step_id,seq,timestamp_ms,datetime,mode,tx_mob,tx_sta,rssi_mob,rssi_sta,source,status`.
  `source` = STA (complete row) or MOB (Mobile-buffered during uplink outage).
- **Config** — tuneables in `config.h`; board overrides in `board_config.h`
  (included *before* `config.h` so its `#ifndef` guards pick them up).
- **Build (firmware)** — `idf.py set-target <esp32|esp32c3>` per firmware dir.
  Serial port: WROOM USB-UART bridge → `/dev/ttyUSB*`; C3-Zero/SuperMini
  native USB JTAG/serial → `/dev/ttyACM*` (see [HARDWARE.md](docs/HARDWARE.md)).
- **Build (host tool)** — on host or VM: `python3 -m venv .venv && pip install -r tools/requirements.txt`.
  Run: `python tools/server.py --port 8000` (Station on this machine's USB).
  Mockup/analyze CLI: `tools/mock_session.py` + `tools/analyze.py`.

## Shared dev resources

This project lives in a VM with a host-shared folder mounted at
`/home/elliot/projects/share/lib/`. **Shared dev libraries live there, not
in `~/`** — prefer that location for anything bulky/reusable across VMs to
conserve VM disk. Notably:

- **ESP-IDF v5.2.3** — `/home/elliot/projects/share/lib/esp/esp-idf`.
  Source the environment with the `get_idf` shell alias (defined in
  `~/.bash_aliases`), which runs `. /home/elliot/projects/share/lib/esp/esp-idf/export.sh`.
  In a non-interactive shell, source that script directly.
- **Toolchains** — `~/.espressif/` (installed by `install.sh`; shared across
  IDF versions on this VM).
- **USB passthrough gotcha** — the C3 board (`/dev/ttyACM*`) disappears from
  the VM after a reset because virt-manager pins the USB bus/device number;
  the fix is a vendor/product-only libvirt `<hostdev>`. See
  [docs/DEVENV.md](docs/DEVENV.md).

If a tool/library seems missing, check `projects/share/lib/` before installing
into `~/`.

## Out of scope

Throughput/latency, >2 boards, 5 GHz, external network/cloud, OTA, noise-floor/SNR,
battery/RTC hardware, sleep modes, multi-axis orientation, **ad-hoc Manual/Auto**
(protocol-free Mobile logged sessions). See [SPEC §1](docs/SPEC.md).

## Current next step

1. **TODO (field):** Prove asymmetric outage path → `source=MOB` rows.
   Needs downlink OK + empty/stale Station piggyback >~2 s on Mobile, then
   drain (TX skew / geometry; SoftAP total drop alone is not enough).
2. Host UI: wrap `analyze.py` on `logs/host/*.csv` (range/orientation/time plots).
3. **Deferred:** in-browser protocol authoring (edit `protocols/*.json` instead).

Ad-hoc Manual/Auto are **out of scope** (SPEC DI-11). Time-soak = host guided
`soak`/`free` +
plots. Day-to-day characterization needs firmware flashed once; operate from
host-side `tools/server.py` + Station USB (no VM required for RF runs).

Update the "Implementation status" line above and this list as work lands.
