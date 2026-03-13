# sensor-endstop component — GPIO interrupt endstop driver
# Task: F09

idf_component_register(
    SRCS "sensor_endstop.c"
    INCLUDE_DIRS "include"
    REQUIRES driver json esp_timer sb_common
)
