/**
 * @file ws_server.h
 * @brief WebSocket server — subscribe/publish protocol.
 *
 * Runs on top of the HTTP server with WebSocket upgrade.
 * Supports topic subscription with glob patterns and command dispatch.
 *
 * Task: F01 (structure), F10 (implementation)
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize WebSocket endpoint on the HTTP server.
 *
 * @param server  HTTP server handle.
 * @return ESP_OK on success.
 */
esp_err_t sb_ws_init(httpd_handle_t server);

/**
 * Broadcast an event to all subscribed WebSocket clients.
 *
 * @param topic  Event topic string (e.g. "servo.position_changed").
 * @param data   JSON payload string.
 * @return Number of clients the event was sent to.
 */
int sb_ws_broadcast(const char *topic, const char *data);

/**
 * Get the number of connected WebSocket clients.
 */
int sb_ws_client_count(void);

#ifdef __cplusplus
}
#endif
