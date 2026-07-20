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

**Token discipline:** ESP-IDF boot logs are ~2 KB of `I (...)` spam per
flash. Capture to a file, grep for `^#|^\$|^>|evt|panic|error:`, and paste
only the relevant slice — don't dump raw boot output into context.

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
- `oled_text` is **vendored in-repo** under `firmware/hwtest/components/oled_text/`
  (real files, not a symlink) — so a fresh clone builds without a
  machine-local shared lib. If Mobile later needs it, promote to a shared
  in-repo copy then.
- LittleFS is a managed component: `firmware/station/main/idf_component.yml`
  pulls `joltwallet/littlefs ^1.20` (NOT `esp_littlefs`, which fails to
  resolve). Populates `managed_components/` on first build.

## config.h override mechanism

`firmware/shared/include/config.h` defaults to **Config A (C3-Mobile)** pin
values (SDA=8, SCL=9, Button=5) behind `#ifndef` guards. A Config B
(WROOM-Mobile) build overrides via a `board_config.h` included *before*
`config.h`. The hwtest ships a `board_config.h` as an explicit wiring
assertion even though its values now match the config.h defaults.

## Gotchas hit here (don't re-walk)

Recorded so the next increment (or contributor) doesn't re-derive them.
Full detail + fix in the `esp-idf-build` skill.

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
- **`-Werror=format-truncation`** on path buffers built from `d_name` —
  size path buffers to ~300 bytes.
- **`fgets`/`fread` on USB-CDC VFS return partial lines** (`serial.c`) —
  ESP-IDF's USB-CDC VFS does NOT line-buffer on input; `fgets` returns as
  soon as any bytes arrive, not when `\n` arrives, so a command crossing
  a USB packet boundary is returned truncated and cJSON fails with
  "malformed JSON". Fix: accumulate bytes (fgetc loop) until `\n`, then
  dispatch the complete line. (Intermittent — fails ~20% under rapid-fire.)

## Reader task

The serial reader accumulates bytes until `\n` then dispatches the complete
line — do NOT use `fgets`/`fread` directly on USB-CDC stdin, which return
partial lines (see gotchas).
