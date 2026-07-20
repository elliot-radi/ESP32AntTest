# Station main component
# Increment 2: serial + session + logger + RF (SoftAP/UDP + ESP-NOW beacons).
idf_component_register(
    SRCS           "main.c" "serial.c" "session.c" "logger.c" "rf.c"
    INCLUDE_DIRS   "."
    REQUIRES       ant_shared json littlefs esp_timer nvs_flash
                   esp_wifi esp_netif esp_event lwip
)
