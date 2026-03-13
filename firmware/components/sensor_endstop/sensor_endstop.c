/**
 * @file sensor_endstop.c
 * @brief GPIO interrupt-driven endstop sensor.
 *
 * Supports configurable GPIO pins with debounce, NC/NO type.
 * Publishes events on state change via event bus.
 *
 * Task: F09
 */

#include "sensor_endstop.h"
#include "plugin_manager.h"
#include "app_config.h"
#include "event_bus.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cJSON.h"

static const char *TAG = "sensor_endstop";

#define MAX_ENDSTOPS       8
#define DEBOUNCE_US        5000   /* 5ms debounce */

typedef struct {
    int8_t pin;
    bool triggered;          /* Current debounced state */
    bool raw_state;          /* Last raw GPIO read */
    int64_t last_change_us;  /* Timestamp of last state change */
    bool active;
} endstop_t;

typedef struct {
    bool initialized;
    endstop_t endstops[MAX_ENDSTOPS];
    uint8_t endstop_count;
    TaskHandle_t poll_task;
} sensor_endstop_ctx_t;

static sensor_endstop_ctx_t s_ctx;
static sb_plugin_t s_plugin;

/**
 * ISR handler — queues the pin number for processing.
 * We use a polling approach with ISR notification for simplicity.
 */
static QueueHandle_t s_event_queue;

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t pin = (uint32_t)(uintptr_t)arg;
    xQueueSendFromISR(s_event_queue, &pin, NULL);
}

/**
 * Process endstop state changes with debounce.
 */
static void endstop_poll_task(void *arg)
{
    (void)arg;
    uint32_t pin;

    while (1) {
        /* Wait for ISR event or poll every 50ms */
        if (xQueueReceive(s_event_queue, &pin, pdMS_TO_TICKS(50)) == pdTRUE) {
            /* Find the endstop for this pin */
            for (uint8_t i = 0; i < s_ctx.endstop_count; i++) {
                if (!s_ctx.endstops[i].active || s_ctx.endstops[i].pin != (int8_t)pin) {
                    continue;
                }

                int64_t now = esp_timer_get_time();
                bool new_state = (gpio_get_level((gpio_num_t)pin) == 0); /* Active low */

                /* Debounce: only accept if enough time has passed */
                if ((now - s_ctx.endstops[i].last_change_us) >= DEBOUNCE_US) {
                    if (new_state != s_ctx.endstops[i].triggered) {
                        s_ctx.endstops[i].triggered = new_state;
                        s_ctx.endstops[i].last_change_us = now;

                        /* Publish event */
                        char ev[96];
                        snprintf(ev, sizeof(ev),
                                 "{\"endstop\":%u,\"pin\":%lu,\"triggered\":%s}",
                                 (unsigned)i, (unsigned long)pin, new_state ? "true" : "false");
                        sb_event_publish("sensor.endstop", ev);

                        ESP_LOGD(TAG, "Endstop %u (pin %lu): %s",
                                 (unsigned)i, (unsigned long)pin, new_state ? "TRIGGERED" : "released");
                    }
                }
                break;
            }
        }

        /* Periodic poll as backup (in case ISR was missed) */
        for (uint8_t i = 0; i < s_ctx.endstop_count; i++) {
            if (!s_ctx.endstops[i].active) continue;
            bool current = (gpio_get_level((gpio_num_t)s_ctx.endstops[i].pin) == 0);
            int64_t now = esp_timer_get_time();

            if (current != s_ctx.endstops[i].triggered &&
                (now - s_ctx.endstops[i].last_change_us) >= DEBOUNCE_US) {
                s_ctx.endstops[i].triggered = current;
                s_ctx.endstops[i].last_change_us = now;

                char ev[96];
                snprintf(ev, sizeof(ev),
                         "{\"endstop\":%d,\"pin\":%d,\"triggered\":%s}",
                         i, s_ctx.endstops[i].pin, current ? "true" : "false");
                sb_event_publish("sensor.endstop", ev);
            }
        }
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

    uint8_t count = app_cfg->pins.endstop_count;
    if (count == 0) {
        ESP_LOGW(TAG, "No endstop pins configured");
        s_ctx.initialized = true;
        return ESP_OK;
    }
    if (count > MAX_ENDSTOPS) count = MAX_ENDSTOPS;

    /* Create event queue for ISR */
    s_event_queue = xQueueCreate(16, sizeof(uint32_t));
    if (s_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }

    /* Install GPIO ISR service (shared across all endstop pins) */
    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        /* ESP_ERR_INVALID_STATE means already installed — that's fine */
        ESP_LOGE(TAG, "GPIO ISR service install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    for (uint8_t i = 0; i < count; i++) {
        int8_t pin = app_cfg->pins.endstop_pins[i];
        if (pin < 0) continue;

        gpio_config_t io_cfg = {
            .pin_bit_mask = (1ULL << pin),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_ANYEDGE,
        };
        gpio_config(&io_cfg);
        gpio_isr_handler_add((gpio_num_t)pin, gpio_isr_handler,
                             (void *)(uintptr_t)pin);

        s_ctx.endstops[i].pin = pin;
        s_ctx.endstops[i].triggered = (gpio_get_level((gpio_num_t)pin) == 0);
        s_ctx.endstops[i].last_change_us = esp_timer_get_time();
        s_ctx.endstops[i].active = true;
        s_ctx.endstop_count++;
    }

    /* Start processing task */
    BaseType_t xret = xTaskCreate(endstop_poll_task, "endstop_poll", 2048, NULL, 6, &s_ctx.poll_task);
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create endstop poll task");
        return ESP_FAIL;
    }

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "Endstop sensor initialized: %d endstops", s_ctx.endstop_count);
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
        for (uint8_t i = 0; i < s_ctx.endstop_count; i++) {
            if (s_ctx.endstops[i].active) {
                gpio_isr_handler_remove((gpio_num_t)s_ctx.endstops[i].pin);
            }
        }
        if (s_event_queue != NULL) {
            vQueueDelete(s_event_queue);
            s_event_queue = NULL;
        }
        s_ctx.initialized = false;
        ESP_LOGI(TAG, "Endstop sensor shut down");
    }
    return ESP_OK;
}

static cJSON *plugin_get_state(sb_plugin_t *self)
{
    (void)self;
    cJSON *state = cJSON_CreateObject();
    cJSON *endstops = cJSON_AddArrayToObject(state, "endstops");

    for (uint8_t i = 0; i < s_ctx.endstop_count; i++) {
        if (!s_ctx.endstops[i].active) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "index", i);
        cJSON_AddNumberToObject(e, "pin", s_ctx.endstops[i].pin);
        cJSON_AddBoolToObject(e, "triggered", s_ctx.endstops[i].triggered);
        cJSON_AddItemToArray(endstops, e);
    }
    return state;
}

static cJSON *plugin_handle_command(sb_plugin_t *self, const char *cmd, const cJSON *params)
{
    (void)self;
    (void)params;
    cJSON *result = cJSON_CreateObject();

    if (strcmp(cmd, "get_reading") == 0) {
        cJSON_Delete(result);
        return plugin_get_state(self);

    } else if (strcmp(cmd, "get_state") == 0) {
        const cJSON *idx_j = cJSON_GetObjectItem(params, "index");
        if (idx_j && cJSON_IsNumber(idx_j)) {
            int idx = idx_j->valueint;
            if (idx >= 0 && idx < (int)s_ctx.endstop_count && s_ctx.endstops[idx].active) {
                cJSON_AddNumberToObject(result, "index", idx);
                cJSON_AddNumberToObject(result, "pin", s_ctx.endstops[idx].pin);
                cJSON_AddBoolToObject(result, "triggered", s_ctx.endstops[idx].triggered);
            } else {
                cJSON_AddStringToObject(result, "error", "Invalid endstop index");
            }
        } else {
            cJSON_Delete(result);
            return plugin_get_state(self);
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
    } else {
        hs.state = SB_HEALTH_HEALTHY;
        uint8_t triggered = 0;
        for (uint8_t i = 0; i < s_ctx.endstop_count; i++) {
            if (s_ctx.endstops[i].active && s_ctx.endstops[i].triggered) {
                triggered++;
            }
        }
        snprintf(hs.message, sizeof(hs.message), "%d endstops, %d triggered",
                 s_ctx.endstop_count, triggered);
    }
    return hs;
}

sb_plugin_t *sb_sensor_endstop_plugin(void)
{
    s_plugin.name           = "sensor-endstop";
    s_plugin.version        = "1.0.0";
    s_plugin.initialize     = plugin_init;
    s_plugin.shutdown       = plugin_shutdown;
    s_plugin.get_state      = plugin_get_state;
    s_plugin.handle_command = plugin_handle_command;
    s_plugin.health_check   = plugin_health_check;
    s_plugin.ctx            = &s_ctx;
    return &s_plugin;
}
