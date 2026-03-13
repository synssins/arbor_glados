/**
 * @file api_ota.h
 * @brief OTA firmware upload endpoint.
 *
 * Provides POST /api/v1/system/ota/upload for streaming firmware
 * binary uploads to the next OTA partition.
 *
 * Task: F15
 */

#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the OTA upload endpoint on the given HTTP server.
 *
 * @param server  HTTP server handle.
 */
void sb_api_ota_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
