/**
 * @file api_cors.h
 * @brief CORS support for cross-origin browser requests.
 *
 * Enables the Arbor control server's WebUI (on a different host)
 * to probe this node's API endpoints directly from the browser.
 */

#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the OPTIONS wildcard handler for CORS preflight.
 *
 * @param server  httpd server handle.
 * @return ESP_OK on success.
 */
esp_err_t sb_cors_register(httpd_handle_t server);

/**
 * Set CORS response headers on a request.
 * Call before any httpd_resp_send*() in a handler.
 *
 * @param req  HTTP request handle.
 */
void sb_cors_set_headers(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
