/**
 * @file servo_pwm.h
 * @brief Standard PWM servo driver.
 *
 * Uses ESP32 LEDC peripheral for PWM generation.
 * Pin assignments from NVS config — no hardcoded GPIOs.
 *
 * Task: F07
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Get the servo-pwm plugin instance.
 */
struct sb_plugin *sb_servo_pwm_plugin(void);

#ifdef __cplusplus
}
#endif
