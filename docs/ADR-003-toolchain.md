# ADR-003: Build Toolchain

**Status:** Accepted  
**Date:** 2026-07-03  
**Deciders:** Project architect

---

## Context

The project targets two different ESP32 chip families (ESP32 / Xtensa and ESP32-C3 / RISC-V) with potential future support for ESP32-S3. The toolchain must support both architectures, direct ESP-IDF API access (for promiscuous mode, `esp_wifi_*` calls, NVS, LittleFS), and a terminal-centric workflow.

---

## Decision

**ESP-IDF v5.2+ with `idf.py` CLI**, developed in a terminal environment.

Each firmware target (`mobile/`, `station/`) is a standalone ESP-IDF component-based project with its own `CMakeLists.txt`, `sdkconfig`, and `partitions.csv`. Shared code lives in `shared/` and is referenced as a local component.

Project structure follows ESP-IDF conventions:

```
firmware/
  mobile/
    CMakeLists.txt        # top-level project
    main/
      CMakeLists.txt
      main.c
      ui.c / ui.h
      wifi_mode.c / .h
      espnow_mode.c / .h
      board_config.h
    components/
      ant_shared -> ../../shared   # symlink or idf_component.yml reference
  station/
    CMakeLists.txt
    main/
      CMakeLists.txt
      main.c
      logger.c / .h
      wifi_mode.c / .h
      espnow_mode.c / .h
      board_config.h
    components/
      ant_shared -> ../../shared
  shared/
    CMakeLists.txt
    include/
      protocol.h          # ant_packet_t definition
      config.h            # tuneable constants
    src/
      protocol.c          # packet encode/decode helpers
```

Coding language: **C** (not C++). ESP-IDF is primarily a C API; C keeps the code portable to any ESP32 target without Arduino-ecosystem dependencies.

---

## Alternatives Considered

### Arduino IDE
Familiar and fast to start, but: no native component system, limited CLI support, `WiFi.RSSI()` is not valid in SoftAP mode or alongside ESP-NOW (as confirmed in research), and hidden HAL abstractions make it harder to use low-level promiscuous callbacks correctly. Rejected.

### PlatformIO
Better than Arduino IDE for multi-target projects and has good CLI support. However, PlatformIO wraps ESP-IDF and sometimes lags in IDF version support. For a project that deliberately uses low-level IDF APIs (`esp_wifi_set_promiscuous_rx_cb`, `esp_wifi_ap_get_sta_list`, LittleFS partition tables), direct IDF is cleaner. Rejected.

### MicroPython
Some prior art exists (RSSI display on OLED via MicroPython ESP-NOW). However, the promiscuous mode callback must run at interrupt/driver level — MicroPython's GIL and GC pauses make this unreliable. Rejected.

---

## Consequences

- Developer must have ESP-IDF v5.2+ installed and `idf.py` in PATH before building
- Setting the target (`idf.py set-target esp32c3`) must be done before the first build in each project directory
- `sdkconfig` files are committed to the repo (with board-specific overrides documented in `HARDWARE.md`)
- VS Code with the ESP-IDF extension or any terminal editor works; no IDE is required
- GitHub Actions CI can use `espressif/esp-idf` Docker image for automated build checks (future)
