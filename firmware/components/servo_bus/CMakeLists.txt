# servo-bus component — Feetech STS protocol driver
# Task: F06

idf_component_register(
    SRCS "servo_bus.c"
    INCLUDE_DIRS "include"
    REQUIRES driver json esp_timer sb_common
)
