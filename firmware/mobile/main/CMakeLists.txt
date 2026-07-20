# Mobile main component
idf_component_register(
    SRCS           "main.c" "ui.c" "button.c" "buffer.c" "mrf.c" "guide.c"
    INCLUDE_DIRS   "."
    REQUIRES       ant_shared oled_text driver json
                   nvs_flash esp_wifi esp_netif esp_event esp_timer lwip
)
