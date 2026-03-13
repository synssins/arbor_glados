/**
 * @file wifi_manager.h
 * @brief WiFi AP/STA manager.
 *
 * Starts in AP mode by default (Arbor-XXXX hotspot).
 * STA mode configurable via WebUI settings.
 */

#pragma once

#include "esp_err.h"
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Current WiFi state, readable by OLED display and API. */
typedef struct {
    bool     ap_active;
    bool     sta_connected;
    char     ap_ssid[33];
    char     ap_password[65];
    char     sta_ssid[33];
    char     ip_addr[16];           /**< Current IP as string */
    char     board_id[20];          /**< Unique board ID from MAC */
    uint8_t  mac[6];
} sb_wifi_state_t;

/**
 * Initialize WiFi subsystem and start in the configured mode.
 * If no config provisioned, starts in AP mode with defaults.
 *
 * @param wifi_cfg  WiFi config (NULL = use defaults).
 * @return ESP_OK on success.
 */
esp_err_t sb_wifi_init(const sb_wifi_config_t *wifi_cfg);

/**
 * Get current WiFi state (thread-safe snapshot).
 */
const sb_wifi_state_t *sb_wifi_get_state(void);

/** Scan result entry. */
typedef struct {
    char     ssid[33];
    int8_t   rssi;
    uint8_t  authmode;  /**< wifi_auth_mode_t */
} sb_wifi_scan_entry_t;

/**
 * Scan for nearby WiFi networks.
 *
 * @param[out] results  Array to fill with scan results.
 * @param[in,out] count  In: max entries. Out: actual count.
 * @return ESP_OK on success.
 */
esp_err_t sb_wifi_scan(sb_wifi_scan_entry_t *results, uint16_t *count);

/**
 * Switch to STA mode and connect to a network.
 * Saves the config to NVS. Falls back to AP if connection fails.
 *
 * @param ssid      Target SSID.
 * @param password  Target password.
 * @return ESP_OK on successful connection.
 */
esp_err_t sb_wifi_connect_sta(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
