#pragma once
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* oled_text — reusable SSD1306 OLED text rendering for ESP-IDF v5.2+.
 *
 * Thin wrapper over the official esp_lcd SSD1306 panel driver: initializes
 * I2C + the panel, keeps a 128x64 1bpp framebuffer in page format, and
 * exposes a simple puts(row,col,str)/clear/refresh text API with a built-in
 * 5x7 font. Designed for reuse across ESP32 projects.
 *
 * Geometry: 128x64 display, 6x8 cell (5x7 glyph + 1px spacing) ->
 * 21 columns x 8 rows of text. Row/col are zero-indexed and silently
 * clipped if out of range.
 */

#define OLED_TEXT_WIDTH      128
#define OLED_TEXT_HEIGHT     64
#define OLED_TEXT_COLS       21    /* floor(128 / 6) */
#define OLED_TEXT_ROWS       8     /* floor(64 / 8) */

/* Initialize I2C master bus + SSD1306 panel + clear the display.
 *  sda_pin / scl_pin: GPIO numbers for the I2C bus
 *  i2c_addr: 7-bit address (typically 0x3C; some modules use 0x3D)
 *  scl_hz: I2C clock, 400000 is typical for SSD1306
 * Returns ESP_OK on success. */
esp_err_t oled_text_init(int sda_pin, int scl_pin, uint8_t i2c_addr, uint32_t scl_hz);

/* Clear the framebuffer (does NOT flush to the display — call refresh). */
void oled_text_clear(void);

/* Write a string at the given text grid position (row 0-7, col 0-20).
 * Writes to the framebuffer only — call oled_text_refresh() to push to the
 * display. Silently clips at the display edge. */
void oled_text_puts(int row, int col, const char *s);

/* Flush the framebuffer to the display. */
void oled_text_refresh(void);

#ifdef __cplusplus
}
#endif
