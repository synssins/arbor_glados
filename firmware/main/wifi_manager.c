/**
 * @file wifi_manager.c
 * @brief WiFi AP/STA manager for Arbor.
 *
 * Default: AP mode, SSID "Arbor-XXXX" (last 4 hex of MAC),
 * password "12345678". STA mode configurable via WebUI.
 */

#include "wifi_manager.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "sb_wifi";

#define DEFAULT_AP_PASSWORD  "12345678"
#define DEFAULT_AP_CHANNEL   1
#define MAX_STA_RETRIES      5

/* Event bits for STA connection */
#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_FAIL_BIT        BIT1

static sb_wifi_state_t s_state = {0};
static EventGroupHandle_t s_wifi_event_group;
static int s_sta_retry_count = 0;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;

/**
 * Generate board ID and default SSID from MAC address.
 */
static void generate_board_identity(void)
{
    esp_read_mac(s_state.mac, ESP_MAC_WIFI_SOFTAP);

    snprintf(s_state.board_id, sizeof(s_state.board_id),
             "SB-%02X%02X%02X",
             s_state.mac[3], s_state.mac[4], s_state.mac[5]);

    /* Default AP SSID: Arbor-XXXX (last 2 bytes hex) */
    if (s_state.ap_ssid[0] == '\0') {
        snprintf(s_state.ap_ssid, sizeof(s_state.ap_ssid),
                 "Arbor-%02X%02X",
                 s_state.mac[4], s_state.mac[5]);
    }
}

/**
 * WiFi event handler.
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "Station connected: " MACSTR, MAC2STR(ev->mac));
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *ev = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "Station disconnected: " MACSTR, MAC2STR(ev->mac));
            break;
        }
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_state.sta_connected = false;
            s_state.ip_addr[0] = '\0';
            if (s_sta_retry_count < MAX_STA_RETRIES) {
                esp_wifi_connect();
                s_sta_retry_count++;
                ESP_LOGW(TAG, "STA reconnect attempt %d/%d", s_sta_retry_count, MAX_STA_RETRIES);
            } else {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                ESP_LOGE(TAG, "STA connection failed after %d retries", MAX_STA_RETRIES);
            }
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        snprintf(s_state.ip_addr, sizeof(s_state.ip_addr),
                 IPSTR, IP2STR(&ev->ip_info.ip));
        s_state.sta_connected = true;
        s_sta_retry_count = 0;
        ESP_LOGI(TAG, "STA connected, IP: %s", s_state.ip_addr);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * Start AP mode.
 */
static esp_err_t start_ap(const sb_wifi_config_t *cfg)
{
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t wifi_config = {0};

    /* SSID */
    const char *ssid = (cfg && cfg->ap_ssid[0]) ? cfg->ap_ssid : s_state.ap_ssid;
    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = (uint8_t)strlen(ssid);

    /* Password */
    const char *pass = (cfg && cfg->ap_password[0]) ? cfg->ap_password : DEFAULT_AP_PASSWORD;
    strncpy((char *)wifi_config.ap.password, pass, sizeof(wifi_config.ap.password) - 1);

    /* Channel */
    wifi_config.ap.channel = (cfg && cfg->ap_channel > 0) ? cfg->ap_channel : DEFAULT_AP_CHANNEL;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    if (strlen(pass) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) return ret;

    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) return ret;

    ret = esp_wifi_start();
    if (ret != ESP_OK) return ret;

    /* Store active SSID/password for display */
    strncpy(s_state.ap_ssid, ssid, sizeof(s_state.ap_ssid) - 1);
    strncpy(s_state.ap_password, pass, sizeof(s_state.ap_password) - 1);
    s_state.ap_active = true;

    /* AP gateway IP */
    strncpy(s_state.ip_addr, "192.168.4.1", sizeof(s_state.ip_addr));

    ESP_LOGI(TAG, "AP started: SSID=\"%s\" Password=\"%s\" IP=%s",
             s_state.ap_ssid, s_state.ap_password, s_state.ip_addr);

    return ESP_OK;
}

/**
 * Start STA mode.
 */
static esp_err_t start_sta(const sb_wifi_config_t *cfg)
{
    if (!cfg || cfg->sta_ssid[0] == '\0') {
        ESP_LOGE(TAG, "STA mode requested but no SSID configured");
        return ESP_ERR_INVALID_ARG;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, cfg->sta_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, cfg->sta_password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) return ret;

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) return ret;

    ret = esp_wifi_start();
    if (ret != ESP_OK) return ret;

    strncpy(s_state.sta_ssid, cfg->sta_ssid, sizeof(s_state.sta_ssid) - 1);

    ESP_LOGI(TAG, "STA mode: connecting to \"%s\"...", s_state.sta_ssid);

    /* Wait for connection or failure */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "STA connected to \"%s\"", s_state.sta_ssid);
        return ESP_OK;
    }

    /* STA failed — fall back to AP mode */
    ESP_LOGW(TAG, "STA connection failed — falling back to AP mode");
    esp_wifi_stop();
    if (s_sta_netif) {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = NULL;
    }
    s_sta_retry_count = 0;
    return start_ap(cfg);
}

esp_err_t sb_wifi_init(const sb_wifi_config_t *wifi_cfg)
{
    memset(&s_state, 0, sizeof(s_state));

    s_wifi_event_group = xEventGroupCreate();

    /* Initialize TCP/IP and event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Initialize WiFi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* We manage WiFi config ourselves (in our NVS namespace).
     * Tell ESP-IDF NOT to persist its own WiFi config — prevents
     * conflicts with stale data after full-flash or NVS migration. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    /* Generate board identity from MAC */
    generate_board_identity();

    ESP_LOGI(TAG, "Board ID: %s", s_state.board_id);

    /* Start in configured mode (default: AP) */
    if (wifi_cfg && wifi_cfg->mode == SB_WIFI_MODE_STA && wifi_cfg->sta_ssid[0] != '\0') {
        return start_sta(wifi_cfg);
    }

    return start_ap(wifi_cfg);
}

const sb_wifi_state_t *sb_wifi_get_state(void)
{
    return &s_state;
}

esp_err_t sb_wifi_scan(sb_wifi_scan_entry_t *results, uint16_t *count)
{
    if (results == NULL || count == NULL || *count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    /* Scanning requires STA or APSTA mode.  If we're pure-AP, switch to
     * APSTA temporarily so the scan can proceed, then switch back.
     * ESP-IDF v5.x requires a STA netif to exist for scanning. */
    wifi_mode_t cur_mode;
    esp_wifi_get_mode(&cur_mode);
    bool switched = false;
    bool created_sta_netif = false;

    if (cur_mode == WIFI_MODE_AP) {
        /* Create a temporary STA netif if one doesn't exist yet —
         * esp_wifi_scan_start() needs a STA interface. */
        if (s_sta_netif == NULL) {
            s_sta_netif = esp_netif_create_default_wifi_sta();
            if (s_sta_netif == NULL) {
                ESP_LOGE(TAG, "Failed to create STA netif for scan");
                return ESP_FAIL;
            }
            created_sta_netif = true;
        }

        esp_err_t sw = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (sw != ESP_OK) {
            ESP_LOGE(TAG, "Failed to switch to APSTA for scan: %s", esp_err_to_name(sw));
            if (created_sta_netif) {
                esp_netif_destroy(s_sta_netif);
                s_sta_netif = NULL;
            }
            return sw;
        }
        switched = true;
    }

    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Scan start failed: %s", esp_err_to_name(ret));
        if (switched) esp_wifi_set_mode(WIFI_MODE_AP);
        if (created_sta_netif) {
            esp_netif_destroy(s_sta_netif);
            s_sta_netif = NULL;
        }
        return ret;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        *count = 0;
        if (switched) esp_wifi_set_mode(WIFI_MODE_AP);
        if (created_sta_netif) {
            esp_netif_destroy(s_sta_netif);
            s_sta_netif = NULL;
        }
        return ESP_OK;
    }

    uint16_t max = *count;
    if (ap_count > max) ap_count = max;

    wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (ap_records == NULL) {
        if (switched) esp_wifi_set_mode(WIFI_MODE_AP);
        if (created_sta_netif) {
            esp_netif_destroy(s_sta_netif);
            s_sta_netif = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    ret = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (ret != ESP_OK) {
        free(ap_records);
        if (switched) esp_wifi_set_mode(WIFI_MODE_AP);
        if (created_sta_netif) {
            esp_netif_destroy(s_sta_netif);
            s_sta_netif = NULL;
        }
        return ret;
    }

    for (uint16_t i = 0; i < ap_count; i++) {
        strncpy(results[i].ssid, (const char *)ap_records[i].ssid, 32);
        results[i].ssid[32] = '\0';
        results[i].rssi = ap_records[i].rssi;
        results[i].authmode = (uint8_t)ap_records[i].authmode;
    }
    *count = ap_count;

    free(ap_records);

    /* Restore AP-only mode if we switched */
    if (switched) {
        esp_wifi_set_mode(WIFI_MODE_AP);
    }
    if (created_sta_netif) {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = NULL;
    }

    ESP_LOGI(TAG, "Scan complete: %d networks found", ap_count);
    return ESP_OK;
}

esp_err_t sb_wifi_connect_sta(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Switching to STA mode: SSID=%s", ssid);

    /* Stop current WiFi */
    esp_wifi_stop();

    /* Destroy existing netifs */
    if (s_ap_netif) {
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
    }
    if (s_sta_netif) {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = NULL;
    }

    /* Reset state */
    s_state.ap_active = false;
    s_state.sta_connected = false;
    s_state.ip_addr[0] = '\0';
    s_sta_retry_count = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    /* Save to NVS config */
    sb_config_t cfg;
    const sb_config_t *cur = sb_config_get();
    if (cur) {
        memcpy(&cfg, cur, sizeof(cfg));
    } else {
        memset(&cfg, 0, sizeof(cfg));
    }
    cfg.wifi.mode = SB_WIFI_MODE_STA;
    strncpy(cfg.wifi.sta_ssid, ssid, sizeof(cfg.wifi.sta_ssid) - 1);
    if (password) {
        strncpy(cfg.wifi.sta_password, password, sizeof(cfg.wifi.sta_password) - 1);
    }
    sb_config_save(&cfg);

    /* Start STA */
    sb_wifi_config_t wifi_cfg = cfg.wifi;
    return start_sta(&wifi_cfg);
}
