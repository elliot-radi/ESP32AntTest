# ESP32AntTest — Hardware Guide

## Mobile Unit (ESP32-C3 Dev Module)

### Components

| Item | Notes |
|------|-------|
| ESP32-C3 dev board | Any standard C3 dev module with USB-C |
| SSD1306 OLED 0.96" | 128×64, I2C variant (4-pin: VCC, GND, SCL, SDA) |
| Tactile push button | Momentary NO, any standard 6mm PCB type |
| 10 kΩ resistor | Pull-up for button (or use internal pull-up in firmware) |
| 0.1 µF capacitor | Button debounce (optional if using software debounce) |
| LiPo battery + regulator | 3.7V LiPo with 3.3V LDO, or USB power bank |

### Pin Assignments (default)

| Signal | GPIO | Notes |
|--------|------|-------|
| OLED SDA | 8 | I2C data |
| OLED SCL | 9 | I2C clock |
| Button | 5 | Active-low; internal pull-up enabled in firmware |

> **Note:** Confirm I2C address of your OLED module. Most 0.96" SSD1306 modules use `0x3C`. Some use `0x3D`. Run an I2C scanner sketch to verify before first flash.

### Wiring Diagram (text)

```
ESP32-C3 Dev Board
┌─────────────────────────────┐
│  3V3 ──────────────────────►│VCC   SSD1306 OLED
│  GND ──────────────────────►│GND
│  GPIO8 ────────────────────►│SDA
│  GPIO9 ────────────────────►│SCL
│                             └───────────────────
│
│  3V3 ───┬── 10kΩ ──┐
│         │          │
│  GPIO5 ─┘      [BUTTON] ── GND
└─────────────────────────────┘
```

For software debounce only (no RC filter), simply wire button between GPIO5 and GND. Internal pull-up is enabled via `gpio_set_pull_mode(ANT_BUTTON_PIN, GPIO_PULLUP_ONLY)`.

---

## Station Unit (ESP32-WROOM-32 Dev Module)

### Components

| Item | Notes |
|------|-------|
| ESP32-WROOM-32 dev board | Standard 38-pin or 30-pin variant |
| USB cable | For power and serial logging to PC |

No additional peripherals required. All output is via USB-serial.

### Antenna Options for Testing

The following antenna types are candidates for substitution during testing sessions. Document which antenna is installed at the start of each session.

| Type | Notes |
|------|-------|
| PCB trace (integral) | Default on WROOM and C3 modules |
| Quarter-wave monopole | ~31 mm wire at 2.4 GHz; attach to U.FL or SMA connector |
| External dipole | Via U.FL pigtail + SMA; requires board with U.FL connector |
| Rubber duck (2.4 GHz) | Standard 2 dBi whip; via SMA adapter |

---

## Changing Target Board

To use a different board variant, edit `board_config.h` in the appropriate firmware directory:

```c
// firmware/mobile/main/board_config.h
#define ANT_OLED_SDA_PIN    8    // adjust for your board
#define ANT_OLED_SCL_PIN    9
#define ANT_BUTTON_PIN      5
```

Then re-run `idf.py set-target <target>` and `idf.py build`.

Common alternate pin assignments:

| Board | SDA | SCL | Button |
|-------|-----|-----|--------|
| ESP32-C3 (default) | 8 | 9 | 5 |
| ESP32-WROOM-32 | 21 | 22 | 0 |
| ESP32-S3 | 8 | 9 | 0 |

---

## Power Considerations for Mobile

- At 20 dBm TX power, ESP32-C3 draws ~300–400 mA peak during transmit bursts
- A 1000 mAh LiPo provides roughly 2–3 hours of active testing
- The firmware does not currently implement sleep modes between bursts (future enhancement)
- Do not power the C3 from a USB power bank that auto-shuts off below a load threshold — add a small bleed resistor or keep the serial monitor open
