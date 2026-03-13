/**
 * @file http_server.h
 * @brief HTTPS server setup and route registration.
 *
 * Uses ESP-IDF httpd with mbedTLS for TLS. All API routes are
 * registered here. WebSocket upgrade is handled separately.
 *
 * Task: F01 (structure), F03 (implementation)
 */

#pragma once

#include "app_config.h"
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the HTTPS server.
 *
 * @param config  Server configuration (port, TLS settings).
 * @return ESP_OK on success.
 */
esp_err_t sb_http_server_start(const sb_server_config_t *config);

/**
 * Stop the HTTPS server and free resources.
 */
void sb_http_server_stop(void);

/**
 * Get the server handle for registering additional URI handlers.
 *
 * @return httpd_handle_t or NULL if server not started.
 */
httpd_handle_t sb_http_server_get_handle(void);

#ifdef __cplusplus
}
#endif
