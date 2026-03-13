/**
 * @file webui_server.h
 * @brief Serve WebUI static files from SPIFFS.
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize SPIFFS and register URI handlers for serving the WebUI.
 *
 * @param server  HTTP server handle to register routes on.
 * @return ESP_OK on success.
 */
esp_err_t sb_webui_server_init(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
