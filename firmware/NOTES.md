# ESP32AntTest — Firmware Dev Notes

Working notes for building and flashing the firmware. Project-specific;
the general ESP-IDF build/debug workflow (backtrace decoding, the
mutex-vs-printf and stack-size gotchas, token discipline on boot logs)
lives in the `esp-idf-build` skill, loaded on-demand.

See [docs/SPEC.md §7](../docs/SPEC.md) for the canonical build commands and
[docs/HARDWARE.md](../docs/HARDWARE.md) / [docs/DEVENV.md](../docs/DEVENV.md)
for board wiring + USB passthrough.

## Config A (default): C3-Mobile / WROOM-Station

| Firmware | Target | Port | Notes |
|----------|--------|------|-------|
| `firmware/station/` | `esp32` | `/dev/ttyUSB0` | WROOM-32 (USB-UART bridge; port persists through reset) |
| `firmware/mobile/` | `esp32c3` | `/dev/ttyACM0` | C3-Zero/SuperMini (native USB; port re-enumerates on reset) |
| `firmware/hwtest/` | `esp32c3` | `/dev/ttyACM0` | C3 board bring-up (OLED + button); runs on the Mobile board |

Config B swaps the boards; the pin maps are board-keyed in `board_config.h`
so only `set-target` and the `-p` port change.

## What builds / flashes where

| Module | Key sources | Role |
|--------|-------------|------|
| Station RF | `station/main/rf.c` (`ant_rf_*`) | SoftAP, UDP beacons, ESP-NOW, promiscuous RSSI, log on session |
| Station serial | `serial.c` / `session.c` / `logger.c` | Host `#`/`$`/`>` contract + LittleFS |
| Mobile RF | `mobile/main/mrf.c` (`ant_mrf_*`) | STA join `AntTest-*`, beacons, protocol RX, outage buffer |
| Mobile UI | `ui.c` / `button.c` + vendored `oled_text` | Quick-Check OLED + menu gestures |

**API prefix rule:** do **not** export `rf_init` / `rf_start` — ESP-PHY already
defines `rf_init`, and the linker fails with `multiple definition of rf_init`.
Station = `ant_rf_*`, Mobile = `ant_mrf_*`.

## Build / flash / capture (non-interactive)

`idf.py monitor` needs a TTY (it wants stdin for keypresses). In this
agent shell, read the UART with pyserial instead:

```bash
cd firmware/station
idf.py build
idf.py -p /dev/ttyUSB0 flash
# capture (reset + read 8s):
/home/elliot/.espressif/python_env/idf5.2_py3.14_env/bin/python - <<'PY'
import serial, time, sys
with serial.Serial("/dev/ttyUSB0", 115200, timeout=0.2) as s:
    s.dtr=False; s.rts=True; time.sleep(0.1); s.dtr=False; s.rts=False; time.sleep(0.05)
    s.reset_input_buffer()
    end=time.time()+8
    while time.time()<end:
        n=s.in_waiting
        if n: sys.stdout.write(s.read(n).decode("utf-8","replace")); sys.stdout.flush()
        else: time.sleep(0.05)
PY
```

To send a serial command: `s.write(b'$ {"cmd":"hello"}\n')`.

**C3 after flash/reset:** `/dev/ttyACM0` may vanish briefly while native USB
re-enumerates. Retry open for ~2–5 s before declaring the port dead. In a VM
see DEVENV.md if it never comes back (libvirt hostdev bus/dev pin).

**Token discipline:** ESP-IDF boot logs are ~2 KB of `I (...)` spam per
flash. Capture to a file, grep for `^#|^\$|^>|evt|panic|error:| mrf:| rf:`,
and paste only the relevant slice — don't dump raw boot output into context.

### Smoke-test the RF link (both boards)

With Station already SoftAP'ing and Mobile associated:

```bash
# On Station serial — start a short logged session and expect > rows at ~5 Hz
$ {"cmd":"settime","iso":"2026-07-20T12:30:00"}
$ {"cmd":"start_session","mode":"WIFI","tx_mob":17,"tx_sta":17"}
# … >session_id,step,seq,...,WIFI,17,16,<rssi_mob>,<rssi_sta>,STA,OK
$ {"cmd":"end_session"}
```

Healthy Quick-Check join look like: Station `STA joined: …`, Mobile
`found AP AntTest-XXXX` → `got ip: 192.168.26.2` → beaconing. Dual RSSI
columns both populated means piggyback works.

## Target swap needs fullclean

`build/CMakeCache.txt` bakes in the old target's toolchain path. Switching
`esp32c3` → `esp32` (or back) without cleaning fails with
`include could not find requested file: .../toolchain-esp32c3.cmake`:

```bash
idf.py fullclean      # removes build/ AND managed_components/
idf.py set-target esp32
idf.py build
```

## Shared component wiring

- `firmware/shared/` is referenced as a local component via a per-project
  `components/ant_shared -> ../../shared` symlink (per ADR-003). Each
  firmware dir has its own `components/` with the symlinks it needs.
- `oled_text` is **vendored in-repo** under both
  `firmware/hwtest/components/oled_text/` and
  `firmware/mobile/components/oled_text/` (real files, not a symlink) —
  so each project builds from a fresh clone. Optional later promote to a
  single shared tree if the copies drift.
- LittleFS is a managed component: `firmware/station/main/idf_component.yml`
  pulls `joltwallet/littlefs ^1.20` (NOT `esp_littlefs`, which fails to
  resolve). Populates `managed_components/` on first build.

## config.h override mechanism

`firmware/shared/include/config.h` defaults to **Config A (C3-Mobile)** pin
values (SDA=8, SCL=9, Button=5) behind `#ifndef` guards. A Config B
(WROOM-Mobile) build overrides via a `board_config.h` included *before*
`config.h`. Mobile and hwtest both ship a C3 `board_config.h` as an
explicit wiring assertion.

## SoftAP / WiFi RF notes (Station + Mobile)

- Station SSID: `AntTest-<last2 MAC bytes hex>` (e.g. `AntTest-6BE1`).
- Static SoftAP IP: `ANT_STATION_IP` / `192.168.26.1` (not IDF's default
  192.168.4.1). **Set `esp_netif` IP + stop/start DHCPS before
  `esp_wifi_start()`** — setting it after start leaves DHCPS on 192.168.4.1
  while logs claim 192.168.26.1 (Mobile then gets the wrong gateway/subnet).
- Mobile must **`esp_wifi_start()` before any scan**; scan-before-start
  silently finds zero APs.
- Prefer **all-channel scan** (`channel = 0`) then connect to the matched
  SSID; locking the scan to ch1 before SoftAP is known can miss it during
  bring-up races.
- TX power: store dBm user/log units; only convert with `ANT_DBM_TO_IDF()`
  at `esp_wifi_set_max_tx_power()`; read back and log actual (WROOM 17→~16).

## Gotchas hit here (don't re-walk)

Recorded so the next increment (or contributor) doesn't re-derive them.
General IDF items also live in the `esp-idf-build` skill.

### Serial / session / LittleFS (Increment 1)

- **`portENTER_CRITICAL` around `printf` aborts** (`serial.c`) — use a
  recursive FreeRTOS mutex; create it before the first emit.
- **Reader task stack overflow** (`serial.c`, 4 KB → 12 KB) — `fopen` +
  LittleFS VFS + cJSON overflows a 4 KB stack; panic signature is
  `IllegalInstruction`/`LoadProhibited`.
- **`sscanf` scanset `[T ]` doesn't feed `%d`** (`session.c`) — parse the
  ISO separator as explicit `%c`.
- **LittleFS `d_type` is `DT_UNKNOWN`** (`logger.c`) — use `stat()` +
  `S_ISREG`, not `d_type != DT_REG`.
- **`esp_vfs_littlefs_conf_t` field is `base_path`**, not `mount_point`.
- **`-Werror=format-truncation`** on path buffers / SSID copies — size
  path buffers ~300 B; for fixed-size WiFi SSID fields prefer `strncpy`
  into `sizeof(cfg.sta.ssid) - 1` rather than `snprintf`.
- **`fgets`/`fread` on USB-CDC VFS return partial lines** (`serial.c`) —
  ESP-IDF's USB-CDC VFS does NOT line-buffer on input; `fgets` returns as
  soon as any bytes arrive, not when `\n` arrives. Fix: accumulate bytes
  (`fgetc` loop) until `\n`, then dispatch. (Intermittent — ~20% under
  rapid-fire.)

### RF link (Increment 2)

- **`rf_init` linker clash** — ESP-PHY exports `rf_init`. Use `ant_rf_*` /
  `ant_mrf_*` prefixes (skill §8).
- **SoftAP IP after `esp_wifi_start()`** — DHCPS stays on 192.168.4.1; set
  netif IP before start (above).
- **STA scan before `esp_wifi_start()`** — always empty; start first.
- **`esp_mac` is not a standalone IDF component** — on IDF 5.2 don't put
  `esp_mac` in `REQUIRES` (resolve failure). `esp_read_mac` comes via
  existing wifi/system deps; include `esp_mac.h` is fine.
- **Reconnect storm when SSID is empty** — don't call `esp_wifi_connect()`
  on every disconnect if no target SSID was discovered yet.

## Reader task

The serial reader accumulates bytes until `\n` then dispatches the complete
line — do NOT use `fgets`/`fread` directly on USB-CDC stdin, which return
partial lines (see gotchas).
