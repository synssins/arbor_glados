# sensor-temp component — DS18B20 temperature sensor
# Task: F08

idf_component_register(
    SRCS "sensor_temp.c"
    INCLUDE_DIRS "include"
    REQUIRES driver json esp_timer sb_common
)
