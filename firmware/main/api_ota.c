/**
 * @file api_ota.c
 * @brief OTA firmware upload endpoint — streams binary to next OTA partition.
 *
 * Supports two upload formats:
 *   1. Combined SBFW format (app + SPIFFS WebUI in one file)
 *   2. Plain app-only binary (legacy/fallback)
 *
 * The SBFW format has a 16-byte header:
 *   [4] magic "SBFW"
 *   [4] app_size (little-endian)
 *   [4] spiffs_size (little-endian)
 *   [4] reserved (0)
 *   [app_size bytes] app binary
 *   [spiffs_size bytes] SPIFFS image
 *
 * POST /api/v1/system/ota/upload
 *
 * Task: F15
 */

#include "api_ota.h"
#include "json_util.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include <sys/socket.h>
#include <string.h>

static const char *TAG = "sb_ota";

#define SBFW_MAGIC  0x57464253  /* "SBFW" in little-endian */

typedef struct {
    uint32_t magic;
    uint32_t app_size;
    uint32_t spiffs_size;
    uint32_t reserved;
} __attribute__((packed)) sbfw_header_t;

/* ── Restart callback (one-shot timer) ──────────────────────────── */

static void restart_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "Restarting after OTA update...");
    esp_restart();
}

static void schedule_restart(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = restart_timer_cb,
        .name     = "ota_restart",
    };
    esp_timer_handle_t timer = NULL;
    esp_err_t err = esp_timer_create(&timer_args, &timer);
    if (err == ESP_OK) {
        esp_timer_start_once(timer, 1000000); /* 1 second in microseconds */
    } else {
        ESP_LOGE(TAG, "Failed to create restart timer: %s", esp_err_to_name(err));
        /* Fall back to immediate restart */
        esp_restart();
    }
}

/* ── Helpers ────────────────────────────────────────────────────── */

/**
 * Receive exactly `len` bytes from the request, handling timeouts.
 * Returns total bytes received, or -1 on error.
 */
static int recv_exact(httpd_req_t *req, char *buf, int len)
{
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r > 0) {
            got += r;
        } else if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        } else {
            return -1;
        }
    }
    return got;
}

/**
 * Stream `byte_count` bytes from the request into an OTA handle.
 * Logs progress. Returns ESP_OK on success.
 */
static esp_err_t stream_to_ota(httpd_req_t *req, esp_ota_handle_t ota_handle,
                                int byte_count, const char *label)
{
    char buf[4096];
    int remaining = byte_count;
    int total = 0;

    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int received = httpd_req_recv(req, buf, to_read);

        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "%s: receive error at %d/%d", label, total, byte_count);
            return ESP_FAIL;
        }

        esp_err_t err = esp_ota_write(ota_handle, buf, (size_t)received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s: write failed at %d/%d: %s",
                     label, total, byte_count, esp_err_to_name(err));
            return err;
        }

        total     += received;
        remaining -= received;

        if ((total % (64 * 1024)) < received) {
            ESP_LOGI(TAG, "%s progress: %d / %d (%d%%)",
                     label, total, byte_count, (total * 100) / byte_count);
        }
    }
    return ESP_OK;
}

/**
 * Stream `byte_count` bytes from the request directly to a partition.
 * Erases the partition first, then writes sequentially.
 */
static esp_err_t stream_to_partition(httpd_req_t *req, const esp_partition_t *part,
                                      int byte_count, const char *label)
{
    ESP_LOGI(TAG, "Erasing %s partition '%s' (%"PRIu32" bytes)...",
             label, part->label, part->size);
    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erase failed: %s", esp_err_to_name(err));
        return err;
    }

    char buf[4096];
    int remaining = byte_count;
    int offset = 0;

    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int received = httpd_req_recv(req, buf, to_read);

        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "%s: receive error at %d/%d", label, offset, byte_count);
            return ESP_FAIL;
        }

        err = esp_partition_write(part, (size_t)offset, buf, (size_t)received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s: write failed at offset %d: %s",
                     label, offset, esp_err_to_name(err));
            return err;
        }

        offset    += received;
        remaining -= received;

        if ((offset % (64 * 1024)) < received) {
            ESP_LOGI(TAG, "%s progress: %d / %d (%d%%)",
                     label, offset, byte_count, (offset * 100) / byte_count);
        }
    }

    ESP_LOGI(TAG, "%s: wrote %d bytes to '%s'", label, offset, part->label);
    return ESP_OK;
}

/* ── POST /api/v1/system/ota/upload ─────────────────────────────── */

static esp_err_t handle_ota_upload(httpd_req_t *req)
{
    if (req->content_len <= 0) {
        return sb_json_error(req, "400 Bad Request", "No firmware data in request body");
    }

    ESP_LOGI(TAG, "OTA upload started, content_len=%d", req->content_len);

    /* Increase socket timeout for large uploads */
    int sock_fd = httpd_req_to_sockfd(req);
    if (sock_fd >= 0) {
        struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    /* Read first 16 bytes to detect format */
    char header_buf[16];
    if (recv_exact(req, header_buf, sizeof(header_buf)) != (int)sizeof(header_buf)) {
        return sb_json_error(req, "400 Bad Request", "Upload too short");
    }

    sbfw_header_t hdr;
    memcpy(&hdr, header_buf, sizeof(hdr));
    bool is_combined = (hdr.magic == SBFW_MAGIC);

    /* Locate the next OTA app partition */
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        return sb_json_error(req, "500 Internal Server Error",
                             "No OTA update partition available");
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err;
    bool spiffs_updated = false;

    if (is_combined) {
        /* ── SBFW combined format: app + SPIFFS ── */
        ESP_LOGI(TAG, "SBFW combined update: app=%"PRIu32" bytes, spiffs=%"PRIu32" bytes",
                 hdr.app_size, hdr.spiffs_size);

        if (hdr.app_size == 0) {
            return sb_json_error(req, "400 Bad Request", "SBFW header has zero app_size");
        }
        if (hdr.app_size > update_partition->size) {
            return sb_json_error(req, "400 Bad Request", "App too large for OTA partition");
        }

        /* Phase 1: Write app to OTA partition */
        ESP_LOGI(TAG, "Phase 1/2: Writing app to '%s'", update_partition->label);
        err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
            return sb_json_error(req, "500 Internal Server Error",
                                 "Failed to begin OTA update");
        }

        err = stream_to_ota(req, ota_handle, (int)hdr.app_size, "App");
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            return sb_json_error(req, "500 Internal Server Error",
                                 "Failed to write app firmware");
        }

        err = esp_ota_end(ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "App image validation failed: %s", esp_err_to_name(err));
            return sb_json_error(req, "422 Unprocessable Entity",
                                 "App firmware image validation failed");
        }

        /* Phase 2: Write SPIFFS if present */
        if (hdr.spiffs_size > 0) {
            const esp_partition_t *spiffs_part = esp_partition_find_first(
                ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
            if (!spiffs_part) {
                ESP_LOGW(TAG, "No SPIFFS partition found — skipping WebUI update");
            } else if (hdr.spiffs_size > spiffs_part->size) {
                ESP_LOGW(TAG, "SPIFFS image (%"PRIu32") exceeds partition (%"PRIu32") — skipping",
                         hdr.spiffs_size, spiffs_part->size);
            } else {
                ESP_LOGI(TAG, "Phase 2/2: Writing SPIFFS to '%s'", spiffs_part->label);
                err = stream_to_partition(req, spiffs_part, (int)hdr.spiffs_size, "SPIFFS");
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "SPIFFS write failed — app was updated, WebUI may be stale");
                } else {
                    spiffs_updated = true;
                }
            }
        }
    } else {
        /* ── Plain app-only binary (first 16 bytes are start of app image) ── */
        ESP_LOGI(TAG, "Plain app-only OTA update");

        err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
        if (err != ESP_OK) {
            return sb_json_error(req, "500 Internal Server Error",
                                 "Failed to begin OTA update");
        }

        /* Write the 16 bytes we already read */
        err = esp_ota_write(ota_handle, header_buf, sizeof(header_buf));
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            return sb_json_error(req, "500 Internal Server Error",
                                 "Failed to write firmware data");
        }

        /* Stream the rest */
        int remaining_app = req->content_len - (int)sizeof(header_buf);
        if (remaining_app > 0) {
            err = stream_to_ota(req, ota_handle, remaining_app, "App");
            if (err != ESP_OK) {
                esp_ota_abort(ota_handle);
                return sb_json_error(req, "500 Internal Server Error",
                                     "Failed to write firmware data");
            }
        }

        err = esp_ota_end(ota_handle);
        if (err != ESP_OK) {
            return sb_json_error(req, "422 Unprocessable Entity",
                                 "Firmware image validation failed");
        }
    }

    /* Set boot partition */
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        return sb_json_error(req, "500 Internal Server Error",
                             "Failed to set new boot partition");
    }

    ESP_LOGI(TAG, "OTA update successful (spiffs=%s) — scheduling restart",
             spiffs_updated ? "updated" : "unchanged");

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        schedule_restart();
        return sb_json_error(req, "500 Internal Server Error", "OOM");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "spiffs_updated", spiffs_updated);
    cJSON_AddStringToObject(resp, "detail",
        spiffs_updated ? "Firmware + WebUI updated, restarting..."
                       : "Firmware updated, restarting...");

    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);

    schedule_restart();
    return ret;
}

/* ── Registration ───────────────────────────────────────────────── */

void sb_api_ota_register(httpd_handle_t server)
{
    if (server == NULL) {
        ESP_LOGE(TAG, "Cannot register OTA endpoint: server is NULL");
        return;
    }

    const httpd_uri_t ota_upload = {
        .uri     = "/api/v1/system/ota/upload",
        .method  = HTTP_POST,
        .handler = handle_ota_upload,
    };

    esp_err_t err = httpd_register_uri_handler(server, &ota_upload);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register %s: %s", ota_upload.uri, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "OTA upload endpoint registered");
    }
}
