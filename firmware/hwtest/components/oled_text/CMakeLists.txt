# oled_text — reusable SSD1306 OLED text rendering on ESP-IDF v5.2+
#
# Wraps the official esp_lcd SSD1306 panel driver with a simple framebuffer +
# 5x7 font text API. Designed for reuse across projects: init with I2C pins +
# address, then puts(row,col,str)/clear/refresh. No external deps beyond
# ESP-IDF's esp_lcd + driver (i2c) components.
idf_component_register(
    SRCS           "src/oled_text.c"
    INCLUDE_DIRS   "include"
    REQUIRES       esp_lcd driver
)
