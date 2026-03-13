/**
 * @file bldc_safety.h
 * @brief Direction interlock FSM and emergency stop for BLDC motors.
 *
 * Safety rules:
 *   1. Direction change requires: decelerate → stop → wait → verify → change
 *   2. Emergency stop: brake + disable ALL motors in <100ms
 *   3. Alarm monitoring: poll ALM pin / fault register, auto-disable on fault
 *   4. Watchdog: BLDC task feeds task watchdog; hang → reset → motors off
 *
 * The FSM ensures direction changes never happen while the motor is spinning,
 * which would damage the BLD-510B driver or the motor.
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
 * Initialize the safety FSM for all motors.
 *
 * Sets all motors to STOPPED state. Must be called before any motor
 * commands are accepted.
 *
 * @param config  BLDC module config (for timing parameters).
 * @return ESP_OK on success.
 */
esp_err_t bldc_safety_init(const sb_bldc_config_t *config);

/**
 * Request a speed change. The FSM validates the request.
 *
 * @param motor_idx  Motor index.
 * @param speed      Desired speed (0-255). 0 = stop.
 * @return ESP_OK if accepted, ESP_ERR_INVALID_STATE if motor is in
 *         ESTOP/FAULT/REVERSING state.
 */
esp_err_t bldc_safety_request_speed(uint8_t motor_idx, uint8_t speed);

/**
 * Request a direction change. Triggers the interlock sequence:
 *   RUNNING → set speed 0 → BRAKING → wait stop → REVERSING
 *   → wait direction_change_stop_ms → verify stopped → set new direction
 *   → STOPPED (caller can then set speed to resume)
 *
 * This function starts the sequence; it may be asynchronous.
 * Check the motor state to know when the reversal is complete.
 *
 * @param motor_idx  Motor index.
 * @param dir        Desired direction.
 * @return ESP_OK if interlock sequence started,
 *         ESP_ERR_INVALID_STATE if already reversing or in fault.
 */
esp_err_t bldc_safety_request_direction(uint8_t motor_idx, sb_bldc_dir_t dir);

/**
 * Request enable/disable. Validates against current FSM state.
 *
 * @param motor_idx  Motor index.
 * @param enable     true = enable.
 * @return ESP_OK if accepted.
 */
esp_err_t bldc_safety_request_enable(uint8_t motor_idx, bool enable);

/**
 * Request immediate brake.
 *
 * @param motor_idx  Motor index.
 * @return ESP_OK if accepted.
 */
esp_err_t bldc_safety_request_brake(uint8_t motor_idx);

/**
 * Emergency stop ALL motors. Transitions all to ESTOP state.
 * Must complete in <100ms.
 *
 * @return ESP_OK on success.
 */
esp_err_t bldc_safety_emergency_stop(void);

/**
 * Clear fault/estop state for a motor, returning it to STOPPED.
 *
 * @param motor_idx  Motor index.
 * @return ESP_OK if cleared, ESP_ERR_INVALID_STATE if fault still present.
 */
esp_err_t bldc_safety_clear_fault(uint8_t motor_idx);

/**
 * Report a fault detected by hardware polling (ALM pin or Modbus fault register).
 * Transitions the motor to FAULT state and disables it.
 *
 * @param motor_idx  Motor index.
 * @param faults     Fault bitmask (sb_bldc_fault_t values).
 */
void bldc_safety_report_fault(uint8_t motor_idx, uint8_t faults);

/**
 * Get the current FSM state for a motor.
 *
 * @param motor_idx  Motor index.
 * @return Current state.
 */
sb_bldc_state_t bldc_safety_get_state(uint8_t motor_idx);

/**
 * Get the full runtime state for a motor.
 *
 * @param motor_idx       Motor index.
 * @param[out] state_out  Filled with current state.
 * @return ESP_OK on success.
 */
esp_err_t bldc_safety_get_motor_state(uint8_t motor_idx,
                                       sb_bldc_motor_state_t *state_out);

/**
 * Periodic tick — call from the BLDC task every alarm_poll_ms.
 * Checks ALM pins / Modbus fault registers and advances the FSM
 * for any motors in BRAKING or REVERSING state.
 */
void bldc_safety_tick(void);

#ifdef __cplusplus
}
#endif
