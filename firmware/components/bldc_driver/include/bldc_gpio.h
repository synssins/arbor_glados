/**
 * @file bldc_gpio.h
 * @brief GPIO + LEDC PWM sub-driver for BLDC motor control.
 *
 * Handles direct digital control signals to external drivers like BLD-510B:
 *   - EN (Enable): digital output, polarity configurable
 *   - F/R (Forward/Reverse): digital output
 *   - BK (Brake): digital output
 *   - SV (Speed): LEDC PWM output (1-2 KHz)
 *   - PG (Pulse Generator): pulse input for RPM measurement
 *   - ALM (Alarm): digital input (active low from driver)
 *
 * Note: SV output is 3.3V — external level shifter to 5V required
 * for most BLDC drivers.
 *
 * Task: BLDC Phase A
 */

#pragma once

#include "bldc_driver.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize GPIO sub-driver for all configured motors.
 *
 * Sets up:
 *   - Digital outputs for EN, F/R, BK (all initially de-asserted/safe)
 *   - LEDC PWM channels for SV (duty = 0 = stopped)
 *   - GPIO inputs for ALM with interrupt on falling edge
 *   - PCNT or ISR for PG pulse counting (RPM measurement)
 *
 * @param config  BLDC module config with per-motor pin assignments.
 * @return ESP_OK on success.
 */
esp_err_t bldc_gpio_init(const sb_bldc_config_t *config);

/**
 * De-initialize GPIO sub-driver. Stops all motors first.
 */
void bldc_gpio_deinit(void);

/**
 * Set motor speed via PWM duty cycle.
 *
 * @param motor_idx  Motor index (0 to motor_count-1).
 * @param speed      Speed value 0-255 (mapped to PWM duty).
 * @return ESP_OK on success.
 */
esp_err_t bldc_gpio_set_speed(uint8_t motor_idx, uint8_t speed);

/**
 * Set motor direction via F/R pin.
 *
 * IMPORTANT: Caller must ensure motor is stopped before calling this.
 * The safety FSM enforces the direction-change interlock.
 *
 * @param motor_idx  Motor index.
 * @param dir        Desired direction.
 * @return ESP_OK on success.
 */
esp_err_t bldc_gpio_set_direction(uint8_t motor_idx, sb_bldc_dir_t dir);

/**
 * Set motor enable state via EN pin.
 *
 * Respects the en_active_high config for polarity.
 *
 * @param motor_idx  Motor index.
 * @param enable     true = motor enabled, false = disabled.
 * @return ESP_OK on success.
 */
esp_err_t bldc_gpio_set_enable(uint8_t motor_idx, bool enable);

/**
 * Engage or release brake via BK pin.
 *
 * @param motor_idx  Motor index.
 * @param brake      true = brake engaged, false = released.
 * @return ESP_OK on success.
 */
esp_err_t bldc_gpio_set_brake(uint8_t motor_idx, bool brake);

/**
 * Read the ALM (alarm) pin state.
 *
 * @param motor_idx  Motor index.
 * @return true if alarm is active (pin low = fault).
 */
bool bldc_gpio_read_alarm(uint8_t motor_idx);

/**
 * Get the current measured RPM from PG pulse counting.
 *
 * @param motor_idx  Motor index.
 * @return RPM value, or 0 if not available / motor stopped.
 */
uint16_t bldc_gpio_get_rpm(uint8_t motor_idx);

/**
 * Emergency stop ALL motors via GPIO — brake ON, enable OFF, speed 0.
 * Must complete in under 100ms.
 *
 * @return ESP_OK on success.
 */
esp_err_t bldc_gpio_emergency_stop(void);

#ifdef __cplusplus
}
#endif
