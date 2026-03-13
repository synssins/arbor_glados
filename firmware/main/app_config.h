/**
 * @file app_config.h
 * @brief NVS-backed configuration system.
 *
 * All configuration is stored in NVS (non-volatile storage). No defaults
 * are compiled into the firmware — values must be provisioned via the
 * control server's config push endpoint.
 *
 * Pin assignments are fully configurable — no hardcoded GPIO numbers.
 *
 * Task: F01 (structure), F02 (implementation)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum length for string config values. */
#define SB_CONFIG_STR_MAX 128

/** Maximum number of servos per bus. */
#define SB_MAX_SERVOS 32

/** Maximum number of sensor instances. */
#define SB_MAX_SENSORS 16

/**
 * Pin assignment configuration.
 *
 * ALL pins are configurable — no hardcoded GPIO numbers anywhere.
 * Pin assignments are stored in NVS and can be changed via WebUI.
 */
typedef struct {
    int8_t servo_bus_tx;     /**< UART TX for servo bus (-1 = not assigned) */
    int8_t servo_bus_rx;     /**< UART RX for servo bus (-1 = not assigned) */
    int8_t servo_bus_dir;    /**< Direction pin for half-duplex (-1 = not used) */
    int8_t i2c_sda;          /**< I2C SDA (-1 = not assigned) */
    int8_t i2c_scl;          /**< I2C SCL (-1 = not assigned) */
    int8_t spi_mosi;         /**< SPI MOSI (-1 = not assigned) */
    int8_t spi_miso;         /**< SPI MISO (-1 = not assigned) */
    int8_t spi_sclk;         /**< SPI SCLK (-1 = not assigned) */
    int8_t oled_sda;         /**< OLED display SDA (-1 = not assigned) */
    int8_t oled_scl;         /**< OLED display SCL (-1 = not assigned) */
    int8_t ws2812_data;      /**< WS2812b data pin (-1 = not assigned) */
    int8_t onewire;          /**< 1-Wire bus for DS18B20 (-1 = not assigned) */
    int8_t endstop_pins[8];  /**< Endstop GPIO pins (-1 = not assigned) */
    int8_t button_pins[4];   /**< Button GPIO pins (-1 = not assigned) */
    int8_t pwm_pins[8];      /**< PWM output pins (-1 = not assigned) */
    uint8_t endstop_count;   /**< Number of configured endstops */
    uint8_t button_count;    /**< Number of configured buttons */
    uint8_t pwm_count;       /**< Number of configured PWM outputs */
} sb_pin_config_t;

/** Single servo definition. */
typedef struct {
    uint8_t  id;                             /**< Servo bus ID (0-253) */
    char     name[SB_CONFIG_STR_MAX];        /**< Human-readable name */
    uint16_t min_position;                   /**< Minimum allowed position */
    uint16_t max_position;                   /**< Maximum allowed position */
    uint16_t max_speed;                      /**< Maximum speed value */
} sb_servo_config_t;

/** Servo bus module configuration. */
typedef struct {
    char     protocol[32];                   /**< Protocol name (e.g. "feetech-sts") */
    uint32_t baud;                           /**< Bus baud rate */
    uint8_t  servo_count;                    /**< Number of servos */
    sb_servo_config_t servos[SB_MAX_SERVOS]; /**< Servo definitions */
} sb_servo_bus_config_t;

/** Server/network configuration. */
typedef struct {
    uint16_t port;                           /**< HTTPS listen port */
    bool     tls_enabled;                    /**< TLS on/off */
    char     hostname[SB_CONFIG_STR_MAX];    /**< mDNS hostname */
} sb_server_config_t;

/** Security configuration. */
typedef struct {
    uint16_t rate_limit_rpm;                 /**< Requests per minute per key */
    uint16_t api_key_min_length;             /**< Minimum API key length */
} sb_security_config_t;

/** Logging configuration. */
typedef struct {
    uint8_t level;                           /**< ESP_LOG level (0-5) */
} sb_logging_config_t;

/** WiFi mode. */
typedef enum {
    SB_WIFI_MODE_AP  = 0,   /**< Access point (hotspot) */
    SB_WIFI_MODE_STA = 1,   /**< Station (join existing network) */
} sb_wifi_mode_t;

/** WiFi configuration. */
typedef struct {
    sb_wifi_mode_t mode;                     /**< AP or STA */
    char     ap_ssid[33];                    /**< AP mode SSID (auto-generated if empty) */
    char     ap_password[65];                /**< AP mode password */
    uint8_t  ap_channel;                     /**< AP mode channel (1-13) */
    char     sta_ssid[33];                   /**< STA mode target SSID */
    char     sta_password[65];               /**< STA mode password */
} sb_wifi_config_t;

/** Root configuration — everything reachable from here. */
typedef struct {
    sb_server_config_t    server;
    sb_security_config_t  security;
    sb_logging_config_t   logging;
    sb_pin_config_t       pins;
    sb_servo_bus_config_t servo_bus;
    sb_wifi_config_t      wifi;
} sb_config_t;

/**
 * Initialize the config system (open NVS).
 *
 * @return ESP_OK on success.
 */
esp_err_t sb_config_init(void);

/**
 * Load all configuration from NVS into the provided struct.
 *
 * @param[out] config  Destination config struct.
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if not provisioned.
 */
esp_err_t sb_config_load(sb_config_t *config);

/**
 * Save configuration to NVS.
 *
 * @param[in] config  Source config struct.
 * @return ESP_OK on success.
 */
esp_err_t sb_config_save(const sb_config_t *config);

/**
 * Get a pointer to the current running configuration.
 * Returns NULL if config has not been loaded.
 */
const sb_config_t *sb_config_get(void);

/**
 * Load configuration from a JSON object (control server push).
 *
 * Parses the JSON, validates fields, saves to NVS, and updates
 * the running config.
 *
 * @param[in]  json    cJSON object with config fields.
 * @param[out] config  Destination config struct (optional, may be NULL).
 * @return ESP_OK on success.
 */
esp_err_t sb_config_load_json(const void *json, sb_config_t *config);

/**
 * Export current configuration as a JSON object.
 *
 * Caller must free the returned cJSON with cJSON_Delete().
 * Secrets (key hashes, cert paths) are redacted.
 *
 * @return cJSON object or NULL on error.
 */
void *sb_config_to_json(const sb_config_t *config);

/**
 * Check if the node has been provisioned (has valid config in NVS).
 *
 * @return true if provisioned.
 */
bool sb_config_is_provisioned(void);

/**
 * Save a JSON backup of the current config to a file (e.g. SPIFFS).
 * Called automatically after sb_config_save() when SPIFFS is mounted.
 *
 * @param[in] path  File path (e.g. "/spiffs/config_backup.json").
 * @return ESP_OK on success.
 */
esp_err_t sb_config_backup_to_file(const char *path);

/**
 * Restore configuration from a JSON backup file.
 * Parses the file, loads into NVS, and updates running config.
 *
 * @param[in] path  File path (e.g. "/spiffs/config_backup.json").
 * @return ESP_OK on success.
 */
esp_err_t sb_config_restore_from_file(const char *path);

/**
 * Export the full config (including secrets) as a JSON string.
 * Caller must free() the returned string.
 *
 * @return JSON string or NULL on error.
 */
char *sb_config_export_json(void);

#ifdef __cplusplus
}
#endif
