/**
 * @file api_emergency.h
 * @brief Emergency stop endpoint — immediate all-servo torque disable.
 *
 * Parity with control server C13 endpoint.
 * This endpoint is EXEMPT from rate limiting.
 *
 * Task: F01 (structure), F14 (implementation)
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register emergency stop endpoint with the HTTP server.
 *
 * Endpoint:
 *   POST /api/v1/emergency-stop
 *
 * @param server  HTTP server handle.
 * @return ESP_OK on success.
 */
esp_err_t sb_api_emergency_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
