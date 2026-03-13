idf_component_register(
    SRCS "led_ws2812.c"
    INCLUDE_DIRS "include"
    REQUIRES driver sb_common
    PRIV_REQUIRES freertos esp_timer
)
