/**
 * @file pin_validator.c
 * @brief Pin configuration validation for ESP32.
 *
 * Ensures no GPIO conflicts and all assignments are valid.
 * All pins are configurable via WebUI — this is the safety net.
 *
 * Task: F01
 */

#include "pin_validator.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "sb_pins";

/** GPIOs 6-11 are reserved for SPI flash on ESP32. */
static bool is_flash_pin(int8_t gpio)
{
    return gpio >= 6 && gpio <= 11;
}

/** GPIOs 34-39 are input-only on ESP32. */
static bool is_input_only(int8_t gpio)
{
    return gpio >= 34 && gpio <= 39;
}

bool sb_pin_is_valid_output(int8_t gpio)
{
    if (gpio < 0 || gpio > 39) {
        return false;
    }
    if (is_flash_pin(gpio)) {
        return false;
    }
    if (is_input_only(gpio)) {
        return false;
    }
    return true;
}

bool sb_pin_is_valid_input(int8_t gpio)
{
    if (gpio < 0 || gpio > 39) {
        return false;
    }
    if (is_flash_pin(gpio)) {
        return false;
    }
    return true;
}

/**
 * Check if a pin is already used. Returns true if conflict found.
 */
static bool check_and_mark(bool used[40], int8_t gpio, const char *label)
{
    if (gpio < 0) {
        return false;  /* Not assigned — no conflict */
    }
    if (gpio > 39) {
        ESP_LOGE(TAG, "%s: GPIO %d out of range (0-39)", label, gpio);
        return true;
    }
    if (is_flash_pin(gpio)) {
        ESP_LOGE(TAG, "%s: GPIO %d reserved for SPI flash", label, gpio);
        return true;
    }
    if (used[gpio]) {
        ESP_LOGE(TAG, "%s: GPIO %d already assigned to another function", label, gpio);
        return true;
    }
    used[gpio] = true;
    return false;
}

esp_err_t sb_pin_validate(const sb_pin_config_t *pins)
{
    if (pins == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool used[40];
    memset(used, 0, sizeof(used));
    bool conflict = false;

    /* UART servo bus */
    conflict |= check_and_mark(used, pins->servo_bus_tx, "servo_bus_tx");
    conflict |= check_and_mark(used, pins->servo_bus_rx, "servo_bus_rx");
    conflict |= check_and_mark(used, pins->servo_bus_dir, "servo_bus_dir");

    /* Check TX is output-capable */
    if (pins->servo_bus_tx >= 0 && !sb_pin_is_valid_output(pins->servo_bus_tx)) {
        ESP_LOGE(TAG, "servo_bus_tx GPIO %d cannot be used as output", pins->servo_bus_tx);
        conflict = true;
    }

    /* I2C */
    conflict |= check_and_mark(used, pins->i2c_sda, "i2c_sda");
    conflict |= check_and_mark(used, pins->i2c_scl, "i2c_scl");

    /* SPI */
    conflict |= check_and_mark(used, pins->spi_mosi, "spi_mosi");
    conflict |= check_and_mark(used, pins->spi_miso, "spi_miso");
    conflict |= check_and_mark(used, pins->spi_sclk, "spi_sclk");

    /* OLED display */
    conflict |= check_and_mark(used, pins->oled_sda, "oled_sda");
    conflict |= check_and_mark(used, pins->oled_scl, "oled_scl");

    /* WS2812b — must be output */
    conflict |= check_and_mark(used, pins->ws2812_data, "ws2812_data");
    if (pins->ws2812_data >= 0 && !sb_pin_is_valid_output(pins->ws2812_data)) {
        ESP_LOGE(TAG, "ws2812_data GPIO %d cannot be used as output", pins->ws2812_data);
        conflict = true;
    }

    /* 1-Wire (DS18B20) */
    conflict |= check_and_mark(used, pins->onewire, "onewire");

    /* Endstops (input) */
    for (uint8_t i = 0; i < pins->endstop_count && i < 8; i++) {
        char label[24];
        snprintf(label, sizeof(label), "endstop[%d]", i);
        conflict |= check_and_mark(used, pins->endstop_pins[i], label);
    }

    /* Buttons (input) */
    for (uint8_t i = 0; i < pins->button_count && i < 4; i++) {
        char label[24];
        snprintf(label, sizeof(label), "button[%d]", i);
        conflict |= check_and_mark(used, pins->button_pins[i], label);
    }

    /* PWM outputs */
    for (uint8_t i = 0; i < pins->pwm_count && i < 8; i++) {
        char label[24];
        snprintf(label, sizeof(label), "pwm[%d]", i);
        conflict |= check_and_mark(used, pins->pwm_pins[i], label);
        if (pins->pwm_pins[i] >= 0 && !sb_pin_is_valid_output(pins->pwm_pins[i])) {
            ESP_LOGE(TAG, "pwm[%d] GPIO %d cannot be used as output", i, pins->pwm_pins[i]);
            conflict = true;
        }
    }

    if (conflict) {
        ESP_LOGE(TAG, "Pin configuration has conflicts — fix via WebUI");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Pin configuration valid");
    return ESP_OK;
}
