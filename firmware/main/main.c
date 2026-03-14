/**
 * @file main.c
 * @brief Arbor ESP32 firmware entry point.
 *
 * Initialization sequence:
 *   1. NVS flash init + OTA rollback confirmation
 *   2. Load configuration from NVS (use defaults if not provisioned)
 *   3. OLED display init (show boot status)
 *   4. WiFi init (AP or STA mode)
 *   5. OLED status update (show SSID/IP)
 *   6. Initialize event bus + plugin system
 *   7. Start HTTP server + API routes + WebSocket + WebUI
 *
 * Targets:
 *   - ESP32-WROOM-32 (Waveshare Servo Driver board) — servo + sensor plugins
 *   - ESP32-C3 (XIAO ESP32-C3) — BLDC motor driver plugin
 *
 * Task: F01, BLDC Phase A
 */

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "esp_spiffs.h"
#include "version.h"

#include "app_config.h"
#include "pin_validator.h"
#include "event_bus.h"
#include "plugin_manager.h"
#include "http_server.h"
#include "ws_server.h"
#include "api_auth.h"
#include "api_system.h"
#include "api_servo.h"
#include "api_sensor.h"
#include "api_emergency.h"
#include "api_ota.h"
#include "api_led.h"
#include "api_pwm_servo.h"
#include "api_cors.h"
#include "webui_server.h"
#include "wifi_manager.h"
#include "oled_display.h"

/* Plugin headers from components/ */
#include "servo_bus.h"
#include "servo_pwm.h"
#include "sensor_temp.h"
#include "sensor_endstop.h"
#include "led_ws2812.h"
#include "bldc_driver.h"

static const char *TAG = "arbor";

/**
 * Apply Waveshare Servo Driver board defaults when NVS has no config.
 */
static void apply_defaults(sb_config_t *config)
{
    memset(config, 0, sizeof(*config));

    /* Waveshare board pin defaults */
    config->pins.servo_bus_tx  = 19;
    config->pins.servo_bus_rx  = 18;
    config->pins.servo_bus_dir = -1;   /* Hardware auto-direction */
    config->pins.oled_sda      = 21;
    config->pins.oled_scl      = 22;
    config->pins.ws2812_data   = 23;
    config->pins.i2c_sda       = -1;
    config->pins.i2c_scl       = -1;
    config->pins.spi_mosi      = -1;
    config->pins.spi_miso      = -1;
    config->pins.spi_sclk      = -1;
    config->pins.onewire       = -1;
    memset(config->pins.endstop_pins, -1, sizeof(config->pins.endstop_pins));
    memset(config->pins.button_pins, -1, sizeof(config->pins.button_pins));
    memset(config->pins.pwm_pins, -1, sizeof(config->pins.pwm_pins));

    /* Server defaults */
    config->server.port = 80;
    config->server.tls_enabled = false;
    strncpy(config->server.hostname, "Arbor", sizeof(config->server.hostname) - 1);

    /* Servo bus defaults */
    strncpy(config->servo_bus.protocol, "feetech-sts", sizeof(config->servo_bus.protocol) - 1);
    config->servo_bus.baud = 1000000;

    /* Security defaults */
    config->security.rate_limit_rpm = 120;
    config->security.api_key_min_length = 8;

    /* Logging */
    config->logging.level = 3; /* ESP_LOG_INFO */

    /* WiFi defaults: AP mode */
    config->wifi.mode = SB_WIFI_MODE_AP;
    /* ap_ssid left empty — wifi_manager generates Arbor-XXXX from MAC */
    strncpy(config->wifi.ap_password, "12345678", sizeof(config->wifi.ap_password) - 1);
    config->wifi.ap_channel = 1;

    ESP_LOGW(TAG, "Using Waveshare Servo Driver board defaults");
}

/** Mount SPIFFS early so config backup/restore can use it. */
static bool s_spiffs_mounted = false;

static void mount_spiffs_early(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_OK) {
        s_spiffs_mounted = true;
        ESP_LOGI(TAG, "SPIFFS mounted early for config backup");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        /* Already mounted */
        s_spiffs_mounted = true;
    } else {
        ESP_LOGW(TAG, "Early SPIFFS mount failed: %s (backup unavailable)", esp_err_to_name(ret));
    }
}

bool sb_spiffs_is_mounted(void)
{
    return s_spiffs_mounted;
}

void app_main(void)
{
    esp_err_t ret;
    bool nvs_was_erased = false;

    ESP_LOGI(TAG, "Arbor firmware v%s starting...", SB_VERSION);

    /* ---- 0. OTA rollback confirmation ---- */
    esp_ota_mark_app_valid_cancel_rollback();

    /* ---- 1. Mount SPIFFS early (for config backup/restore) ---- */
    mount_spiffs_early();

    /* ---- 2. NVS Flash ---- */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        nvs_was_erased = true;
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ---- 3. Configuration ---- */
    ret = sb_config_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config init failed: %s", esp_err_to_name(ret));
        return;
    }

    sb_config_t config;
    ret = sb_config_load(&config);
    if (ret != ESP_OK) {
        /* NVS empty — try restoring from SPIFFS backup first */
        if (nvs_was_erased && s_spiffs_mounted) {
            ESP_LOGI(TAG, "NVS was erased — attempting restore from SPIFFS backup");
            if (sb_config_restore_from_file("/spiffs/config_backup.json") == ESP_OK) {
                sb_config_load(&config);
                ESP_LOGI(TAG, "Config restored from SPIFFS backup");
            } else {
                ESP_LOGW(TAG, "No backup found — applying board defaults");
                apply_defaults(&config);
                sb_config_save(&config);
            }
        } else {
            ESP_LOGW(TAG, "No config in NVS — applying and saving board defaults");
            apply_defaults(&config);
            sb_config_save(&config);
        }
    }

    /* Auto-backup config to SPIFFS for crash recovery */
    if (s_spiffs_mounted) {
        sb_config_backup_to_file("/spiffs/config_backup.json");
    }

    /* ---- 3. OLED display ---- */
    ret = sb_oled_init(config.pins.oled_sda, config.pins.oled_scl);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OLED init failed (non-fatal): %s", esp_err_to_name(ret));
    } else {
        sb_oled_clear();
        sb_oled_text(0, config.server.hostname);
        sb_oled_text(1, "Booting...");
        sb_oled_flush();
    }

    /* ---- 4. WiFi ---- */
    ret = sb_wifi_init(&config.wifi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
        /* Continue anyway — WebUI won't be reachable but serial is */
    }

    /* ---- 5. OLED status update ---- */
    sb_oled_update_status();

    /* ---- 6. Pin validation (soft — don't block boot) ---- */
    ret = sb_pin_validate(&config.pins);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Pin validation warnings — check config");
    }

    /* ---- 7. Event bus ---- */
    ret = sb_event_bus_init();
    ESP_ERROR_CHECK(ret);

    /* ---- 8. Plugin system ---- */
    ret = sb_plugin_manager_init();
    ESP_ERROR_CHECK(ret);

    /* Target-specific plugin registration.
     * ESP32-WROOM: servo + sensor + LED plugins (Waveshare Servo Driver board)
     * ESP32-C3:    BLDC motor plugin (XIAO ESP32-C3 motor controller)
     * BLDC plugin is always registered but idles with motor_count=0. */
#if !CONFIG_IDF_TARGET_ESP32C3
    sb_plugin_register(sb_servo_bus_plugin());
    sb_plugin_register(sb_servo_pwm_plugin());
    /* Only register sensor_temp if 1-Wire pin is configured */
    if (config.pins.onewire >= 0) {
        sb_plugin_register(sb_sensor_temp_plugin());
    }
    sb_plugin_register(sb_sensor_endstop_plugin());
    sb_plugin_register(sb_led_ws2812_plugin());
#endif

    /* BLDC motor driver — active on ESP32-C3, available on all targets */
    sb_plugin_register(sb_bldc_driver_plugin());

    ret = sb_plugin_init_all(NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Some plugins failed to initialize (non-fatal)");
    }

    /* ---- 9. HTTP server ---- */
    ret = sb_http_server_start(&config.server);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(ret));
        return;
    }

    httpd_handle_t server = sb_http_server_get_handle();
    if (server == NULL) {
        ESP_LOGE(TAG, "No HTTP server handle");
        return;
    }

    /* Auth middleware */
    ret = sb_auth_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Auth init failed — running without auth");
    }

    /* CORS — must be before API routes for OPTIONS preflight */
    sb_cors_register(server);

    /* API routes */
    sb_api_system_register(server);
    sb_api_servo_register(server);
    sb_api_sensor_register(server);
    sb_api_emergency_register(server);
    sb_api_ota_register(server);
    sb_api_led_register(server);
    sb_api_pwm_servo_register(server);

    /* WebSocket */
    ret = sb_ws_init(server);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WebSocket init failed: %s", esp_err_to_name(ret));
    }

    /* WebUI static file server */
    ret = sb_webui_server_init(server);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WebUI server init failed — continuing without WebUI");
    }

    ESP_LOGI(TAG, "Arbor firmware ready (port %d)", config.server.port);
}
