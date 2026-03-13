/**
 * @file servo_pwm.c
 * @brief Standard PWM servo driver using ESP32 LEDC peripheral.
 *
 * Supports up to 8 PWM servos on configurable GPIO pins.
 * Pulse width: 500-2500us (configurable per servo).
 * PWM frequency: 50Hz (standard hobby servo).
 *
 * Task: F07
 */

#include "servo_pwm.h"
#include "plugin_manager.h"
#include "app_config.h"

#include <string.h>
#include "esp_log.h"
#include "driver/ledc.h"
#include "cJSON.h"

static const char *TAG = "servo_pwm";

#define MAX_PWM_SERVOS   8
#define PWM_FREQUENCY    50       /* 50 Hz = 20ms period */
#define PWM_TIMER        LEDC_TIMER_0
#define PWM_MODE         LEDC_LOW_SPEED_MODE
#define PWM_RESOLUTION   LEDC_TIMER_14_BIT  /* 16384 steps */
#define PWM_DUTY_MAX     16384

/* Pulse width in microseconds */
#define DEFAULT_MIN_PULSE_US   500
#define DEFAULT_MAX_PULSE_US   2500
#define PERIOD_US              20000  /* 20ms at 50Hz */

typedef struct {
    int8_t pin;
    uint16_t min_pulse_us;
    uint16_t max_pulse_us;
    uint16_t current_position;  /* 0-1000 (0.1% resolution) */
    bool active;
} pwm_servo_t;

typedef struct {
    bool initialized;
    pwm_servo_t servos[MAX_PWM_SERVOS];
    uint8_t servo_count;
} servo_pwm_ctx_t;

static servo_pwm_ctx_t s_ctx;
static sb_plugin_t s_plugin;

/**
 * Convert position (0-1000) to LEDC duty value.
 */
static uint32_t position_to_duty(const pwm_servo_t *servo, uint16_t position)
{
    if (position > 1000) position = 1000;

    uint32_t pulse_us = servo->min_pulse_us +
        ((uint32_t)(servo->max_pulse_us - servo->min_pulse_us) * position) / 1000;

    /* Convert pulse width to duty cycle */
    return (pulse_us * PWM_DUTY_MAX) / PERIOD_US;
}

static esp_err_t plugin_init(sb_plugin_t *self, const cJSON *config)
{
    (void)self;
    memset(&s_ctx, 0, sizeof(s_ctx));

    const sb_config_t *app_cfg = sb_config_get();
    if (app_cfg == NULL) {
        ESP_LOGE(TAG, "No app config");
        return ESP_ERR_INVALID_STATE;
    }

    /* Configure LEDC timer */
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = PWM_MODE,
        .duty_resolution = PWM_RESOLUTION,
        .timer_num       = PWM_TIMER,
        .freq_hz         = PWM_FREQUENCY,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Initialize each configured PWM pin */
    uint8_t count = app_cfg->pins.pwm_count;
    if (count > MAX_PWM_SERVOS) count = MAX_PWM_SERVOS;

    for (uint8_t i = 0; i < count; i++) {
        int8_t pin = app_cfg->pins.pwm_pins[i];
        if (pin < 0) continue;

        ledc_channel_config_t ch_cfg = {
            .speed_mode = PWM_MODE,
            .channel    = (ledc_channel_t)i,
            .timer_sel  = PWM_TIMER,
            .gpio_num   = pin,
            .duty       = 0,
            .hpoint     = 0,
        };

        ret = ledc_channel_config(&ch_cfg);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "LEDC channel %d (pin %d) failed: %s", i, pin, esp_err_to_name(ret));
            continue;
        }

        s_ctx.servos[i].pin = pin;
        s_ctx.servos[i].min_pulse_us = DEFAULT_MIN_PULSE_US;
        s_ctx.servos[i].max_pulse_us = DEFAULT_MAX_PULSE_US;
        s_ctx.servos[i].current_position = 500; /* Center */
        s_ctx.servos[i].active = true;
        s_ctx.servo_count++;

        /* Set initial position to center */
        uint32_t duty = position_to_duty(&s_ctx.servos[i], 500);
        ledc_set_duty(PWM_MODE, (ledc_channel_t)i, duty);
        ledc_update_duty(PWM_MODE, (ledc_channel_t)i);
    }

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "PWM servo driver initialized: %d servos", s_ctx.servo_count);
    return ESP_OK;
}

static esp_err_t plugin_shutdown(sb_plugin_t *self)
{
    (void)self;
    if (s_ctx.initialized) {
        for (uint8_t i = 0; i < MAX_PWM_SERVOS; i++) {
            if (s_ctx.servos[i].active) {
                ledc_set_duty(PWM_MODE, (ledc_channel_t)i, 0);
                ledc_update_duty(PWM_MODE, (ledc_channel_t)i);
            }
        }
        ledc_timer_pause(PWM_MODE, PWM_TIMER);
        s_ctx.initialized = false;
        ESP_LOGI(TAG, "PWM servo driver shut down");
    }
    return ESP_OK;
}

static cJSON *plugin_get_state(sb_plugin_t *self)
{
    (void)self;
    cJSON *state = cJSON_CreateObject();
    cJSON *servos = cJSON_AddArrayToObject(state, "servos");

    for (uint8_t i = 0; i < MAX_PWM_SERVOS; i++) {
        if (!s_ctx.servos[i].active) continue;
        cJSON *s = cJSON_CreateObject();
        cJSON_AddNumberToObject(s, "channel", i);
        cJSON_AddNumberToObject(s, "pin", s_ctx.servos[i].pin);
        cJSON_AddNumberToObject(s, "position", s_ctx.servos[i].current_position);
        cJSON_AddNumberToObject(s, "min_pulse_us", s_ctx.servos[i].min_pulse_us);
        cJSON_AddNumberToObject(s, "max_pulse_us", s_ctx.servos[i].max_pulse_us);
        cJSON_AddItemToArray(servos, s);
    }
    return state;
}

static cJSON *plugin_handle_command(sb_plugin_t *self, const char *cmd, const cJSON *params)
{
    (void)self;
    cJSON *result = cJSON_CreateObject();

    if (strcmp(cmd, "set_position") == 0) {
        const cJSON *ch_j = cJSON_GetObjectItem(params, "channel");
        const cJSON *pos_j = cJSON_GetObjectItem(params, "position");
        if (ch_j && pos_j && cJSON_IsNumber(ch_j) && cJSON_IsNumber(pos_j)) {
            int ch = ch_j->valueint;
            int pos = pos_j->valueint;
            if (ch >= 0 && ch < MAX_PWM_SERVOS && s_ctx.servos[ch].active) {
                if (pos < 0) pos = 0;
                if (pos > 1000) pos = 1000;
                uint32_t duty = position_to_duty(&s_ctx.servos[ch], (uint16_t)pos);
                ledc_set_duty(PWM_MODE, (ledc_channel_t)ch, duty);
                ledc_update_duty(PWM_MODE, (ledc_channel_t)ch);
                s_ctx.servos[ch].current_position = (uint16_t)pos;
                cJSON_AddBoolToObject(result, "ok", true);
            } else {
                cJSON_AddStringToObject(result, "error", "Invalid channel");
            }
        } else {
            cJSON_AddStringToObject(result, "error", "Missing channel or position");
        }

    } else if (strcmp(cmd, "get_state") == 0) {
        cJSON_Delete(result);
        return plugin_get_state(self);

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
        snprintf(hs.message, sizeof(hs.message), "%d PWM servos active", s_ctx.servo_count);
    }
    return hs;
}

sb_plugin_t *sb_servo_pwm_plugin(void)
{
    s_plugin.name           = "servo-pwm";
    s_plugin.version        = "1.0.0";
    s_plugin.initialize     = plugin_init;
    s_plugin.shutdown       = plugin_shutdown;
    s_plugin.get_state      = plugin_get_state;
    s_plugin.handle_command = plugin_handle_command;
    s_plugin.health_check   = plugin_health_check;
    s_plugin.ctx            = &s_ctx;
    return &s_plugin;
}
