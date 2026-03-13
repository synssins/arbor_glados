/**
 * @file oled_display.h
 * @brief SSD1306 OLED 128x32 display driver for status info.
 *
 * Shows board ID, WiFi mode/SSID/password/IP on the
 * Waveshare Servo Driver board's built-in 0.91" OLED.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the SSD1306 OLED over I2C.
 *
 * @param sda_pin  GPIO for I2C SDA.
 * @param scl_pin  GPIO for I2C SCL.
 * @return ESP_OK on success.
 */
esp_err_t sb_oled_init(int sda_pin, int scl_pin);

/**
 * Clear the entire display.
 */
void sb_oled_clear(void);

/**
 * Write a string at a given row (0-3 for 128x32).
 *
 * @param row   Row number (0-3, each 8px tall).
 * @param text  Null-terminated string.
 */
void sb_oled_text(uint8_t row, const char *text);

/**
 * Push the framebuffer to the display.
 */
void sb_oled_flush(void);

/**
 * Update the display with current WiFi/board status.
 * Call periodically or after state changes.
 */
void sb_oled_update_status(void);

#ifdef __cplusplus
}
#endif
