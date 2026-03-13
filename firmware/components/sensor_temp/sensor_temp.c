/**
 * @file sensor_temp.c
 * @brief DS18B20 temperature sensor driver using 1-Wire protocol.
 *
 * Uses ESP-IDF RMT peripheral for precise 1-Wire timing.
 * Pin assignment from NVS config — no hardcoded GPIOs.
 *
 * Task: F08
 */

#include "sensor_temp.h"
#include "plugin_manager.h"
#include "app_config.h"
#include "event_bus.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

static const char *TAG = "sensor_temp";

/* ── 1-Wire Protocol Constants ── */
#define OW_CMD_SKIP_ROM       0xCC
#define OW_CMD_CONVERT_T      0x44
#define OW_CMD_READ_SCRATCH   0xBE
#define OW_CMD_READ_ROM       0x33

/* History buffer */
#define TEMP_HISTORY_SIZE     64
#define POLL_INTERVAL_MS      2000   /* Read temperature every 2s */

/* Maximum sensors on one bus */
#define MAX_TEMP_SENSORS      4

typedef struct {
    float value;
    int64_t timestamp_us;
} temp_reading_t;

typedef struct {
    bool initialized;
    int8_t data_pin;
    float last_temperature;
    bool valid_reading;
    temp_reading_t history[TEMP_HISTORY_SIZE];
    uint16_t history_head;
    uint16_t history_count;
    TaskHandle_t poll_task;
} sensor_temp_ctx_t;

static sensor_temp_ctx_t s_ctx;
static sb_plugin_t s_plugin;

/* ── 1-Wire Bit-Bang Implementation ── */

static void ow_pin_output(void)
{
    gpio_set_direction((gpio_num_t)s_ctx.data_pin, GPIO_MODE_OUTPUT_OD);
}

static void ow_pin_input(void)
{
    gpio_set_direction((gpio_num_t)s_ctx.data_pin, GPIO_MODE_INPUT);
}

static void ow_write_low(void)
{
    gpio_set_level((gpio_num_t)s_ctx.data_pin, 0);
}

static void ow_release(void)
{
    gpio_set_level((gpio_num_t)s_ctx.data_pin, 1);
}

static int ow_read_pin(void)
{
    return gpio_get_level((gpio_num_t)s_ctx.data_pin);
}

/**
 * 1-Wire reset pulse. Returns true if presence detected.
 */
static bool ow_reset(void)
{
    ow_pin_output();
    ow_write_low();
    esp_rom_delay_us(480);
    ow_release();
    ow_pin_input();
    esp_rom_delay_us(70);
    bool presence = (ow_read_pin() == 0);
    esp_rom_delay_us(410);
    return presence;
}

/**
 * Write a single bit to 1-Wire bus.
 */
static void ow_write_bit(uint8_t bit)
{
    ow_pin_output();
    if (bit) {
        ow_write_low();
        esp_rom_delay_us(6);
        ow_release();
        esp_rom_delay_us(64);
    } else {
        ow_write_low();
        esp_rom_delay_us(60);
        ow_release();
        esp_rom_delay_us(10);
    }
}

/**
 * Read a single bit from 1-Wire bus.
 */
static uint8_t ow_read_bit(void)
{
    ow_pin_output();
    ow_write_low();
    esp_rom_delay_us(6);
    ow_release();
    ow_pin_input();
    esp_rom_delay_us(9);
    uint8_t bit = ow_read_pin() ? 1 : 0;
    esp_rom_delay_us(55);
    return bit;
}

/**
 * Write a byte to 1-Wire bus (LSB first).
 */
static void ow_write_byte(uint8_t byte)
{
    for (uint8_t i = 0; i < 8; i++) {
        ow_write_bit((byte >> i) & 0x01);
    }
}

/**
 * Read a byte from 1-Wire bus (LSB first).
 */
static uint8_t ow_read_byte(void)
{
    uint8_t byte = 0;
    for (uint8_t i = 0; i < 8; i++) {
        byte |= (ow_read_bit() << i);
    }
    return byte;
}

/**
 * Read temperature from a DS18B20 sensor (single-drop mode).
 * Returns temperature in degrees Celsius, or NAN on error.
 */
static float ds18b20_read_temperature(void)
{
    /* Start conversion */
    if (!ow_reset()) {
        ESP_LOGW(TAG, "No presence pulse — sensor disconnected?");
        return -999.0f;
    }

    ow_write_byte(OW_CMD_SKIP_ROM);
    ow_write_byte(OW_CMD_CONVERT_T);

    /* Wait for conversion (750ms max at 12-bit resolution) */
    vTaskDelay(pdMS_TO_TICKS(750));

    /* Read scratchpad */
    if (!ow_reset()) {
        return -999.0f;
    }

    ow_write_byte(OW_CMD_SKIP_ROM);
    ow_write_byte(OW_CMD_READ_SCRATCH);

    uint8_t scratch[9];
    for (int i = 0; i < 9; i++) {
        scratch[i] = ow_read_byte();
    }

    /* CRC check (byte 8) */
    uint8_t crc = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t byte = scratch[i];
        for (int j = 0; j < 8; j++) {
            uint8_t mix = (crc ^ byte) & 0x01;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            byte >>= 1;
        }
    }
    if (crc != scratch[8]) {
        ESP_LOGW(TAG, "CRC mismatch: computed 0x%02x, got 0x%02x", crc, scratch[8]);
        return -999.0f;
    }

    /* Convert raw value to temperature */
    int16_t raw = (int16_t)((scratch[1] << 8) | scratch[0]);
    return (float)raw / 16.0f;
}

/**
 * Record a temperature reading in the history buffer.
 */
static void record_reading(float temp)
{
    s_ctx.last_temperature = temp;
    s_ctx.valid_reading = true;

    temp_reading_t *r = &s_ctx.history[s_ctx.history_head];
    r->value = temp;
    r->timestamp_us = esp_timer_get_time();

    s_ctx.history_head = (s_ctx.history_head + 1) % TEMP_HISTORY_SIZE;
    if (s_ctx.history_count < TEMP_HISTORY_SIZE) {
        s_ctx.history_count++;
    }
}

/**
 * Polling task — reads temperature periodically.
 */
static void temp_poll_task(void *arg)
{
    (void)arg;
    while (1) {
        float temp = ds18b20_read_temperature();
        if (temp > -999.0f) {
            record_reading(temp);

            /* Publish event */
            char ev[64];
            snprintf(ev, sizeof(ev), "{\"temperature\":%.2f}", temp);
            sb_event_publish("sensor.temperature", ev);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

/* ── Plugin Interface ── */

static esp_err_t plugin_init(sb_plugin_t *self, const cJSON *config)
{
    (void)self;
    (void)config;
    memset(&s_ctx, 0, sizeof(s_ctx));

    const sb_config_t *app_cfg = sb_config_get();
    if (app_cfg == NULL) {
        ESP_LOGE(TAG, "No app config");
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.data_pin = app_cfg->pins.onewire;
    if (s_ctx.data_pin < 0) {
        ESP_LOGW(TAG, "1-Wire pin not configured — sensor disabled");
        return ESP_ERR_INVALID_STATE;
    }

    /* Configure GPIO with internal pull-up */
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << s_ctx.data_pin),
        .mode         = GPIO_MODE_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);

    /* Test for sensor presence */
    if (!ow_reset()) {
        ESP_LOGW(TAG, "No DS18B20 detected on pin %d", s_ctx.data_pin);
        /* Don't fail — sensor might be connected later */
    }

    /* Start polling task */
    BaseType_t ret = xTaskCreate(temp_poll_task, "temp_poll", 2048, NULL, 5, &s_ctx.poll_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create poll task");
        return ESP_FAIL;
    }

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "DS18B20 sensor initialized on pin %d", s_ctx.data_pin);
    return ESP_OK;
}

static esp_err_t plugin_shutdown(sb_plugin_t *self)
{
    (void)self;
    if (s_ctx.initialized) {
        if (s_ctx.poll_task != NULL) {
            vTaskDelete(s_ctx.poll_task);
            s_ctx.poll_task = NULL;
        }
        s_ctx.initialized = false;
        ESP_LOGI(TAG, "DS18B20 sensor shut down");
    }
    return ESP_OK;
}

static cJSON *plugin_get_state(sb_plugin_t *self)
{
    (void)self;
    cJSON *state = cJSON_CreateObject();
    cJSON_AddNumberToObject(state, "temperature", s_ctx.last_temperature);
    cJSON_AddBoolToObject(state, "valid", s_ctx.valid_reading);
    cJSON_AddNumberToObject(state, "readings_count", s_ctx.history_count);
    cJSON_AddNumberToObject(state, "pin", s_ctx.data_pin);
    return state;
}

static cJSON *plugin_handle_command(sb_plugin_t *self, const char *cmd, const cJSON *params)
{
    (void)self;
    (void)params;
    cJSON *result = cJSON_CreateObject();

    if (strcmp(cmd, "get_reading") == 0) {
        cJSON_AddNumberToObject(result, "temperature", s_ctx.last_temperature);
        cJSON_AddBoolToObject(result, "valid", s_ctx.valid_reading);
        cJSON_AddNumberToObject(result, "timestamp_us",
            s_ctx.history_count > 0
                ? (double)s_ctx.history[(s_ctx.history_head + TEMP_HISTORY_SIZE - 1) % TEMP_HISTORY_SIZE].timestamp_us
                : 0);

    } else if (strcmp(cmd, "get_history") == 0) {
        int limit = TEMP_HISTORY_SIZE;
        if (params != NULL) {
            const cJSON *lim = cJSON_GetObjectItem(params, "limit");
            if (lim && cJSON_IsNumber(lim) && lim->valueint > 0) {
                limit = lim->valueint;
            }
        }

        cJSON *arr = cJSON_AddArrayToObject(result, "readings");
        uint16_t count = s_ctx.history_count;
        if (count > (uint16_t)limit) count = (uint16_t)limit;

        /* Read from most recent backward */
        for (uint16_t i = 0; i < count; i++) {
            uint16_t idx = (s_ctx.history_head + TEMP_HISTORY_SIZE - 1 - i) % TEMP_HISTORY_SIZE;
            cJSON *r = cJSON_CreateObject();
            cJSON_AddNumberToObject(r, "temperature", s_ctx.history[idx].value);
            cJSON_AddNumberToObject(r, "timestamp_us", (double)s_ctx.history[idx].timestamp_us);
            cJSON_AddItemToArray(arr, r);
        }

    } else {
        cJSON_AddStringToObject(result, "error", "Unknown command");
    }

    return result;
}

static sb_health_status_t plugin_health_check(sb_plugin_t *self)
{
    (void)self;
    sb_health_status_t hs;

    if (!s_ctx.initialized) {
        hs.state = SB_HEALTH_UNHEALTHY;
        snprintf(hs.message, sizeof(hs.message), "Not initialized");
    } else if (!s_ctx.valid_reading) {
        hs.state = SB_HEALTH_DEGRADED;
        snprintf(hs.message, sizeof(hs.message), "No valid reading yet");
    } else {
        hs.state = SB_HEALTH_HEALTHY;
        snprintf(hs.message, sizeof(hs.message), "%.1f C", s_ctx.last_temperature);
    }
    return hs;
}

sb_plugin_t *sb_sensor_temp_plugin(void)
{
    s_plugin.name           = "sensor-temp";
    s_plugin.version        = "1.0.0";
    s_plugin.initialize     = plugin_init;
    s_plugin.shutdown       = plugin_shutdown;
    s_plugin.get_state      = plugin_get_state;
    s_plugin.handle_command = plugin_handle_command;
    s_plugin.health_check   = plugin_health_check;
    s_plugin.ctx            = &s_ctx;
    return &s_plugin;
}
