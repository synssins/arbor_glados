/**
 * @file pin_validator.h
 * @brief Pin configuration validation.
 *
 * Validates pin assignments to prevent conflicts (same pin assigned
 * to multiple functions) and invalid GPIO numbers for ESP32.
 *
 * All pins are configurable via WebUI — this module ensures safety.
 *
 * Task: F01
 */

#pragma once

#include "app_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Validate a pin configuration for conflicts and invalid GPIO numbers.
 *
 * Checks:
 * - No duplicate pin assignments (same GPIO used twice)
 * - All assigned pins are valid ESP32 GPIOs (0-39)
 * - Input-only pins (34-39) are not used for outputs
 * - Reserved pins (6-11 for flash) are not assigned
 *
 * @param[in] pins  Pin configuration to validate.
 * @return ESP_OK if valid, ESP_ERR_INVALID_ARG with log message if not.
 */
esp_err_t sb_pin_validate(const sb_pin_config_t *pins);

/**
 * Check if a specific GPIO is valid for output on ESP32.
 *
 * @param gpio  GPIO number.
 * @return true if the pin can be used as output.
 */
bool sb_pin_is_valid_output(int8_t gpio);

/**
 * Check if a specific GPIO is valid for input on ESP32.
 *
 * @param gpio  GPIO number.
 * @return true if the pin can be used as input.
 */
bool sb_pin_is_valid_input(int8_t gpio);

#ifdef __cplusplus
}
#endif
