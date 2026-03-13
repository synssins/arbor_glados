/**
 * @file api_servo.h
 * @brief Servo API endpoints — state, position, speed, torque, sync, scan.
 *
 * Parity with control server C11 endpoints.
 *
 * Task: F01 (structure), F12 (implementation)
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register servo API endpoints with the HTTP server.
 *
 * Endpoints:
 *   GET  /api/v1/servo/{id}/state
 *   PUT  /api/v1/servo/{id}/position
 *   PUT  /api/v1/servo/{id}/speed
 *   PUT  /api/v1/servo/{id}/torque
 *   POST /api/v1/servo/sync
 *   GET  /api/v1/servo/scan
 *
 * @param server  HTTP server handle.
 * @return ESP_OK on success.
 */
esp_err_t sb_api_servo_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
