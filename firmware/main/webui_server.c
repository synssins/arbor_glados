/**
 * @file webui_server.c
 * @brief Serve WebUI static files from SPIFFS partition.
 *
 * Mounts the "spiffs" partition, registers a wildcard URI handler
 * that serves files with correct MIME types. Unknown paths get
 * index.html (SPA fallback).
 */

#include "webui_server.h"

#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"

static const char *TAG = "sb_webui";

/** Max path length on SPIFFS. */
#define MAX_PATH 128

/** Read buffer for streaming files. */
#define BUF_SIZE 1024

/**
 * Determine MIME type from file extension.
 */
static const char *get_mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".js") == 0)   return "application/javascript";
    if (strcmp(ext, ".css") == 0)  return "text/css";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".png") == 0)  return "image/png";
    if (strcmp(ext, ".ico") == 0)  return "image/x-icon";
    if (strcmp(ext, ".svg") == 0)  return "image/svg+xml";
    return "application/octet-stream";
}

/**
 * Send a file from SPIFFS with proper headers.
 * Returns true if file was found and sent.
 */
static bool send_file(httpd_req_t *req, const char *filepath)
{
    struct stat st;
    if (stat(filepath, &st) != 0) {
        return false;
    }

    FILE *f = fopen(filepath, "r");
    if (!f) {
        return false;
    }

    httpd_resp_set_type(req, get_mime_type(filepath));

    /* Cache static assets (hashed filenames) for 1 year. */
    if (strstr(filepath, "/assets/")) {
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    }

    char buf[BUF_SIZE];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, BUF_SIZE, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, read_bytes) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return true;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return true;
}

/**
 * Wildcard handler — serves static files or falls back to index.html.
 */
static esp_err_t webui_handler(httpd_req_t *req)
{
    char filepath[MAX_PATH];
    const char *uri = req->uri;

    /* Strip query string if present. */
    const char *query = strchr(uri, '?');
    size_t uri_len = query ? (size_t)(query - uri) : strlen(uri);

    /* Don't serve API routes — those are handled by other handlers. */
    if (uri_len >= 4 && strncmp(uri, "/api", 4) == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }
    if (uri_len >= 3 && strncmp(uri, "/ws", 3) == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }

    /* Build SPIFFS path. */
    if (uri_len == 1 && uri[0] == '/') {
        snprintf(filepath, sizeof(filepath), "/spiffs/index.html");
    } else {
        snprintf(filepath, sizeof(filepath), "/spiffs%.*s", (int)uri_len, uri);
    }

    /* Try exact file first. */
    if (send_file(req, filepath)) {
        return ESP_OK;
    }

    /* SPA fallback — serve index.html for any unknown path. */
    if (send_file(req, "/spiffs/index.html")) {
        return ESP_OK;
    }

    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "WebUI not found — SPIFFS may be empty");
    return ESP_OK;
}

esp_err_t sb_webui_server_init(httpd_handle_t server)
{
    if (!server) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Mount SPIFFS (may already be mounted by early boot for config backup). */
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = false,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&spiffs_conf);
    if (ret == ESP_ERR_INVALID_STATE) {
        /* Already mounted by early boot — this is fine */
        ESP_LOGI(TAG, "SPIFFS already mounted");
    } else if (ret != ESP_OK) {
        if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "SPIFFS partition not found — WebUI disabled");
        } else {
            ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        }
        return ret;
    } else {
        size_t total = 0, used = 0;
        esp_spiffs_info("spiffs", &total, &used);
        ESP_LOGI(TAG, "SPIFFS mounted: total=%zu, used=%zu", total, used);
    }

    /* Register wildcard handler with lowest priority (runs after API handlers). */
    httpd_uri_t webui_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = webui_handler,
        .user_ctx = NULL,
    };

    ret = httpd_register_uri_handler(server, &webui_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WebUI handler: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WebUI server registered (SPIFFS)");
    return ESP_OK;
}
