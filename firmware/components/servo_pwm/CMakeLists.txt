# servo-pwm component — standard PWM servo driver
# Task: F07

idf_component_register(
    SRCS "servo_pwm.c"
    INCLUDE_DIRS "include"
    REQUIRES driver json sb_common
)
