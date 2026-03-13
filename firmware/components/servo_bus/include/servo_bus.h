/**
 * @file servo_bus.h
 * @brief Feetech STS/SCS serial bus servo driver.
 *
 * Implements the sb_plugin_t interface for serial bus servos.
 * Uses half-duplex UART at 1Mbps per project plan.
 *
 * Pin assignments come from NVS config — no hardcoded GPIOs.
 *
 * Task: F06
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Servo state read from hardware. */
typedef struct {
    uint16_t position;    /**< Current position (0-4095) */
    int16_t  speed;       /**< Current speed */
    uint16_t load;        /**< Current load */
    uint8_t  temperature; /**< Temperature in degrees C */
    uint8_t  voltage;     /**< Voltage in 0.1V units */
    bool     torque_on;   /**< Torque enabled */
} sb_servo_state_t;

/**
 * Get the servo-bus plugin instance.
 *
 * Call this to register the plugin with the plugin manager.
 * The returned pointer is static — do not free.
 */
struct sb_plugin *sb_servo_bus_plugin(void);

#ifdef __cplusplus
}
#endif
