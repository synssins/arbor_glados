/**
 * @file http_server.c
 * @brief HTTP server for Arbor.
 *
 * Phase 1: plain HTTP only. TLS support (esp_https_server) will be
 * added in Phase 2 once certificate provisioning is implemented.
 *
 * Task: F03
 */

#include "http_server.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "sb_http";

static httpd_handle_t s_server = NULL;

esp_err_t sb_http_server_start(const sb_server_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_server != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    if (config->tls_enabled) {
        ESP_LOGW(TAG, "TLS requested but not available in Phase 1 — falling back to HTTP");
    }

    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
    httpd_config.server_port = config->port;
    httpd_config.max_uri_handlers = 48;
    httpd_config.max_resp_headers = 8;
    httpd_config.stack_size = 8192;
    httpd_config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t ret = httpd_start(&s_server, &httpd_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", config->port);
    return ESP_OK;
}

void sb_http_server_stop(void)
{
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

httpd_handle_t sb_http_server_get_handle(void)
{
    return s_server;
}
