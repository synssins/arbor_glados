/**
 * @file bldc_modbus.c
 * @brief RS485 Modbus RTU sub-driver — Phase B stub.
 *
 * This file provides stub implementations that return ESP_ERR_NOT_SUPPORTED.
 * Full Modbus RTU implementation will be added in Phase B.
 *
 * When implemented, this will handle:
 *   - Half-duplex UART with hardware DE pin (ESP32-C3 native RS485)
 *   - Modbus RTU frame builder/parser
 *   - BLD-510B register read/write ($8000H-$801BH)
 *   - Fault polling via $801BH
 *
 * Task: BLDC Phase B (stub for Phase A)
 */

#include "bldc_modbus.h"
#include "bldc_driver.h"

#include "esp_log.h"

static const char *TAG = "bldc-modbus";

esp_err_t bldc_modbus_init(const sb_bldc_config_t *config)
{
    (void)config;
    ESP_LOGW(TAG, "Modbus RTU sub-driver not yet implemented (Phase B)");
    return ESP_ERR_NOT_SUPPORTED;
}

void bldc_modbus_deinit(void)
{
    /* Nothing to clean up in stub */
}

esp_err_t bldc_modbus_set_speed(uint8_t motor_idx, uint16_t speed)
{
    (void)motor_idx;
    (void)speed;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bldc_modbus_set_direction(uint8_t motor_idx, sb_bldc_dir_t dir)
{
    (void)motor_idx;
    (void)dir;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bldc_modbus_set_enable(uint8_t motor_idx, bool enable)
{
    (void)motor_idx;
    (void)enable;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bldc_modbus_set_brake(uint8_t motor_idx, bool brake)
{
    (void)motor_idx;
    (void)brake;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bldc_modbus_read_rpm(uint8_t motor_idx, uint16_t *rpm)
{
    (void)motor_idx;
    if (rpm) *rpm = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bldc_modbus_read_faults(uint8_t motor_idx, uint8_t *faults)
{
    (void)motor_idx;
    if (faults) *faults = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bldc_modbus_emergency_stop(void)
{
    ESP_LOGW(TAG, "Modbus e-stop stub — no action taken");
    return ESP_ERR_NOT_SUPPORTED;
}
