/**
 * @file json_util.h
 * @brief JSON utility helpers for API responses.
 *
 * Wraps cJSON with convenience functions for building standard
 * API responses matching the project plan format.
 *
 * Task: F01
 */

#pragma once

#include "cJSON.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Send a JSON response.
 *
 * @param req         HTTP request to respond to.
 * @param status      HTTP status code string (e.g. "200 OK").
 * @param json        cJSON object to serialize (will NOT be freed).
 * @return ESP_OK on success.
 */
esp_err_t sb_json_respond(httpd_req_t *req, const char *status, const cJSON *json);

/**
 * Send a JSON error response.
 *
 * @param req     HTTP request.
 * @param status  HTTP status code string.
 * @param detail  Error detail message.
 * @return ESP_OK on success.
 */
esp_err_t sb_json_error(httpd_req_t *req, const char *status, const char *detail);

/**
 * Parse the request body as JSON.
 *
 * Caller must free the returned cJSON object with cJSON_Delete.
 *
 * @param req  HTTP request.
 * @return Parsed cJSON object, or NULL on parse error (sends 422 response).
 */
cJSON *sb_json_parse_body(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
