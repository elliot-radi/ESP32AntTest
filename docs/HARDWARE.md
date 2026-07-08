# ESP32AntTest — Hardware Guide

ESP32AntTest uses two ESP32 development boards. The **Mobile** role (OLED + button, battery) and **Station** role (USB-serial logging, LittleFS) are decoupled from the physical board — either the ESP32-C3 or the ESP32-WROOM-32 can fill either role. The assignment is a compile-time choice; see [Configurations](#configurations) below.

---

## Boards

### ESP32-C3 Dev Module

| Item | Notes |
|------|-------|
| Chip | ESP32-C3, RISC-V single-core, 2.4 GHz |
| Antenna | Ceramic PCB antenna (onboard) |
| USB | USB-C for power and serial |

### ESP32-WROOM-32 Dev Module

| Item | Notes |
|------|-------|
| Chip | ESP32-WROOM-32, dual-core Xtensa, 2.4 GHz |
| Antenna | FPC antenna (adhered to PCB) |
| USB | Micro-USB for power and serial (38-pin or 30-pin variant) |

---

## Mobile Peripherals

The Mobile board carries the user interface regardless of which dev module it is.

| Peripheral | Type | Interface |
|-----------|------|-----------|
| Display | 0.96" SSD1306 OLED, 128×64 px | I2C |
| Input | Single tactile push-button | GPIO, active-low, internal pull-up |
| Power | USB (power bank or USB charger/cable) | — |

### Pin Assignments by Board

Pin assignments are keyed to the **board**, not the role, so they remain valid in either configuration. The authoritative source is each firmware directory's `board_config.h`; the values in `shared/config.h` are fallback defaults.

| Signal | ESP32-C3 | ESP32-WROOM-32 | Notes |
|--------|----------|----------------|-------|
| OLED SDA | GPIO 8 | GPIO 21 | I2C data |
| OLED SCL | GPIO 9 | GPIO 22 | I2C clock |
| Button | GPIO 5 | GPIO 17 | Active-low; internal pull-up. (GPIO 0 avoided on WROOM — strapping pin.) |

> **Note:** The OLED modules on hand use I2C address **`0x3C`** (confirmed, set as the default in `shared/config.h` as `ANT_OLED_I2C_ADDR`). Some SSD1306 modules use `0x3D` instead — if you swap in a different module, run an I2C scanner sketch to verify and override `ANT_OLED_I2C_ADDR` in `board_config.h`.

 ### Wiring diagram - ESP32-WROOM-32 as Mobile                                           
                                                                 
 ```                                                                    
 ┌─────────────────────────────┐         ┌─────────────────────┐ 
 │   ESP32-WROOM-32            │         │   SSD1306 OLED      │
 │                             │         │                     │
 │                       3V3   ●─────────● VCC                 │ 
 │                       GND   ●─────────● GND                 │ 
 │                     GPIO21  ●─────────● SDA                 │ 
 │                     GPIO22  ●─────────● SCL                 │ 
 │                             │         └─────────────────────┘
 │                     GPIO17  ●───┐                             
 └─────────────────────────────┘   │                             
                                   │   3V3                       
                                   │    │                        
                                   │   10kΩ (optional)           
                                   └────┤                        
                                    ┌───┴────┐                   
                                    │ BUTTON │                   
                                    │  (NO)  │                   
                                    └───┬────┘                   
                                        │                        
                                       GND                       
 ```                                                                    
                                                                 
 ### Wiring diagram - ESP32-C3 as Mobile                                                 
                                                                 
 ```                                                                    
 ┌─────────────────────────────┐         ┌─────────────────────┐ 
 │   ESP32-C3                  │         │   SSD1306 OLED      │ 
 │                             │         │                     │
 │                       3V3   ●─────────● VCC                 │ 
 │                       GND   ●─────────● GND                 │ 
 │                     GPIO8   ●─────────● SDA                 │ 
 │                     GPIO9   ●─────────● SCL                 │ 
 │                             │         └─────────────────────┘
 │                     GPIO5   ●───┐                             
 └─────────────────────────────┘   │                             
                                   │   3V3                       
                                   │    │                        
                                   │   10kΩ (optional)           
                                   └────┤                        
                                    ┌───┴────┐                   
                                    │ BUTTON │                   
                                    │  (NO)  │                   
                                    └───┬────┘                   
                                        │                        
                                       GND                       
 ```


For software debounce only (no RC filter), simply wire the button between the chosen GPIO and GND. Internal pull-up is enabled via `gpio_set_pull_mode(ANT_BUTTON_PIN, GPIO_PULLUP_ONLY)`.

---

## Station Peripherals

The Station board requires no external peripherals — only a USB cable for power and serial logging. LittleFS session logs are stored in onboard flash.

---

## Configurations

Two role-to-board assignments are supported. The choice is made at compile time via the `ROLE_MOBILE` / `ROLE_STATION` flag in `shared/config.h` (see [SPEC §2.1](SPEC.md)) and the `idf.py set-target` called in each firmware directory.

| Config | Mobile board | Station board | Mobile pins (SDA / SCL / Button) |
|--------|-------------|---------------|----------------------------------|
| **A** (current default) | ESP32-WROOM-32 | ESP32-C3 | 21 / 22 / 17 |
| **B** | ESP32-C3 | ESP32-WROOM-32 | 8 / 9 / 5 |

Both configurations are functionally equivalent — the RF behavior (Station as SoftAP, Mobile as STA in Mode A; symmetric ESP-NOW peers in Mode B) is role-based, not chip-based.

To switch the default from A to B (or vice versa), only the labels above and the `set-target` calls change; the pin maps stay board-keyed.

---

## Antenna Options for Testing

The following antenna types are candidates for substitution during testing sessions. Document which antenna is installed at the start of each session.

| Type | Notes |
|------|-------|
| Ceramic PCB antenna | Default on the ESP32-C3 dev module (onboard chip antenna) |
| FPC antenna | Default on the ESP32-WROOM-32 dev module (adhered to the PCB) |
| Quarter-wave monopole | ~31 mm wire at 2.4 GHz; attach to U.FL or SMA connector |
| External dipole | Via U.FL pigtail + SMA; requires board with U.FL connector |
| Rubber duck (2.4 GHz) | Standard 2 dBi whip; via SMA adapter |

---

## Changing Target Board / Configuration

To switch configurations or substitute a different board variant, edit `board_config.h` in the appropriate firmware directory:

```c
// firmware/mobile/main/board_config.h
#define ANT_OLED_SDA_PIN    21   // WROOM default; use 8 for C3
#define ANT_OLED_SCL_PIN    22   // WROOM default; use 9 for C3
#define ANT_BUTTON_PIN      17   // WROOM default; use 5 for C3
```

Then re-run `idf.py set-target <target>` and `idf.py build`. Use `esp32` for the WROOM-32, `esp32c3` for the C3.

---

## Power Considerations for Mobile

The Mobile is USB-powered (no battery circuit — see [SPEC §9 OI-04](SPEC.md)). Approximate current draw at 20 dBm TX power:

| Board | Peak TX current |
|-------|----------------|
| ESP32-C3 | ~300–400 mA |
| ESP32-WROOM-32 | ~500 mA+ |

- The firmware does not currently implement sleep modes between bursts (future enhancement)
- Do not power the Mobile from a USB power bank that auto-shuts off below a load threshold — add a small bleed resistor or keep the serial monitor open
