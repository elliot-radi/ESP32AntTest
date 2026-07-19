#pragma once
/* ESP32AntTest — board_config.h for the hwtest (ESP32-C3-Zero / SuperMini)
 *
 * config.h supplies ESP32-WROOM-32 pin defaults behind #ifndef guards.
 * Including this header before config.h overrides them for the C3 board
 * actually wired up here. See docs/HARDWARE.md and SPEC §2.1 / §2.2.
 *
 * C3 Mobile pins (per HARDWARE.md):
 *   OLED SDA = GPIO 8, OLED SCL = GPIO 9, Button = GPIO 5
 */
#define ANT_OLED_SDA_PIN  8
#define ANT_OLED_SCL_PIN  9
#define ANT_BUTTON_PIN    5
