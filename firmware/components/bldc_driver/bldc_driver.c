/**
 * @file bldc_driver.c
 * @brief BLDC motor driver plugin — init, shutdown, command dispatch.
 *
 * Implements the sb_plugin_t interface. Delegates hardware operations
 * to either bldc_gpio (GPIO+PWM mode) or bldc_modbus (RS485 mode)
 * through the bldc_safety FSM which enforces all safety interlocks.
 *
 * Commands:
 *   "set_speed"      { "motor": 0, "speed": 128 }
 *   "set_direction"  { "motor": 0, "direction": "cw"|"ccw" }
 *   "set_enable"     { "motor": 0, "enabled": true }
 *   "brake"          { "motor": 0 }
 *   "clear_fault"    { "motor": 0 }
 *   "emergency_stop" {}
 *   "tank_drive"     { "left_speed": 128, "right_speed": -100 }
 *
 * Task: BLDC Phase A
 */

#include "bldc_driver.h"
#include "bldc_gpio.h"
#include "bldc_modbus.h"
#include "bldc_safety.h"
#include "plugin_manager.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bldc-driver";

#define BLDC_PLUGIN_VERSION "0.1.0"

/* ---- Plugin context ---- */

typedef struct {
    sb_bldc_config_t config;
    TaskHandle_t     task_handle;  /**< Background poll/tick task */
    bool             running;
} bldc_ctx_t;

static bldc_ctx_t s_ctx;

/* ---- Background task ---- */

/**
 * BLDC monitor task — periodic safety tick + alarm polling.
 * Runs at the configured alarm_poll_ms interval.
 */
static void bldc_monitor_task(void *arg)
{
    bldc_ctx_t *ctx = (bldc_ctx_t *)arg;
    uint32_t poll_ms = ctx->config.alarm_poll_ms;
    if (poll_ms < 50) poll_ms = 50;
    if (poll_ms > 1000) poll_ms = 1000;

    ESP_LOGI(TAG, "Monitor task started (poll interval=%lums)", (unsigned long)poll_ms);

    while (ctx->running) {
        bldc_safety_tick();
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }

    ESP_LOGI(TAG, "Monitor task exiting");
    vTaskDelete(NULL);
}

/* ---- Plugin lifecycle ---- */

static esp_err_t bldc_initialize(sb_plugin_t *self, const cJSON *config)
{
    (void)config;  /* Config comes from NVS via app_config, not JSON param */

    bldc_ctx_t *ctx = (bldc_ctx_t *)self->ctx;

    /* TODO: Load config from NVS via sb_config_get()
     * For now, check if motor_count > 0 (meaning config was loaded) */
    if (ctx->config.motor_count == 0) {
        ESP_LOGW(TAG, "No motors configured — plugin idle");
        return ESP_OK;
    }

    /* Initialize safety FSM first (sets all motors to STOPPED) */
    esp_err_t ret = bldc_safety_init(&ctx->config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Safety FSM init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Initialize hardware sub-driver */
    if (ctx->config.mode == SB_BLDC_MODE_GPIO) {
        ret = bldc_gpio_init(&ctx->config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "GPIO sub-driver init failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "Initialized in GPIO+PWM mode");
    } else {
        ret = bldc_modbus_init(&ctx->config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Modbus sub-driver init failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "Initialized in RS485 Modbus mode");
    }

    /* Start background monitor task */
    ctx->running = true;
    BaseType_t xret = xTaskCreate(
        bldc_monitor_task,
        "bldc_mon",
        3072,
        ctx,
        5,  /* Priority — same as servo bus task */
        &ctx->task_handle
    );
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create monitor task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Plugin initialized: %d motor(s), mode=%s",
             ctx->config.motor_count,
             ctx->config.mode == SB_BLDC_MODE_GPIO ? "gpio" : "modbus");
    return ESP_OK;
}

static esp_err_t bldc_shutdown(sb_plugin_t *self)
{
    bldc_ctx_t *ctx = (bldc_ctx_t *)self->ctx;

    /* Stop monitor task */
    ctx->running = false;
    if (ctx->task_handle) {
        /* Give task time to exit */
        vTaskDelay(pdMS_TO_TICKS(200));
        ctx->task_handle = NULL;
    }

    /* Emergency stop all motors */
    bldc_safety_emergency_stop();

    /* Deinit hardware */
    if (ctx->config.mode == SB_BLDC_MODE_GPIO) {
        bldc_gpio_deinit();
    } else {
        bldc_modbus_deinit();
    }

    ESP_LOGI(TAG, "Plugin shut down");
    return ESP_OK;
}

/* ---- State query ---- */

static cJSON *bldc_get_state(sb_plugin_t *self)
{
    bldc_ctx_t *ctx = (bldc_ctx_t *)self->ctx;
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "mode",
        ctx->config.mode == SB_BLDC_MODE_GPIO ? "gpio" : "modbus");
    cJSON_AddNumberToObject(root, "motor_count", ctx->config.motor_count);

    cJSON *motors = cJSON_CreateArray();
    if (!motors) {
        cJSON_Delete(root);
        return NULL;
    }

    for (uint8_t i = 0; i < ctx->config.motor_count; i++) {
        sb_bldc_motor_state_t ms;
        if (bldc_safety_get_motor_state(i, &ms) != ESP_OK) continue;

        cJSON *mobj = cJSON_CreateObject();
        if (!mobj) continue;

        cJSON_AddNumberToObject(mobj, "id", i);
        cJSON_AddStringToObject(mobj, "name", ctx->config.motors[i].name);

        const char *state_str;
        switch (ms.state) {
            case SB_BLDC_STATE_STOPPED:   state_str = "stopped";   break;
            case SB_BLDC_STATE_RUNNING:   state_str = "running";   break;
            case SB_BLDC_STATE_BRAKING:   state_str = "braking";   break;
            case SB_BLDC_STATE_REVERSING: state_str = "reversing"; break;
            case SB_BLDC_STATE_ESTOP:     state_str = "estop";     break;
            case SB_BLDC_STATE_FAULT:     state_str = "fault";     break;
            default:                      state_str = "unknown";   break;
        }
        cJSON_AddStringToObject(mobj, "state", state_str);
        cJSON_AddStringToObject(mobj, "direction",
            ms.direction == SB_BLDC_DIR_CW ? "cw" : "ccw");
        cJSON_AddNumberToObject(mobj, "speed", ms.speed);
        cJSON_AddNumberToObject(mobj, "rpm", ms.rpm);
        cJSON_AddBoolToObject(mobj, "enabled", ms.enabled);
        cJSON_AddBoolToObject(mobj, "braking", ms.braking);
        cJSON_AddNumberToObject(mobj, "faults", ms.faults);

        cJSON_AddItemToArray(motors, mobj);
    }

    cJSON_AddItemToObject(root, "motors", motors);
    return root;
}

/* ---- Command dispatch ---- */

static cJSON *bldc_handle_command(sb_plugin_t *self, const char *cmd,
                                   const cJSON *params)
{
    (void)self;
    cJSON *result = cJSON_CreateObject();
    if (!result) return NULL;

    /* ---- emergency_stop ---- */
    if (strcmp(cmd, "emergency_stop") == 0) {
        esp_err_t ret = bldc_safety_emergency_stop();
        cJSON_AddBoolToObject(result, "ok", ret == ESP_OK);
        if (ret != ESP_OK) {
            cJSON_AddStringToObject(result, "error", esp_err_to_name(ret));
        }
        return result;
    }

    /* ---- tank_drive ---- */
    if (strcmp(cmd, "tank_drive") == 0) {
        const cJSON *left_j  = cJSON_GetObjectItem(params, "left_speed");
        const cJSON *right_j = cJSON_GetObjectItem(params, "right_speed");

        if (!left_j || !right_j ||
            !cJSON_IsNumber(left_j) || !cJSON_IsNumber(right_j)) {
            cJSON_AddStringToObject(result, "error",
                "Missing left_speed and/or right_speed");
            return result;
        }

        int left  = left_j->valueint;
        int right = right_j->valueint;

        /* Negative speed = reverse direction */
        sb_bldc_dir_t left_dir  = left >= 0  ? SB_BLDC_DIR_CW : SB_BLDC_DIR_CCW;
        sb_bldc_dir_t right_dir = right >= 0 ? SB_BLDC_DIR_CW : SB_BLDC_DIR_CCW;
        uint8_t left_speed  = (uint8_t)abs(left);
        uint8_t right_speed = (uint8_t)abs(right);

        /* Clamp to 255 */
        if (left_speed > 255)  left_speed  = 255;
        if (right_speed > 255) right_speed = 255;

        /* Motor 0 = left, Motor 1 = right */
        esp_err_t ret_l, ret_r;
        ret_l = bldc_safety_request_direction(0, left_dir);
        ret_r = bldc_safety_request_direction(1, right_dir);

        /* Speed can only be applied when direction change is complete.
         * If motors are already in the correct direction, set speed now. */
        if (bldc_safety_get_state(0) != SB_BLDC_STATE_REVERSING) {
            ret_l = bldc_safety_request_speed(0, left_speed);
        }
        if (bldc_safety_get_state(1) != SB_BLDC_STATE_REVERSING) {
            ret_r = bldc_safety_request_speed(1, right_speed);
        }

        cJSON_AddBoolToObject(result, "ok",
            ret_l == ESP_OK && ret_r == ESP_OK);
        cJSON_AddNumberToObject(result, "left_speed", left_speed);
        cJSON_AddNumberToObject(result, "right_speed", right_speed);
        return result;
    }

    /* ---- Per-motor commands: extract motor index ---- */
    const cJSON *motor_j = cJSON_GetObjectItem(params, "motor");
    if (!motor_j || !cJSON_IsNumber(motor_j)) {
        cJSON_AddStringToObject(result, "error", "Missing 'motor' index");
        return result;
    }
    uint8_t motor_idx = (uint8_t)motor_j->valueint;

    /* ---- set_speed ---- */
    if (strcmp(cmd, "set_speed") == 0) {
        const cJSON *speed_j = cJSON_GetObjectItem(params, "speed");
        if (!speed_j || !cJSON_IsNumber(speed_j)) {
            cJSON_AddStringToObject(result, "error", "Missing 'speed'");
            return result;
        }
        uint8_t speed = (uint8_t)speed_j->valueint;
        esp_err_t ret = bldc_safety_request_speed(motor_idx, speed);
        cJSON_AddBoolToObject(result, "ok", ret == ESP_OK);
        if (ret != ESP_OK) {
            cJSON_AddStringToObject(result, "error", esp_err_to_name(ret));
        }
        return result;
    }

    /* ---- set_direction ---- */
    if (strcmp(cmd, "set_direction") == 0) {
        const cJSON *dir_j = cJSON_GetObjectItem(params, "direction");
        if (!dir_j || !cJSON_IsString(dir_j)) {
            cJSON_AddStringToObject(result, "error", "Missing 'direction'");
            return result;
        }
        sb_bldc_dir_t dir;
        if (strcmp(dir_j->valuestring, "ccw") == 0) {
            dir = SB_BLDC_DIR_CCW;
        } else {
            dir = SB_BLDC_DIR_CW;
        }
        esp_err_t ret = bldc_safety_request_direction(motor_idx, dir);
        cJSON_AddBoolToObject(result, "ok", ret == ESP_OK);
        if (ret != ESP_OK) {
            cJSON_AddStringToObject(result, "error", esp_err_to_name(ret));
        }
        return result;
    }

    /* ---- set_enable ---- */
    if (strcmp(cmd, "set_enable") == 0) {
        const cJSON *en_j = cJSON_GetObjectItem(params, "enabled");
        if (!en_j || !cJSON_IsBool(en_j)) {
            cJSON_AddStringToObject(result, "error", "Missing 'enabled'");
            return result;
        }
        esp_err_t ret = bldc_safety_request_enable(motor_idx,
                                                     cJSON_IsTrue(en_j));
        cJSON_AddBoolToObject(result, "ok", ret == ESP_OK);
        if (ret != ESP_OK) {
            cJSON_AddStringToObject(result, "error", esp_err_to_name(ret));
        }
        return result;
    }

    /* ---- brake ---- */
    if (strcmp(cmd, "brake") == 0) {
        esp_err_t ret = bldc_safety_request_brake(motor_idx);
        cJSON_AddBoolToObject(result, "ok", ret == ESP_OK);
        if (ret != ESP_OK) {
            cJSON_AddStringToObject(result, "error", esp_err_to_name(ret));
        }
        return result;
    }

    /* ---- clear_fault ---- */
    if (strcmp(cmd, "clear_fault") == 0) {
        esp_err_t ret = bldc_safety_clear_fault(motor_idx);
        cJSON_AddBoolToObject(result, "ok", ret == ESP_OK);
        if (ret != ESP_OK) {
            cJSON_AddStringToObject(result, "error", esp_err_to_name(ret));
        }
        return result;
    }

    /* Unknown command */
    cJSON_AddStringToObject(result, "error", "Unknown command");
    ESP_LOGW(TAG, "Unknown command: %s", cmd);
    return result;
}

/* ---- Health check ---- */

static sb_health_status_t bldc_health_check(sb_plugin_t *self)
{
    bldc_ctx_t *ctx = (bldc_ctx_t *)self->ctx;
    sb_health_status_t status = {
        .state = SB_HEALTH_HEALTHY,
    };

    if (ctx->config.motor_count == 0) {
        status.state = SB_HEALTH_UNKNOWN;
        snprintf(status.message, sizeof(status.message), "No motors configured");
        return status;
    }

    /* Check for any faults or estops */
    bool has_fault = false;
    bool has_estop = false;
    for (uint8_t i = 0; i < ctx->config.motor_count; i++) {
        sb_bldc_state_t st = bldc_safety_get_state(i);
        if (st == SB_BLDC_STATE_FAULT) has_fault = true;
        if (st == SB_BLDC_STATE_ESTOP) has_estop = true;
    }

    if (has_fault) {
        status.state = SB_HEALTH_UNHEALTHY;
        snprintf(status.message, sizeof(status.message), "Motor fault active");
    } else if (has_estop) {
        status.state = SB_HEALTH_DEGRADED;
        snprintf(status.message, sizeof(status.message), "Emergency stop active");
    } else {
        snprintf(status.message, sizeof(status.message),
                 "%d motor(s) OK", ctx->config.motor_count);
    }

    return status;
}

/* ---- Plugin singleton ---- */

static sb_plugin_t s_plugin = {
    .name          = "bldc-driver",
    .version       = BLDC_PLUGIN_VERSION,
    .initialize    = bldc_initialize,
    .shutdown      = bldc_shutdown,
    .get_state     = bldc_get_state,
    .handle_command = bldc_handle_command,
    .health_check  = bldc_health_check,
    .ctx           = &s_ctx,
};

struct sb_plugin *sb_bldc_driver_plugin(void)
{
    return &s_plugin;
}
