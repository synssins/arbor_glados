/**
 * @file api_cors.c
 * @brief CORS support — OPTIONS preflight + header helper.
 *
 * Allows the Arbor control server's WebUI (running on a different
 * origin) to make direct HTTP requests to this node for probing
 * and future real-time control.
 */

#include "api_cors.h"

#include "esp_log.h"

static const char *TAG = "sb_cors";

/* ── OPTIONS preflight handler ─────────────────────────────── */

static esp_err_t handle_options(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods",
                       "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers",
                       "Content-Type, Authorization");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ── Public API ────────────────────────────────────────────── */

esp_err_t sb_cors_register(httpd_handle_t server)
{
    const httpd_uri_t options_uri = {
        .uri      = "/*",
        .method   = HTTP_OPTIONS,
        .handler  = handle_options,
        .user_ctx = NULL,
    };

    esp_err_t ret = httpd_register_uri_handler(server, &options_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register OPTIONS handler: %s",
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "CORS preflight handler registered");
    }
    return ret;
}

void sb_cors_set_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}
