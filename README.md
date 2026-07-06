# ESP32AntTest

A two-board RF link characterization tool for ESP32 devices. Measures board-to-board signal quality (RSSI) in two RF modes — **WiFi SoftAP/STA peer** and **ESP-NOW** — with no external router required.

## Purpose

Test and compare antenna types, orientations, and transmit power settings between two ESP32 boards across real-world distances and conditions.

## Hardware

Two ESP32 dev boards (an ESP32-C3 and an ESP32-WROOM-32) fill the Mobile and Station roles. The role-to-board mapping is a compile-time choice — either board can fill either role (see [docs/HARDWARE.md](docs/HARDWARE.md)):

| Config | Mobile | Station |
|--------|--------|---------|
| **A** (current default) | ESP32-WROOM-32 | ESP32-C3 |
| **B** | ESP32-C3 | ESP32-WROOM-32 |

| Role | Peripherals | Power |
|------|-------------|-------|
| **Mobile** | 0.96" SSD1306 OLED (I2C) + tactile button | Battery |
| **Station** | None (USB-serial logging + LittleFS) | USB |

## Quick Start

See [docs/SPEC.md](docs/SPEC.md) for full specification.  
See [docs/HARDWARE.md](docs/HARDWARE.md) for wiring diagrams and pin assignments.

```
firmware/
  mobile/    # Mobile firmware — OLED UI, button input, session control
  station/   # Station firmware — data logging, serial output, LittleFS
  shared/    # Common packet structs, protocol constants
docs/
  SPEC.md
  GLOSSARY.md
  HARDWARE.md
  ADR-001-rssi-method.md
  ADR-002-protocol-stack.md
  ADR-003-toolchain.md
```

## Test Scenarios

1. **Range Walk** — press button every increment, collect bursts at each stop
2. **Time Soak** — auto-sample every 5 seconds until stopped; observe signal variation over time

## Build

Built with ESP-IDF v5.x. See [docs/SPEC.md](docs/SPEC.md#build) for setup instructions.

## License

MIT
