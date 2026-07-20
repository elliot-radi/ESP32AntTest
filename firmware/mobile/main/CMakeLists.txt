# Mobile main component
idf_component_register(
    SRCS           "main.c" "ui.c" "button.c" "buffer.c" "mrf.c"
    INCLUDE_DIRS   "."
    REQUIRES       ant_shared oled_text driver
                   nvs_flash esp_wifi esp_netif esp_event esp_timer lwip
)
