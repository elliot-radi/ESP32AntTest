#include "oled_text.h"
#include "font_5x7.h"
#include <string.h>
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "oled_text";

/* 128x64 1bpp framebuffer in SSD1306 page format: 8 pages of 128 bytes,
 * each byte is 8 vertical pixels (bit 0 = top row of the page). */
static uint8_t fb[OLED_TEXT_WIDTH * (OLED_TEXT_HEIGHT / 8)];

static esp_lcd_panel_handle_t panel = NULL;

/* Set pixel (x,y) in the page-format framebuffer. */
static void set_px(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_TEXT_WIDTH || y < 0 || y >= OLED_TEXT_HEIGHT)
        return;
    int page = y / 8;
    int bit  = y % 8;
    if (on)
        fb[page * OLED_TEXT_WIDTH + x] |=  (1u << bit);
    else
        fb[page * OLED_TEXT_WIDTH + x] &= ~(1u << bit);
}

esp_err_t oled_text_init(int sda_pin, int scl_pin, uint8_t i2c_addr, uint32_t scl_hz)
{
    /* 1. I2C master bus */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,                 /* auto-select port */
        .sda_io_num = (gpio_num_t)sda_pin,
        .scl_io_num = (gpio_num_t)scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err)); return err; }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = i2c_addr,
        .scl_speed_hz = scl_hz,
        .scl_wait_us = 0,
        .flags.disable_ack_check = 0,
    };
    i2c_master_dev_handle_t i2c_dev;
    err = i2c_master_bus_add_device(bus, &dev_cfg, &i2c_dev);
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2c_master_bus_add_device: %s", esp_err_to_name(err)); return err; }

    /* 2. LCD panel IO (I2C transport for the SSD1306 control/data stream) */
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = i2c_addr,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .control_phase_bytes = 1,    /* SSD1306 uses a 1-byte control phase (Co+D/C) */
        .dc_bit_offset = 6,          /* D/C bit is bit 6 of the control byte */
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags.dc_low_on_data = 0,
        .flags.disable_control_phase = 0,
        .scl_speed_hz = scl_hz,
    };
    esp_lcd_panel_io_handle_t io;
    err = esp_lcd_new_panel_io_i2c_v2(bus, &io_cfg, &io);
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_lcd_new_panel_io_i2c: %s", esp_err_to_name(err)); return err; }

    /* 3. SSD1306 panel */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,       /* no reset pin wired */
        .bits_per_pixel = 1,
        .vendor_config = NULL,
    };
    err = esp_lcd_new_panel_ssd1306(io, &panel_cfg, &panel);
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_lcd_new_panel_ssd1306: %s", esp_err_to_name(err)); return err; }

    err = esp_lcd_panel_reset(panel);
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_init(panel);
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_disp_on_off(panel, true);
    if (err != ESP_OK) return err;

    oled_text_clear();
    oled_text_refresh();
    ESP_LOGI(TAG, "SSD1306 ready (addr 0x%02X, %dx%d)", i2c_addr, OLED_TEXT_WIDTH, OLED_TEXT_HEIGHT);
    return ESP_OK;
}

void oled_text_clear(void)
{
    memset(fb, 0, sizeof(fb));
}

void oled_text_puts(int row, int col, const char *s)
{
    if (row < 0 || row >= OLED_TEXT_ROWS)
        return;
    int x0 = col * 6;
    for (const char *p = s; *p && x0 < OLED_TEXT_WIDTH; p++, x0 += 6) {
        uint8_t ch = (uint8_t)*p;
        int idx = ch - FONT5X7_FIRST;
        const uint8_t *glyph = (idx >= 0 && idx < FONT5X7_COUNT)
                              ? font5x7[idx] : font5x7[0];   /* unknown -> space-ish */
        int base_y = row * 8;
        for (int cx = 0; cx < 5; cx++) {
            uint8_t col_bits = glyph[cx];
            for (int cy = 0; cy < 7; cy++)
                set_px(x0 + cx, base_y + cy, (col_bits >> cy) & 1);
        }
        /* 6th column blank (inter-char spacing) */
        for (int cy = 0; cy < 8; cy++)
            set_px(x0 + 5, base_y + cy, false);
    }
}

void oled_text_refresh(void)
{
    if (!panel) return;
    esp_lcd_panel_draw_bitmap(panel, 0, 0, OLED_TEXT_WIDTH, OLED_TEXT_HEIGHT, fb);
}
