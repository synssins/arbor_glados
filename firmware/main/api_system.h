/**
 * @file api_system.h
 * @brief System API endpoints — info, health, config, restart.
 *
 * Parity with control server C10 endpoints.
 *
 * Task: F01 (structure), F11 (implementation)
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register system API endpoints with the HTTP server.
 *
 * Endpoints:
 *   GET  /api/v1/system/info
 *   GET  /api/v1/system/health
 *   GET  /api/v1/system/config
 *   PUT  /api/v1/system/config
 *   POST /api/v1/system/restart
 *
 * @param server  HTTP server handle.
 * @return ESP_OK on success.
 */
esp_err_t sb_api_system_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
