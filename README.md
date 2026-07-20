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
| **Mobile** | 0.96" SSD1306 OLED (I2C) + tactile button | USB (power bank/charger) |
| **Station** | None (USB-serial logging + LittleFS) | USB |

## Quick Start

See [docs/SPEC.md](docs/SPEC.md) for full specification.  
See [docs/HARDWARE.md](docs/HARDWARE.md) for wiring diagrams and pin assignments.

```
firmware/
  mobile/    # Mobile firmware — OLED UI, button input, session control (planned)
  station/   # Station firmware — data logging, serial output, LittleFS (skeleton)
  hwtest/    # C3 board bring-up (I2C scan + OLED + button)
  shared/    # Common packet structs, protocol constants
docs/
  SPEC.md
  GLOSSARY.md
  HARDWARE.md
  DEVENV.md
  SERIAL_PROTOCOL.md
  ADR-001-rssi-method.md
  ADR-002-protocol-stack.md
  ADR-003-toolchain.md
  ADR-004-beacon-sampling-and-host-tool.md
```

## Test Scenarios

1. **Range Walk** — press button at each distance step, collect beacon RSSI samples
2. **Orientation** — single-axis rotation sweep; polar RSSI-vs-angle plot exposes radiation-pattern asymmetry
3. **Time Soak** — auto-sample every 5 s until stopped; observe signal variation over time

## Build

Built with ESP-IDF v5.x. See [docs/SPEC.md](docs/SPEC.md#build) for setup instructions.

The host-side analysis tool (protocol authoring, session execution, plots) lives
in `tools/` and runs from a Python venv — see [tools/README.md](tools/README.md).

## License

MIT
