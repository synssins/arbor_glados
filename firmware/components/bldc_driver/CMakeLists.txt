# bldc-driver component — Brushless DC motor driver plugin
# Supports BLD-510B (and similar) via GPIO+PWM or RS485 Modbus RTU.
# Task: BLDC Phase A

idf_component_register(
    SRCS "bldc_driver.c" "bldc_gpio.c" "bldc_safety.c" "bldc_modbus.c"
    INCLUDE_DIRS "include"
    REQUIRES driver json esp_timer sb_common
)
