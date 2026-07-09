# ESP32AntTest shared component — protocol + config consumed by both firmware
# targets (station, mobile). See ADR-003.
#
# Wired into each target via the components/ant_shared -> ../../shared symlink
# (per ADR-003), so the component is registered with the name `ant_shared`.
idf_component_register(
    SRCS           "src/protocol.c"
    INCLUDE_DIRS   "include"
)
