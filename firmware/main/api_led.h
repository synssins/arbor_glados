/**
 * @file api_led.h
 * @brief LED API endpoints — state, identify, flash, color, off.
 *
 * Controls WS2812B LEDs on the Waveshare Servo Driver board.
 *
 * Task: F11
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register LED API endpoints with the HTTP server.
 *
 * Endpoints:
 *   GET  /api/v1/led
 *   POST /api/v1/led/identify
 *   POST /api/v1/led/flash
 *   PUT  /api/v1/led/color
 *   POST /api/v1/led/off
 *
 * @param server  HTTP server handle.
 * @return ESP_OK on success.
 */
esp_err_t sb_api_led_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
