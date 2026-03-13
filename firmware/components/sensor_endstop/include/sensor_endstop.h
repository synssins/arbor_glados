/**
 * @file sensor_endstop.h
 * @brief GPIO interrupt-driven endstop sensor.
 *
 * Configurable GPIO pins, NC/NO type, debounce.
 *
 * Task: F09
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Get the sensor-endstop plugin instance.
 */
struct sb_plugin *sb_sensor_endstop_plugin(void);

#ifdef __cplusplus
}
#endif
