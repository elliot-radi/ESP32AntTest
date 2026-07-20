# Station main component
# Increment 1: serial protocol + session + logger (LittleFS). No RF yet.
idf_component_register(
    SRCS           "main.c" "serial.c" "session.c" "logger.c"
    INCLUDE_DIRS   "."
    REQUIRES       ant_shared json littlefs esp_timer
)
