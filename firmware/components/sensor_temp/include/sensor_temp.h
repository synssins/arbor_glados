/**
 * @file sensor_temp.h
 * @brief DS18B20 temperature sensor driver.
 *
 * Uses 1-Wire protocol on configurable GPIO pin.
 *
 * Task: F08
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Get the sensor-temp plugin instance.
 */
struct sb_plugin *sb_sensor_temp_plugin(void);

#ifdef __cplusplus
}
#endif
