/**
 * @file led_ws2812.h
 * @brief WS2812b LED driver for the Waveshare Servo Driver with ESP32.
 *
 * Uses the ESP-IDF 5.3 RMT TX API to drive WS2812b addressable LEDs.
 * Default: 10 LEDs on a configurable GPIO pin (config.pins.ws2812_data).
 *
 * Task: F-LED
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "plugin_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Get the WS2812b LED plugin instance.
 *
 * @return Pointer to the static plugin struct.
 */
sb_plugin_t *sb_led_ws2812_plugin(void);

/**
 * Initialize the WS2812b LED strip.
 *
 * @param gpio_pin  GPIO pin connected to WS2812b data line.
 * @param led_count Number of LEDs in the strip (max MAX_WS2812_LEDS).
 * @return ESP_OK on success.
 */
esp_err_t sb_led_init(int gpio_pin, int led_count);

/**
 * Set a single pixel's color (buffered, call sb_led_show() to apply).
 *
 * @param index LED index (0-based).
 * @param r     Red   (0-255).
 * @param g     Green (0-255).
 * @param b     Blue  (0-255).
 * @return ESP_OK on success.
 */
esp_err_t sb_led_set_pixel(int index, uint8_t r, uint8_t g, uint8_t b);

/**
 * Set all pixels to the same color (buffered).
 *
 * @param r Red   (0-255).
 * @param g Green (0-255).
 * @param b Blue  (0-255).
 * @return ESP_OK on success.
 */
esp_err_t sb_led_set_all(uint8_t r, uint8_t g, uint8_t b);

/**
 * Flush the pixel buffer to the LED strip via RMT.
 *
 * @return ESP_OK on success.
 */
esp_err_t sb_led_show(void);

/**
 * Turn all LEDs off immediately.
 *
 * @return ESP_OK on success.
 */
esp_err_t sb_led_off(void);

/**
 * Start identify mode: rainbow cycle on LED 0 (leftmost).
 * Other LEDs remain off.
 *
 * @return ESP_OK on success.
 */
esp_err_t sb_led_identify_start(void);

/**
 * Stop identify mode.
 *
 * @return ESP_OK on success.
 */
esp_err_t sb_led_identify_stop(void);

/**
 * Flash all LEDs white (255, 255, 255).
 *
 * @return ESP_OK on success.
 */
esp_err_t sb_led_flash_on(void);

/**
 * Turn all LEDs off after a flash.
 *
 * @return ESP_OK on success.
 */
esp_err_t sb_led_flash_off(void);

/**
 * Check if identify mode is currently running.
 *
 * @return true if identify task is active.
 */
bool sb_led_is_identifying(void);

/**
 * Get the number of LEDs configured.
 *
 * @return Number of LEDs, or 0 if not initialized.
 */
int sb_led_get_count(void);

#ifdef __cplusplus
}
#endif
