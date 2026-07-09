# Station main component
idf_component_register(
    SRCS           "main.c"
    INCLUDE_DIRS   "."
    REQUIRES       ant_shared
)
