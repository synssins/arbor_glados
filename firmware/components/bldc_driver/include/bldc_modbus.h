/**
 * @file bldc_modbus.h
 * @brief RS485 Modbus RTU sub-driver for BLDC motor control.
 *
 * Communicates with BLD-510B (and compatible) drivers via Modbus RTU
 * over RS485 half-duplex UART. ESP32-C3 has hardware RS485 DE support.
 *
 * Key registers (BLD-510B):
 *   $8000H — Control: EN, FR, BK, NW bits + pole pairs
 *   $8003H — Accel/Decel: hi byte = accel, lo byte = decel (×0.1s)
 *   $8005H — Speed: RPM (closed-loop) or 0-255 (open-loop)
 *   $8018H — Actual RPM (read-only)
 *   $801BH — Faults (read-only)
 *
 * Implementation: Phase B (stub only in Phase A).
 *
 * Task: BLDC Phase B
 */

#pragma once

#include "bldc_driver.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize RS485 Modbus RTU sub-driver.
 *
 * @param config  BLDC module config with RS485 pin assignments.
 * @return ESP_OK on success.
 */
esp_err_t bldc_modbus_init(const sb_bldc_config_t *config);

/**
 * De-initialize Modbus sub-driver.
 */
void bldc_modbus_deinit(void);

/**
 * Set motor speed via Modbus register $8005H.
 *
 * @param motor_idx  Motor index.
 * @param speed      Speed value (0-255 open-loop, or RPM closed-loop).
 * @return ESP_OK on success.
 */
esp_err_t bldc_modbus_set_speed(uint8_t motor_idx, uint16_t speed);

/**
 * Set motor direction via Modbus control register $8000H FR bit.
 *
 * @param motor_idx  Motor index.
 * @param dir        Desired direction.
 * @return ESP_OK on success.
 */
esp_err_t bldc_modbus_set_direction(uint8_t motor_idx, sb_bldc_dir_t dir);

/**
 * Set motor enable state via Modbus control register $8000H EN bit.
 *
 * @param motor_idx  Motor index.
 * @param enable     true = motor enabled.
 * @return ESP_OK on success.
 */
esp_err_t bldc_modbus_set_enable(uint8_t motor_idx, bool enable);

/**
 * Engage brake via Modbus control register $8000H BK bit.
 *
 * @param motor_idx  Motor index.
 * @param brake      true = brake engaged.
 * @return ESP_OK on success.
 */
esp_err_t bldc_modbus_set_brake(uint8_t motor_idx, bool brake);

/**
 * Read actual RPM from Modbus register $8018H.
 *
 * @param motor_idx  Motor index.
 * @param[out] rpm   Measured RPM.
 * @return ESP_OK on success.
 */
esp_err_t bldc_modbus_read_rpm(uint8_t motor_idx, uint16_t *rpm);

/**
 * Read fault register from Modbus register $801BH.
 *
 * @param motor_idx  Motor index.
 * @param[out] faults  Fault bitmask.
 * @return ESP_OK on success.
 */
esp_err_t bldc_modbus_read_faults(uint8_t motor_idx, uint8_t *faults);

/**
 * Emergency stop ALL motors via Modbus — brake + disable on all addresses.
 *
 * @return ESP_OK on success.
 */
esp_err_t bldc_modbus_emergency_stop(void);

#ifdef __cplusplus
}
#endif
