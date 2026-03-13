/**
 * @file api_sensor.h
 * @brief Sensor API endpoints — reading, history, all sensors.
 *
 * Parity with control server C12 endpoints.
 *
 * Task: F01 (structure), F13 (implementation)
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register sensor API endpoints with the HTTP server.
 *
 * Endpoints:
 *   GET  /api/v1/sensor/{id}/reading
 *   GET  /api/v1/sensor/{id}/history
 *   GET  /api/v1/sensors
 *
 * @param server  HTTP server handle.
 * @return ESP_OK on success.
 */
esp_err_t sb_api_sensor_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
