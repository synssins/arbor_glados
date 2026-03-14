/**
 * @file api_pwm_servo.h
 * @brief PWM Servo API endpoints — list, state, position.
 *
 * Dispatches to the servo-pwm plugin via plugin_manager.
 *
 * Task: PWM Servo Support
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register PWM servo API endpoints with the HTTP server.
 *
 * Endpoints:
 *   GET  /api/v1/pwm-servo/list              List all active PWM channels
 *   GET  /api/v1/pwm-servo/{channel}/state    Single channel state
 *   PUT  /api/v1/pwm-servo/{channel}/position Set position (0-1000)
 *
 * @param server  HTTP server handle.
 * @return ESP_OK on success.
 */
esp_err_t sb_api_pwm_servo_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
