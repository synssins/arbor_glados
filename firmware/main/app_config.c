/**
 * @file app_config.c
 * @brief NVS-backed configuration system.
 *
 * All configuration is stored in NVS (non-volatile storage).
 * No operational defaults compiled in — values must be provisioned
 * via the control server's config push endpoint.
 *
 * NVS key naming convention:
 *   "srv_port"     -> server.port
 *   "srv_tls"      -> server.tls_enabled
 *   "srv_host"     -> server.hostname
 *   "sec_rpm"      -> security.rate_limit_rpm
 *   "sec_keylen"   -> security.api_key_min_length
 *   "log_level"    -> logging.level
 *   "pin_*"        -> pins.*
 *   "sbus_*"       -> servo_bus.*
 *   "sv{N}_*"      -> servo_bus.servos[N].*
 *
 * Task: F02
 */

#include "app_config.h"

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "sb_config";

/** NVS namespace — from Kconfig. */
#define NVS_NS CONFIG_SB_NVS_NAMESPACE

/** Key used to mark that provisioning has been done. */
#define NVS_KEY_PROVISIONED "provisioned"

static sb_config_t s_config;
static bool s_loaded = false;
static nvs_handle_t s_nvs_handle;
static bool s_nvs_open = false;

/* Forward declaration for auto-backup in sb_config_save */
esp_err_t sb_config_backup_to_file(const char *path);

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

static esp_err_t nvs_open_ns(void)
{
    if (s_nvs_open) {
        return ESP_OK;
    }
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs_handle);
    if (ret == ESP_OK) {
        s_nvs_open = true;
    }
    return ret;
}

/** Read a uint16 from NVS; leave *val unchanged if key not found. */
static void nvs_read_u16(const char *key, uint16_t *val)
{
    uint16_t tmp;
    if (nvs_get_u16(s_nvs_handle, key, &tmp) == ESP_OK) {
        *val = tmp;
    }
}

/** Read a uint8 from NVS. */
static void nvs_read_u8(const char *key, uint8_t *val)
{
    uint8_t tmp;
    if (nvs_get_u8(s_nvs_handle, key, &tmp) == ESP_OK) {
        *val = tmp;
    }
}

/** Read an int8 from NVS. */
static void nvs_read_i8(const char *key, int8_t *val)
{
    int8_t tmp;
    if (nvs_get_i8(s_nvs_handle, key, &tmp) == ESP_OK) {
        *val = tmp;
    }
}

/** Read a uint32 from NVS. */
static void nvs_read_u32(const char *key, uint32_t *val)
{
    uint32_t tmp;
    if (nvs_get_u32(s_nvs_handle, key, &tmp) == ESP_OK) {
        *val = tmp;
    }
}

/** Read a string from NVS into a fixed-size buffer. */
static void nvs_read_str(const char *key, char *buf, size_t buf_size)
{
    size_t len = buf_size;
    nvs_get_str(s_nvs_handle, key, buf, &len);
}

/** Read a blob from NVS (for arrays of int8_t). */
static void nvs_read_blob(const char *key, void *buf, size_t expected_size)
{
    size_t len = expected_size;
    nvs_get_blob(s_nvs_handle, key, buf, &len);
}

/* -----------------------------------------------------------------------
 * Init all pin fields to -1 (unassigned)
 * ----------------------------------------------------------------------- */
static void pins_set_defaults(sb_pin_config_t *pins)
{
    pins->servo_bus_tx = -1;
    pins->servo_bus_rx = -1;
    pins->servo_bus_dir = -1;
    pins->i2c_sda = -1;
    pins->i2c_scl = -1;
    pins->spi_mosi = -1;
    pins->spi_miso = -1;
    pins->spi_sclk = -1;
    pins->oled_sda = -1;
    pins->oled_scl = -1;
    pins->ws2812_data = -1;
    pins->onewire = -1;
    memset(pins->endstop_pins, -1, sizeof(pins->endstop_pins));
    memset(pins->button_pins, -1, sizeof(pins->button_pins));
    memset(pins->pwm_pins, -1, sizeof(pins->pwm_pins));
    pins->endstop_count = 0;
    pins->button_count = 0;
    pins->pwm_count = 0;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

esp_err_t sb_config_init(void)
{
    esp_err_t ret = nvs_open_ns();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s",
                 NVS_NS, esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Config system initialized (NVS namespace: %s)", NVS_NS);
    return ESP_OK;
}

esp_err_t sb_config_load(sb_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_nvs_open) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Start with all zeros and unassigned pins */
    memset(config, 0, sizeof(*config));
    pins_set_defaults(&config->pins);

    /* Check if provisioned */
    uint8_t prov = 0;
    esp_err_t ret = nvs_get_u8(s_nvs_handle, NVS_KEY_PROVISIONED, &prov);
    if (ret == ESP_ERR_NVS_NOT_FOUND || prov == 0) {
        ESP_LOGW(TAG, "Node not provisioned — no config in NVS");
        /* Return a minimal boot config so the HTTP server can start
         * for provisioning. Port comes from Kconfig. */
        config->server.port = CONFIG_SB_DEFAULT_API_PORT;
        config->server.tls_enabled = false;
        config->security.rate_limit_rpm = 300;
        config->security.api_key_min_length = 32;
        config->logging.level = 3; /* ESP_LOG_INFO */
        memcpy(&s_config, config, sizeof(s_config));
        s_loaded = true;
        return ESP_ERR_NVS_NOT_FOUND;
    }

    /* ---- Server ---- */
    nvs_read_u16("srv_port", &config->server.port);
    uint8_t tls = 0;
    nvs_read_u8("srv_tls", &tls);
    config->server.tls_enabled = (tls != 0);
    nvs_read_str("srv_host", config->server.hostname, sizeof(config->server.hostname));

    /* ---- Security ---- */
    nvs_read_u16("sec_rpm", &config->security.rate_limit_rpm);
    nvs_read_u16("sec_keylen", &config->security.api_key_min_length);

    /* ---- Logging ---- */
    nvs_read_u8("log_level", &config->logging.level);

    /* ---- Pins ---- */
    nvs_read_i8("pin_sbus_tx", &config->pins.servo_bus_tx);
    nvs_read_i8("pin_sbus_rx", &config->pins.servo_bus_rx);
    nvs_read_i8("pin_sbus_dir", &config->pins.servo_bus_dir);
    nvs_read_i8("pin_i2c_sda", &config->pins.i2c_sda);
    nvs_read_i8("pin_i2c_scl", &config->pins.i2c_scl);
    nvs_read_i8("pin_spi_mo", &config->pins.spi_mosi);
    nvs_read_i8("pin_spi_mi", &config->pins.spi_miso);
    nvs_read_i8("pin_spi_ck", &config->pins.spi_sclk);
    nvs_read_i8("pin_oled_da", &config->pins.oled_sda);
    nvs_read_i8("pin_oled_cl", &config->pins.oled_scl);
    nvs_read_i8("pin_ws2812", &config->pins.ws2812_data);
    nvs_read_i8("pin_1wire", &config->pins.onewire);
    nvs_read_u8("pin_es_cnt", &config->pins.endstop_count);
    nvs_read_u8("pin_btn_cnt", &config->pins.button_count);
    nvs_read_u8("pin_pwm_cnt", &config->pins.pwm_count);
    nvs_read_blob("pin_es", config->pins.endstop_pins, sizeof(config->pins.endstop_pins));
    nvs_read_blob("pin_btn", config->pins.button_pins, sizeof(config->pins.button_pins));
    nvs_read_blob("pin_pwm", config->pins.pwm_pins, sizeof(config->pins.pwm_pins));

    /* ---- WiFi ---- */
    uint8_t wifi_mode = 0;
    nvs_read_u8("wifi_mode", &wifi_mode);
    config->wifi.mode = (sb_wifi_mode_t)wifi_mode;
    nvs_read_str("wifi_ap_ssid", config->wifi.ap_ssid, sizeof(config->wifi.ap_ssid));
    nvs_read_str("wifi_ap_pass", config->wifi.ap_password, sizeof(config->wifi.ap_password));
    nvs_read_u8("wifi_ap_ch", &config->wifi.ap_channel);
    nvs_read_str("wifi_sta_ss", config->wifi.sta_ssid, sizeof(config->wifi.sta_ssid));
    nvs_read_str("wifi_sta_pw", config->wifi.sta_password, sizeof(config->wifi.sta_password));

    /* ---- Servo bus ---- */
    nvs_read_str("sbus_proto", config->servo_bus.protocol, sizeof(config->servo_bus.protocol));
    nvs_read_u32("sbus_baud", &config->servo_bus.baud);
    nvs_read_u8("sbus_cnt", &config->servo_bus.servo_count);

    /* Individual servos */
    for (uint8_t i = 0; i < config->servo_bus.servo_count && i < SB_MAX_SERVOS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "sv%d_id", i);
        nvs_read_u8(key, &config->servo_bus.servos[i].id);
        snprintf(key, sizeof(key), "sv%d_name", i);
        nvs_read_str(key, config->servo_bus.servos[i].name,
                      sizeof(config->servo_bus.servos[i].name));
        snprintf(key, sizeof(key), "sv%d_minp", i);
        nvs_read_u16(key, &config->servo_bus.servos[i].min_position);
        snprintf(key, sizeof(key), "sv%d_maxp", i);
        nvs_read_u16(key, &config->servo_bus.servos[i].max_position);
        snprintf(key, sizeof(key), "sv%d_maxs", i);
        nvs_read_u16(key, &config->servo_bus.servos[i].max_speed);
    }

    memcpy(&s_config, config, sizeof(s_config));
    s_loaded = true;

    ESP_LOGI(TAG, "Config loaded from NVS (port=%d, servos=%d)",
             config->server.port, config->servo_bus.servo_count);
    return ESP_OK;
}

esp_err_t sb_config_save(const sb_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_nvs_open) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret;

    /* ---- Server ---- */
    ret = nvs_set_u16(s_nvs_handle, "srv_port", config->server.port);
    if (ret != ESP_OK) goto save_err;
    ret = nvs_set_u8(s_nvs_handle, "srv_tls", config->server.tls_enabled ? 1 : 0);
    if (ret != ESP_OK) goto save_err;
    ret = nvs_set_str(s_nvs_handle, "srv_host", config->server.hostname);
    if (ret != ESP_OK) goto save_err;

    /* ---- Security ---- */
    ret = nvs_set_u16(s_nvs_handle, "sec_rpm", config->security.rate_limit_rpm);
    if (ret != ESP_OK) goto save_err;
    ret = nvs_set_u16(s_nvs_handle, "sec_keylen", config->security.api_key_min_length);
    if (ret != ESP_OK) goto save_err;

    /* ---- Logging ---- */
    ret = nvs_set_u8(s_nvs_handle, "log_level", config->logging.level);
    if (ret != ESP_OK) goto save_err;

    /* ---- Pins ---- */
    nvs_set_i8(s_nvs_handle, "pin_sbus_tx", config->pins.servo_bus_tx);
    nvs_set_i8(s_nvs_handle, "pin_sbus_rx", config->pins.servo_bus_rx);
    nvs_set_i8(s_nvs_handle, "pin_sbus_dir", config->pins.servo_bus_dir);
    nvs_set_i8(s_nvs_handle, "pin_i2c_sda", config->pins.i2c_sda);
    nvs_set_i8(s_nvs_handle, "pin_i2c_scl", config->pins.i2c_scl);
    nvs_set_i8(s_nvs_handle, "pin_spi_mo", config->pins.spi_mosi);
    nvs_set_i8(s_nvs_handle, "pin_spi_mi", config->pins.spi_miso);
    nvs_set_i8(s_nvs_handle, "pin_spi_ck", config->pins.spi_sclk);
    nvs_set_i8(s_nvs_handle, "pin_oled_da", config->pins.oled_sda);
    nvs_set_i8(s_nvs_handle, "pin_oled_cl", config->pins.oled_scl);
    nvs_set_i8(s_nvs_handle, "pin_ws2812", config->pins.ws2812_data);
    nvs_set_i8(s_nvs_handle, "pin_1wire", config->pins.onewire);
    nvs_set_u8(s_nvs_handle, "pin_es_cnt", config->pins.endstop_count);
    nvs_set_u8(s_nvs_handle, "pin_btn_cnt", config->pins.button_count);
    nvs_set_u8(s_nvs_handle, "pin_pwm_cnt", config->pins.pwm_count);
    nvs_set_blob(s_nvs_handle, "pin_es", config->pins.endstop_pins,
                 sizeof(config->pins.endstop_pins));
    nvs_set_blob(s_nvs_handle, "pin_btn", config->pins.button_pins,
                 sizeof(config->pins.button_pins));
    nvs_set_blob(s_nvs_handle, "pin_pwm", config->pins.pwm_pins,
                 sizeof(config->pins.pwm_pins));

    /* ---- WiFi ---- */
    nvs_set_u8(s_nvs_handle, "wifi_mode", (uint8_t)config->wifi.mode);
    nvs_set_str(s_nvs_handle, "wifi_ap_ssid", config->wifi.ap_ssid);
    nvs_set_str(s_nvs_handle, "wifi_ap_pass", config->wifi.ap_password);
    nvs_set_u8(s_nvs_handle, "wifi_ap_ch", config->wifi.ap_channel);
    nvs_set_str(s_nvs_handle, "wifi_sta_ss", config->wifi.sta_ssid);
    nvs_set_str(s_nvs_handle, "wifi_sta_pw", config->wifi.sta_password);

    /* ---- Servo bus ---- */
    nvs_set_str(s_nvs_handle, "sbus_proto", config->servo_bus.protocol);
    nvs_set_u32(s_nvs_handle, "sbus_baud", config->servo_bus.baud);
    nvs_set_u8(s_nvs_handle, "sbus_cnt", config->servo_bus.servo_count);

    for (uint8_t i = 0; i < config->servo_bus.servo_count && i < SB_MAX_SERVOS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "sv%d_id", i);
        nvs_set_u8(s_nvs_handle, key, config->servo_bus.servos[i].id);
        snprintf(key, sizeof(key), "sv%d_name", i);
        nvs_set_str(s_nvs_handle, key, config->servo_bus.servos[i].name);
        snprintf(key, sizeof(key), "sv%d_minp", i);
        nvs_set_u16(s_nvs_handle, key, config->servo_bus.servos[i].min_position);
        snprintf(key, sizeof(key), "sv%d_maxp", i);
        nvs_set_u16(s_nvs_handle, key, config->servo_bus.servos[i].max_position);
        snprintf(key, sizeof(key), "sv%d_maxs", i);
        nvs_set_u16(s_nvs_handle, key, config->servo_bus.servos[i].max_speed);
    }

    /* Mark as provisioned */
    nvs_set_u8(s_nvs_handle, NVS_KEY_PROVISIONED, 1);

    /* Commit all writes */
    ret = nvs_commit(s_nvs_handle);
    if (ret != ESP_OK) goto save_err;

    /* Update running config */
    memcpy(&s_config, config, sizeof(s_config));
    s_loaded = true;

    ESP_LOGI(TAG, "Config saved to NVS (port=%d, servos=%d)",
             config->server.port, config->servo_bus.servo_count);

    /* Auto-backup to SPIFFS (best-effort, fails silently if not mounted) */
    sb_config_backup_to_file("/spiffs/config_backup.json");

    return ESP_OK;

save_err:
    ESP_LOGE(TAG, "NVS write failed: %s", esp_err_to_name(ret));
    return ret;
}

const sb_config_t *sb_config_get(void)
{
    return s_loaded ? &s_config : NULL;
}

bool sb_config_is_provisioned(void)
{
    if (!s_nvs_open) {
        return false;
    }
    uint8_t prov = 0;
    esp_err_t ret = nvs_get_u8(s_nvs_handle, NVS_KEY_PROVISIONED, &prov);
    return (ret == ESP_OK && prov != 0);
}

/* -----------------------------------------------------------------------
 * JSON import/export (for control server config push)
 * ----------------------------------------------------------------------- */

/**
 * Helper: read a JSON int field, leave *val unchanged if missing.
 */
static void json_get_int(const cJSON *obj, const char *name, int *val)
{
    const cJSON *item = cJSON_GetObjectItem(obj, name);
    if (cJSON_IsNumber(item)) {
        *val = item->valueint;
    }
}

static void json_get_str(const cJSON *obj, const char *name, char *buf, size_t sz)
{
    const cJSON *item = cJSON_GetObjectItem(obj, name);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strncpy(buf, item->valuestring, sz - 1);
        buf[sz - 1] = '\0';
    }
}

static void json_get_bool(const cJSON *obj, const char *name, bool *val)
{
    const cJSON *item = cJSON_GetObjectItem(obj, name);
    if (cJSON_IsBool(item)) {
        *val = cJSON_IsTrue(item);
    }
}

esp_err_t sb_config_load_json(const void *json_ptr, sb_config_t *config)
{
    const cJSON *json = (const cJSON *)json_ptr;
    if (json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    sb_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    pins_set_defaults(&cfg.pins);

    /* If we have a running config, start from that */
    if (s_loaded) {
        memcpy(&cfg, &s_config, sizeof(cfg));
    }

    /* ---- Server ---- */
    const cJSON *server = cJSON_GetObjectItem(json, "server");
    if (server != NULL) {
        int port = cfg.server.port;
        json_get_int(server, "port", &port);
        cfg.server.port = (uint16_t)port;
        json_get_bool(server, "tls_enabled", &cfg.server.tls_enabled);
        json_get_str(server, "hostname", cfg.server.hostname, sizeof(cfg.server.hostname));
    }

    /* ---- Security ---- */
    const cJSON *security = cJSON_GetObjectItem(json, "security");
    if (security != NULL) {
        int rpm = cfg.security.rate_limit_rpm;
        json_get_int(security, "rate_limit_rpm", &rpm);
        cfg.security.rate_limit_rpm = (uint16_t)rpm;
        int keylen = cfg.security.api_key_min_length;
        json_get_int(security, "api_key_min_length", &keylen);
        cfg.security.api_key_min_length = (uint16_t)keylen;
    }

    /* ---- Logging ---- */
    const cJSON *logging = cJSON_GetObjectItem(json, "logging");
    if (logging != NULL) {
        int level = cfg.logging.level;
        json_get_int(logging, "level", &level);
        cfg.logging.level = (uint8_t)level;
    }

    /* ---- Pins ---- */
    const cJSON *pins = cJSON_GetObjectItem(json, "pins");
    if (pins != NULL) {
        int tmp;
        tmp = cfg.pins.servo_bus_tx;  json_get_int(pins, "servo_bus_tx", &tmp);  cfg.pins.servo_bus_tx = (int8_t)tmp;
        tmp = cfg.pins.servo_bus_rx;  json_get_int(pins, "servo_bus_rx", &tmp);  cfg.pins.servo_bus_rx = (int8_t)tmp;
        tmp = cfg.pins.servo_bus_dir; json_get_int(pins, "servo_bus_dir", &tmp); cfg.pins.servo_bus_dir = (int8_t)tmp;
        tmp = cfg.pins.i2c_sda;      json_get_int(pins, "i2c_sda", &tmp);       cfg.pins.i2c_sda = (int8_t)tmp;
        tmp = cfg.pins.i2c_scl;      json_get_int(pins, "i2c_scl", &tmp);       cfg.pins.i2c_scl = (int8_t)tmp;
        tmp = cfg.pins.spi_mosi;     json_get_int(pins, "spi_mosi", &tmp);      cfg.pins.spi_mosi = (int8_t)tmp;
        tmp = cfg.pins.spi_miso;     json_get_int(pins, "spi_miso", &tmp);      cfg.pins.spi_miso = (int8_t)tmp;
        tmp = cfg.pins.spi_sclk;     json_get_int(pins, "spi_sclk", &tmp);      cfg.pins.spi_sclk = (int8_t)tmp;
        tmp = cfg.pins.oled_sda;     json_get_int(pins, "oled_sda", &tmp);      cfg.pins.oled_sda = (int8_t)tmp;
        tmp = cfg.pins.oled_scl;     json_get_int(pins, "oled_scl", &tmp);      cfg.pins.oled_scl = (int8_t)tmp;
        tmp = cfg.pins.ws2812_data;  json_get_int(pins, "ws2812_data", &tmp);   cfg.pins.ws2812_data = (int8_t)tmp;
        tmp = cfg.pins.onewire;      json_get_int(pins, "onewire", &tmp);       cfg.pins.onewire = (int8_t)tmp;

        /* Array pins */
        const cJSON *arr;
        arr = cJSON_GetObjectItem(pins, "endstop_pins");
        if (cJSON_IsArray(arr)) {
            cfg.pins.endstop_count = 0;
            const cJSON *item;
            cJSON_ArrayForEach(item, arr) {
                if (cfg.pins.endstop_count < 8 && cJSON_IsNumber(item)) {
                    cfg.pins.endstop_pins[cfg.pins.endstop_count++] = (int8_t)item->valueint;
                }
            }
        }
        arr = cJSON_GetObjectItem(pins, "button_pins");
        if (cJSON_IsArray(arr)) {
            cfg.pins.button_count = 0;
            const cJSON *item;
            cJSON_ArrayForEach(item, arr) {
                if (cfg.pins.button_count < 4 && cJSON_IsNumber(item)) {
                    cfg.pins.button_pins[cfg.pins.button_count++] = (int8_t)item->valueint;
                }
            }
        }
        arr = cJSON_GetObjectItem(pins, "pwm_pins");
        if (cJSON_IsArray(arr)) {
            cfg.pins.pwm_count = 0;
            const cJSON *item;
            cJSON_ArrayForEach(item, arr) {
                if (cfg.pins.pwm_count < 8 && cJSON_IsNumber(item)) {
                    cfg.pins.pwm_pins[cfg.pins.pwm_count++] = (int8_t)item->valueint;
                }
            }
        }
    }

    /* ---- WiFi ---- */
    const cJSON *wifi_json = cJSON_GetObjectItem(json, "wifi");
    if (wifi_json != NULL) {
        int mode = cfg.wifi.mode;
        json_get_int(wifi_json, "mode", &mode);
        cfg.wifi.mode = (sb_wifi_mode_t)mode;
        json_get_str(wifi_json, "ap_ssid", cfg.wifi.ap_ssid, sizeof(cfg.wifi.ap_ssid));
        json_get_str(wifi_json, "ap_password", cfg.wifi.ap_password, sizeof(cfg.wifi.ap_password));
        int ch = cfg.wifi.ap_channel;
        json_get_int(wifi_json, "ap_channel", &ch);
        cfg.wifi.ap_channel = (uint8_t)ch;
        json_get_str(wifi_json, "sta_ssid", cfg.wifi.sta_ssid, sizeof(cfg.wifi.sta_ssid));
        json_get_str(wifi_json, "sta_password", cfg.wifi.sta_password, sizeof(cfg.wifi.sta_password));
    }

    /* ---- Servo bus ---- */
    const cJSON *servo_bus = cJSON_GetObjectItem(json, "servo_bus");
    if (servo_bus != NULL) {
        json_get_str(servo_bus, "protocol", cfg.servo_bus.protocol,
                     sizeof(cfg.servo_bus.protocol));
        int baud = (int)cfg.servo_bus.baud;
        json_get_int(servo_bus, "baud", &baud);
        cfg.servo_bus.baud = (uint32_t)baud;

        const cJSON *servos = cJSON_GetObjectItem(servo_bus, "servos");
        if (cJSON_IsArray(servos)) {
            cfg.servo_bus.servo_count = 0;
            const cJSON *sv;
            cJSON_ArrayForEach(sv, servos) {
                if (cfg.servo_bus.servo_count >= SB_MAX_SERVOS) break;
                uint8_t idx = cfg.servo_bus.servo_count;
                int id = 0;
                json_get_int(sv, "id", &id);
                cfg.servo_bus.servos[idx].id = (uint8_t)id;
                json_get_str(sv, "name", cfg.servo_bus.servos[idx].name,
                             sizeof(cfg.servo_bus.servos[idx].name));
                int minp = 0, maxp = 4095, maxs = 2000;
                json_get_int(sv, "min_position", &minp);
                json_get_int(sv, "max_position", &maxp);
                json_get_int(sv, "max_speed", &maxs);
                cfg.servo_bus.servos[idx].min_position = (uint16_t)minp;
                cfg.servo_bus.servos[idx].max_position = (uint16_t)maxp;
                cfg.servo_bus.servos[idx].max_speed = (uint16_t)maxs;
                cfg.servo_bus.servo_count++;
            }
        }
    }

    /* Save to NVS */
    esp_err_t ret = sb_config_save(&cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Output to caller if requested */
    if (config != NULL) {
        memcpy(config, &cfg, sizeof(cfg));
    }

    ESP_LOGI(TAG, "Config loaded from JSON and saved to NVS");
    return ESP_OK;
}

void *sb_config_to_json(const sb_config_t *config)
{
    if (config == NULL) {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    /* Server */
    cJSON *server = cJSON_AddObjectToObject(root, "server");
    cJSON_AddNumberToObject(server, "port", config->server.port);
    cJSON_AddBoolToObject(server, "tls_enabled", config->server.tls_enabled);
    cJSON_AddStringToObject(server, "hostname", config->server.hostname);

    /* Security (no secrets exposed) */
    cJSON *security = cJSON_AddObjectToObject(root, "security");
    cJSON_AddNumberToObject(security, "rate_limit_rpm", config->security.rate_limit_rpm);
    cJSON_AddNumberToObject(security, "api_key_min_length", config->security.api_key_min_length);

    /* Logging */
    cJSON *logging = cJSON_AddObjectToObject(root, "logging");
    cJSON_AddNumberToObject(logging, "level", config->logging.level);

    /* Pins */
    cJSON *pins = cJSON_AddObjectToObject(root, "pins");
    cJSON_AddNumberToObject(pins, "servo_bus_tx", config->pins.servo_bus_tx);
    cJSON_AddNumberToObject(pins, "servo_bus_rx", config->pins.servo_bus_rx);
    cJSON_AddNumberToObject(pins, "servo_bus_dir", config->pins.servo_bus_dir);
    cJSON_AddNumberToObject(pins, "i2c_sda", config->pins.i2c_sda);
    cJSON_AddNumberToObject(pins, "i2c_scl", config->pins.i2c_scl);
    cJSON_AddNumberToObject(pins, "spi_mosi", config->pins.spi_mosi);
    cJSON_AddNumberToObject(pins, "spi_miso", config->pins.spi_miso);
    cJSON_AddNumberToObject(pins, "spi_sclk", config->pins.spi_sclk);
    cJSON_AddNumberToObject(pins, "oled_sda", config->pins.oled_sda);
    cJSON_AddNumberToObject(pins, "oled_scl", config->pins.oled_scl);
    cJSON_AddNumberToObject(pins, "ws2812_data", config->pins.ws2812_data);
    cJSON_AddNumberToObject(pins, "onewire", config->pins.onewire);

    cJSON *es_arr = cJSON_AddArrayToObject(pins, "endstop_pins");
    for (uint8_t i = 0; i < config->pins.endstop_count; i++) {
        cJSON_AddItemToArray(es_arr, cJSON_CreateNumber(config->pins.endstop_pins[i]));
    }
    cJSON *btn_arr = cJSON_AddArrayToObject(pins, "button_pins");
    for (uint8_t i = 0; i < config->pins.button_count; i++) {
        cJSON_AddItemToArray(btn_arr, cJSON_CreateNumber(config->pins.button_pins[i]));
    }
    cJSON *pwm_arr = cJSON_AddArrayToObject(pins, "pwm_pins");
    for (uint8_t i = 0; i < config->pins.pwm_count; i++) {
        cJSON_AddItemToArray(pwm_arr, cJSON_CreateNumber(config->pins.pwm_pins[i]));
    }

    /* WiFi */
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddNumberToObject(wifi, "mode", config->wifi.mode);
    cJSON_AddStringToObject(wifi, "ap_ssid", config->wifi.ap_ssid);
    cJSON_AddStringToObject(wifi, "ap_password", config->wifi.ap_password);
    cJSON_AddNumberToObject(wifi, "ap_channel", config->wifi.ap_channel);
    cJSON_AddStringToObject(wifi, "sta_ssid", config->wifi.sta_ssid);
    /* STA password redacted from public export */

    /* Servo bus */
    cJSON *sbus = cJSON_AddObjectToObject(root, "servo_bus");
    cJSON_AddStringToObject(sbus, "protocol", config->servo_bus.protocol);
    cJSON_AddNumberToObject(sbus, "baud", config->servo_bus.baud);
    cJSON *servos = cJSON_AddArrayToObject(sbus, "servos");
    for (uint8_t i = 0; i < config->servo_bus.servo_count; i++) {
        cJSON *sv = cJSON_CreateObject();
        cJSON_AddNumberToObject(sv, "id", config->servo_bus.servos[i].id);
        cJSON_AddStringToObject(sv, "name", config->servo_bus.servos[i].name);
        cJSON_AddNumberToObject(sv, "min_position", config->servo_bus.servos[i].min_position);
        cJSON_AddNumberToObject(sv, "max_position", config->servo_bus.servos[i].max_position);
        cJSON_AddNumberToObject(sv, "max_speed", config->servo_bus.servos[i].max_speed);
        cJSON_AddItemToArray(servos, sv);
    }

    return root;
}

/* -----------------------------------------------------------------------
 * Full export (including secrets) for backup — NOT for API responses
 * ----------------------------------------------------------------------- */

static void *config_to_json_full(const sb_config_t *config)
{
    cJSON *root = (cJSON *)sb_config_to_json(config);
    if (root == NULL) return NULL;

    /* Add STA password (not included in public export) */
    cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    if (wifi) {
        cJSON_AddStringToObject(wifi, "sta_password", config->wifi.sta_password);
    }

    return root;
}

char *sb_config_export_json(void)
{
    if (!s_loaded) return NULL;
    cJSON *root = (cJSON *)config_to_json_full(&s_config);
    if (root == NULL) return NULL;
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return str;
}

/* -----------------------------------------------------------------------
 * SPIFFS backup/restore
 * ----------------------------------------------------------------------- */

#define CONFIG_BACKUP_PATH "/spiffs/config_backup.json"

esp_err_t sb_config_backup_to_file(const char *path)
{
    if (!s_loaded) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = (cJSON *)config_to_json_full(&s_config);
    if (root == NULL) {
        ESP_LOGE(TAG, "Backup: failed to serialize config");
        return ESP_ERR_NO_MEM;
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGW(TAG, "Backup: cannot open %s for writing (SPIFFS not mounted?)", path);
        free(json_str);
        return ESP_ERR_NOT_FOUND;
    }

    size_t len = strlen(json_str);
    size_t written = fwrite(json_str, 1, len, f);
    fclose(f);
    free(json_str);

    if (written != len) {
        ESP_LOGE(TAG, "Backup: partial write (%zu/%zu)", written, len);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Config backed up to %s (%zu bytes)", path, len);
    return ESP_OK;
}

esp_err_t sb_config_restore_from_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGW(TAG, "Restore: no backup file at %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 16384) {
        ESP_LOGE(TAG, "Restore: invalid backup size %ld", fsize);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc((size_t)fsize + 1);
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[read] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);

    if (json == NULL) {
        ESP_LOGE(TAG, "Restore: invalid JSON in backup file");
        return ESP_ERR_INVALID_ARG;
    }

    sb_config_t restored;
    esp_err_t ret = sb_config_load_json(json, &restored);
    cJSON_Delete(json);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Config restored from %s", path);
    } else {
        ESP_LOGE(TAG, "Restore: failed to apply config from %s", path);
    }

    return ret;
}
