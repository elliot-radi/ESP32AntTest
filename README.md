# ESP32AntTest

A two-board RF link characterization tool for ESP32 devices. Measures board-to-board signal quality (RSSI) in two RF modes — **WiFi SoftAP/STA peer** and **ESP-NOW** — with no external router required.

## Purpose

Test and compare antenna types, orientations, and transmit power settings between two ESP32 boards across real-world distances and conditions.

## Hardware

Two ESP32 dev boards (an ESP32-C3 and an ESP32-WROOM-32) fill the Mobile and Station roles. The role-to-board mapping is a compile-time choice — either board can fill either role (see [docs/HARDWARE.md](docs/HARDWARE.md)):

| Config | Station | Mobile |
|--------|---------|--------|
| **A** (current default) | ESP32-WROOM-32 | ESP32-C3 |
| **B** | ESP32-C3 | ESP32-WROOM-32 |


| Role | Peripherals | Power |
|------|-------------|-------|
| **Station** | None (USB-serial logging + LittleFS) | USB |
| **Mobile** | 0.96" SSD1306 OLED (I2C) + tactile button | USB (power bank/charger) |


## Quick Start

See [docs/SPEC.md](docs/SPEC.md) for full specification.  
See [docs/HARDWARE.md](docs/HARDWARE.md) for wiring diagrams and pin assignments.

```
firmware/
  mobile/    # Mobile — Quick-Check, guided steps, beacons, outage buffer
  station/   # Station — SoftAP/ESP-NOW, session log stream, LittleFS
  hwtest/    # C3 board bring-up (I2C scan + OLED + button)
  shared/    # Common packet structs, protocol constants
tools/       # Host web UI + analyze CLI (see tools/README.md)
protocols/   # Guided session JSON (edit in-repo; no UI editor yet)
docs/
  SPEC.md, HARDWARE.md, DEVENV.md, SERIAL_PROTOCOL.md, ADRs, …
```

## Test Scenarios

1. **Range Walk** — host-loaded `range_walk` protocol; short-press at each distance; continuous beacons log RSSI
2. **Orientation** — host-loaded orientation sweep; polar RSSI-vs-angle plot exposes radiation-pattern asymmetry
3. **Time Soak** — host-guided `soak` / long `free` step; continuous beacons for a fixed placement; analyze RSSI vs time

## Build / run

**Firmware (ESP-IDF v5.x):** flash from a machine with the toolchain (often the
dev VM). See [docs/SPEC.md](docs/SPEC.md#build), [docs/DEVENV.md](docs/DEVENV.md),
[firmware/NOTES.md](firmware/NOTES.md).

**Host instrument UI:** Python venv on the **PC that has Station USB** (need not
be the build VM — shared project tree is enough):

```bash
./tools/antTestServe.sh            # foreground; Ctrl+C to stop
# or: ./tools/antTestServe.sh --background
```

Details: [tools/README.md](tools/README.md). Plots: `tools/analyze.py` on
`logs/host/*.csv` (UI wrap still pending).

## License

MIT
