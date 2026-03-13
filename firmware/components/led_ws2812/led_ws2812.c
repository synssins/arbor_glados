/**
 * @file led_ws2812.c
 * @brief WS2812b LED driver using ESP-IDF 5.3 RMT TX API.
 *
 * Drives WS2812b addressable LEDs on the Waveshare Servo Driver with ESP32.
 * Uses rmt_new_tx_channel() and rmt_new_bytes_encoder() with custom
 * WS2812b timing (GRB color order, 24 bits per LED).
 *
 * Task: F-LED
 */

#include "led_ws2812.h"
#include "plugin_manager.h"
#include "app_config.h"

#include <string.h>
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

static const char *TAG = "led_ws2812";

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

#define MAX_WS2812_LEDS    10

/*
 * WS2812b timing at 10 MHz RMT resolution (100 ns per tick):
 *   T0H = 400 ns  ->  4 ticks high
 *   T0L = 850 ns  ->  9 ticks low
 *   T1H = 800 ns  ->  8 ticks high
 *   T1L = 450 ns  ->  5 ticks low
 *   Reset >= 50 us -> 500 ticks (handled by encoder reset code)
 */
#define RMT_RESOLUTION_HZ  10000000   /* 10 MHz */

#define WS2812_T0H_TICKS   4
#define WS2812_T0L_TICKS   9
#define WS2812_T1H_TICKS   8
#define WS2812_T1L_TICKS   5
#define WS2812_RESET_TICKS 500        /* 50 us */

#define IDENTIFY_TASK_STACK  2048
#define IDENTIFY_TASK_PRIO   5
#define IDENTIFY_HUE_STEP    5
#define IDENTIFY_DELAY_MS    50

/* --------------------------------------------------------------------------
 * LED strip encoder (bytes encoder wrapping approach)
 *
 * Each byte is encoded as 8 RMT symbols by a bytes_encoder using the
 * bit0/bit1 symbol definitions.  A copy encoder appends the reset signal.
 * -------------------------------------------------------------------------- */

typedef struct {
    rmt_encoder_t       base;
    rmt_encoder_t      *bytes_encoder;
    rmt_encoder_t      *copy_encoder;
    int                 state;        /* 0 = encoding data, 1 = encoding reset */
    rmt_symbol_word_t   reset_code;
} led_strip_encoder_t;

static size_t led_strip_encode(rmt_encoder_t *encoder,
                               rmt_channel_handle_t channel,
                               const void *primary_data,
                               size_t data_size,
                               rmt_encode_state_t *ret_state)
{
    led_strip_encoder_t *enc = __containerof(encoder, led_strip_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    switch (enc->state) {
    case 0: { /* encode pixel data */
        rmt_encode_state_t st = RMT_ENCODING_RESET;
        encoded_symbols += enc->bytes_encoder->encode(
            enc->bytes_encoder, channel, primary_data, data_size, &st);
        if (st & RMT_ENCODING_COMPLETE) {
            enc->state = 1;
        }
        if (st & RMT_ENCODING_MEM_FULL) {
            session_state |= RMT_ENCODING_MEM_FULL;
            *ret_state = session_state;
            return encoded_symbols;
        }
    }
    /* fall through */
    case 1: { /* encode reset */
        rmt_encode_state_t st = RMT_ENCODING_RESET;
        encoded_symbols += enc->copy_encoder->encode(
            enc->copy_encoder, channel, &enc->reset_code,
            sizeof(enc->reset_code), &st);
        if (st & RMT_ENCODING_COMPLETE) {
            enc->state = 0;
            session_state |= RMT_ENCODING_COMPLETE;
        }
        if (st & RMT_ENCODING_MEM_FULL) {
            session_state |= RMT_ENCODING_MEM_FULL;
        }
    }
    break;
    }

    *ret_state = session_state;
    return encoded_symbols;
}

static esp_err_t led_strip_encoder_del(rmt_encoder_t *encoder)
{
    led_strip_encoder_t *enc = __containerof(encoder, led_strip_encoder_t, base);
    rmt_del_encoder(enc->bytes_encoder);
    rmt_del_encoder(enc->copy_encoder);
    free(enc);
    return ESP_OK;
}

static esp_err_t led_strip_encoder_reset(rmt_encoder_t *encoder)
{
    led_strip_encoder_t *enc = __containerof(encoder, led_strip_encoder_t, base);
    rmt_encoder_reset(enc->bytes_encoder);
    rmt_encoder_reset(enc->copy_encoder);
    enc->state = 0;
    return ESP_OK;
}

static esp_err_t new_led_strip_encoder(rmt_encoder_handle_t *ret_encoder)
{
    led_strip_encoder_t *enc = calloc(1, sizeof(led_strip_encoder_t));
    if (!enc) return ESP_ERR_NO_MEM;

    enc->base.encode = led_strip_encode;
    enc->base.del    = led_strip_encoder_del;
    enc->base.reset  = led_strip_encoder_reset;

    rmt_bytes_encoder_config_t bytes_cfg = {
        .bit0 = {
            .duration0 = WS2812_T0H_TICKS,
            .level0    = 1,
            .duration1 = WS2812_T0L_TICKS,
            .level1    = 0,
        },
        .bit1 = {
            .duration0 = WS2812_T1H_TICKS,
            .level0    = 1,
            .duration1 = WS2812_T1L_TICKS,
            .level1    = 0,
        },
        .flags.msb_first = 1,
    };

    esp_err_t ret = rmt_new_bytes_encoder(&bytes_cfg, &enc->bytes_encoder);
    if (ret != ESP_OK) {
        free(enc);
        return ret;
    }

    rmt_copy_encoder_config_t copy_cfg = {};
    ret = rmt_new_copy_encoder(&copy_cfg, &enc->copy_encoder);
    if (ret != ESP_OK) {
        rmt_del_encoder(enc->bytes_encoder);
        free(enc);
        return ret;
    }

    /* Reset symbol: low for >= 50 us */
    enc->reset_code = (rmt_symbol_word_t){
        .duration0 = WS2812_RESET_TICKS,
        .level0    = 0,
        .duration1 = WS2812_RESET_TICKS,
        .level1    = 0,
    };

    enc->state = 0;
    *ret_encoder = &enc->base;
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------------- */

typedef struct {
    bool                   initialized;
    rmt_channel_handle_t   rmt_channel;
    rmt_encoder_handle_t   encoder;
    uint8_t                pixels[MAX_WS2812_LEDS * 3]; /* GRB order */
    int                    led_count;
    int                    gpio_pin;
    /* Identify mode */
    bool                   identifying;
    TaskHandle_t           identify_task;
} led_ws2812_ctx_t;

static led_ws2812_ctx_t s_ctx;
static sb_plugin_t s_plugin;

/* --------------------------------------------------------------------------
 * HSV -> RGB conversion (for identify rainbow)
 * -------------------------------------------------------------------------- */

/**
 * Convert HSV to RGB.
 *
 * @param h Hue        (0-359)
 * @param s Saturation (0-255)
 * @param v Value      (0-255)
 * @param r Output red   (0-255)
 * @param g Output green (0-255)
 * @param b Output blue  (0-255)
 */
static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v,
                        uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (s == 0) {
        *r = *g = *b = v;
        return;
    }

    h = h % 360;
    uint8_t region    = h / 60;
    uint8_t remainder = (h - (region * 60)) * 255 / 60;

    uint8_t p = (uint8_t)((uint16_t)v * (255 - s) / 255);
    uint8_t q = (uint8_t)((uint16_t)v * (255 - ((uint16_t)s * remainder / 255)) / 255);
    uint8_t t = (uint8_t)((uint16_t)v * (255 - ((uint16_t)s * (255 - remainder) / 255)) / 255);

    switch (region) {
    case 0:  *r = v; *g = t; *b = p; break;
    case 1:  *r = q; *g = v; *b = p; break;
    case 2:  *r = p; *g = v; *b = t; break;
    case 3:  *r = p; *g = q; *b = v; break;
    case 4:  *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
    }
}

/* --------------------------------------------------------------------------
 * Identify task
 * -------------------------------------------------------------------------- */

static void identify_task_fn(void *arg)
{
    (void)arg;
    uint16_t hue = 0;

    while (s_ctx.identifying) {
        uint8_t r, g, b;
        hsv_to_rgb(hue, 255, 255, &r, &g, &b);

        /* Clear all pixels, set only LED 0 */
        memset(s_ctx.pixels, 0, sizeof(s_ctx.pixels));
        /* GRB order */
        s_ctx.pixels[0] = g;
        s_ctx.pixels[1] = r;
        s_ctx.pixels[2] = b;

        sb_led_show();

        hue = (hue + IDENTIFY_HUE_STEP) % 360;
        vTaskDelay(pdMS_TO_TICKS(IDENTIFY_DELAY_MS));
    }

    /* Turn off LED 0 when stopped */
    memset(s_ctx.pixels, 0, sizeof(s_ctx.pixels));
    sb_led_show();

    s_ctx.identify_task = NULL;
    vTaskDelete(NULL);
}

/* --------------------------------------------------------------------------
 * Public standalone API
 * -------------------------------------------------------------------------- */

esp_err_t sb_led_init(int gpio_pin, int led_count)
{
    if (s_ctx.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (led_count <= 0 || led_count > MAX_WS2812_LEDS) {
        ESP_LOGE(TAG, "Invalid LED count %d (max %d)", led_count, MAX_WS2812_LEDS);
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.led_count = led_count;
    s_ctx.gpio_pin  = gpio_pin;

    /* Create RMT TX channel */
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num           = gpio_pin,
        .clk_src            = RMT_CLK_SRC_DEFAULT,
        .resolution_hz      = RMT_RESOLUTION_HZ,
        .mem_block_symbols  = 64,
        .trans_queue_depth  = 4,
    };

    esp_err_t ret = rmt_new_tx_channel(&tx_cfg, &s_ctx.rmt_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RMT TX channel creation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Create LED strip encoder */
    ret = new_led_strip_encoder(&s_ctx.encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED strip encoder creation failed: %s", esp_err_to_name(ret));
        rmt_del_channel(s_ctx.rmt_channel);
        return ret;
    }

    /* Enable the channel */
    ret = rmt_enable(s_ctx.rmt_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RMT enable failed: %s", esp_err_to_name(ret));
        rmt_del_encoder(s_ctx.encoder);
        rmt_del_channel(s_ctx.rmt_channel);
        return ret;
    }

    /* All LEDs off initially */
    memset(s_ctx.pixels, 0, sizeof(s_ctx.pixels));
    sb_led_show();

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "WS2812b initialized: %d LEDs on GPIO %d", led_count, gpio_pin);
    return ESP_OK;
}

esp_err_t sb_led_set_pixel(int index, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (index < 0 || index >= s_ctx.led_count) return ESP_ERR_INVALID_ARG;

    /* GRB order */
    s_ctx.pixels[index * 3 + 0] = g;
    s_ctx.pixels[index * 3 + 1] = r;
    s_ctx.pixels[index * 3 + 2] = b;
    return ESP_OK;
}

esp_err_t sb_led_set_all(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;

    for (int i = 0; i < s_ctx.led_count; i++) {
        s_ctx.pixels[i * 3 + 0] = g;  /* GRB */
        s_ctx.pixels[i * 3 + 1] = r;
        s_ctx.pixels[i * 3 + 2] = b;
    }
    return ESP_OK;
}

esp_err_t sb_led_show(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,  /* no loop */
    };

    esp_err_t ret = rmt_transmit(s_ctx.rmt_channel, s_ctx.encoder,
                                  s_ctx.pixels, s_ctx.led_count * 3,
                                  &tx_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RMT transmit failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Wait for transmission to complete */
    ret = rmt_tx_wait_all_done(s_ctx.rmt_channel, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "RMT TX wait timeout: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t sb_led_off(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;

    memset(s_ctx.pixels, 0, sizeof(s_ctx.pixels));
    return sb_led_show();
}

esp_err_t sb_led_identify_start(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (s_ctx.identifying) return ESP_OK; /* already running */

    s_ctx.identifying = true;

    BaseType_t ret = xTaskCreate(identify_task_fn, "led_identify",
                                  IDENTIFY_TASK_STACK, NULL,
                                  IDENTIFY_TASK_PRIO, &s_ctx.identify_task);
    if (ret != pdPASS) {
        s_ctx.identifying = false;
        ESP_LOGE(TAG, "Failed to create identify task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Identify mode started");
    return ESP_OK;
}

esp_err_t sb_led_identify_stop(void)
{
    if (!s_ctx.identifying) return ESP_OK;

    s_ctx.identifying = false;

    /* Wait for the task to exit */
    int wait_count = 0;
    while (s_ctx.identify_task != NULL && wait_count < 20) {
        vTaskDelay(pdMS_TO_TICKS(IDENTIFY_DELAY_MS));
        wait_count++;
    }

    ESP_LOGI(TAG, "Identify mode stopped");
    return ESP_OK;
}

esp_err_t sb_led_flash_on(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;

    sb_led_set_all(255, 255, 255);
    return sb_led_show();
}

esp_err_t sb_led_flash_off(void)
{
    return sb_led_off();
}

bool sb_led_is_identifying(void)
{
    return s_ctx.identifying;
}

int sb_led_get_count(void)
{
    return s_ctx.led_count;
}

/* --------------------------------------------------------------------------
 * Plugin interface
 * -------------------------------------------------------------------------- */

static esp_err_t plugin_init(sb_plugin_t *self, const cJSON *config)
{
    (void)self;
    (void)config;

    const sb_config_t *app_cfg = sb_config_get();
    if (app_cfg == NULL) {
        ESP_LOGE(TAG, "No app config available");
        return ESP_ERR_INVALID_STATE;
    }

    int gpio_pin = app_cfg->pins.ws2812_data;
    if (gpio_pin < 0) {
        ESP_LOGW(TAG, "WS2812b data pin not configured (ws2812_data = -1)");
        return ESP_ERR_INVALID_STATE;
    }

    return sb_led_init(gpio_pin, MAX_WS2812_LEDS);
}

static esp_err_t plugin_shutdown(sb_plugin_t *self)
{
    (void)self;

    /* Stop identify if running */
    sb_led_identify_stop();

    /* Turn off all LEDs */
    if (s_ctx.initialized) {
        sb_led_off();

        rmt_disable(s_ctx.rmt_channel);
        rmt_del_encoder(s_ctx.encoder);
        rmt_del_channel(s_ctx.rmt_channel);

        s_ctx.initialized = false;
        ESP_LOGI(TAG, "WS2812b driver shut down");
    }
    return ESP_OK;
}

static cJSON *plugin_get_state(sb_plugin_t *self)
{
    (void)self;
    cJSON *state = cJSON_CreateObject();
    cJSON_AddBoolToObject(state, "initialized", s_ctx.initialized);
    cJSON_AddBoolToObject(state, "identifying", s_ctx.identifying);
    cJSON_AddNumberToObject(state, "led_count", s_ctx.led_count);
    cJSON_AddNumberToObject(state, "gpio_pin", s_ctx.gpio_pin);
    return state;
}

static cJSON *plugin_handle_command(sb_plugin_t *self, const char *cmd, const cJSON *params)
{
    (void)self;
    cJSON *result = cJSON_CreateObject();

    if (!s_ctx.initialized) {
        cJSON_AddStringToObject(result, "error", "LED driver not initialized");
        return result;
    }

    if (strcmp(cmd, "set_color") == 0) {
        const cJSON *idx_j = cJSON_GetObjectItem(params, "index");
        const cJSON *r_j   = cJSON_GetObjectItem(params, "r");
        const cJSON *g_j   = cJSON_GetObjectItem(params, "g");
        const cJSON *b_j   = cJSON_GetObjectItem(params, "b");

        if (idx_j && r_j && g_j && b_j &&
            cJSON_IsNumber(idx_j) && cJSON_IsNumber(r_j) &&
            cJSON_IsNumber(g_j) && cJSON_IsNumber(b_j)) {

            esp_err_t ret = sb_led_set_pixel(idx_j->valueint,
                                              (uint8_t)r_j->valueint,
                                              (uint8_t)g_j->valueint,
                                              (uint8_t)b_j->valueint);
            if (ret == ESP_OK) {
                ret = sb_led_show();
            }
            if (ret == ESP_OK) {
                cJSON_AddBoolToObject(result, "ok", true);
            } else {
                cJSON_AddStringToObject(result, "error", esp_err_to_name(ret));
            }
        } else {
            cJSON_AddStringToObject(result, "error",
                                    "Missing or invalid params: index, r, g, b");
        }

    } else if (strcmp(cmd, "set_all") == 0) {
        const cJSON *r_j = cJSON_GetObjectItem(params, "r");
        const cJSON *g_j = cJSON_GetObjectItem(params, "g");
        const cJSON *b_j = cJSON_GetObjectItem(params, "b");

        if (r_j && g_j && b_j &&
            cJSON_IsNumber(r_j) && cJSON_IsNumber(g_j) && cJSON_IsNumber(b_j)) {

            esp_err_t ret = sb_led_set_all((uint8_t)r_j->valueint,
                                            (uint8_t)g_j->valueint,
                                            (uint8_t)b_j->valueint);
            if (ret == ESP_OK) {
                ret = sb_led_show();
            }
            if (ret == ESP_OK) {
                cJSON_AddBoolToObject(result, "ok", true);
            } else {
                cJSON_AddStringToObject(result, "error", esp_err_to_name(ret));
            }
        } else {
            cJSON_AddStringToObject(result, "error",
                                    "Missing or invalid params: r, g, b");
        }

    } else if (strcmp(cmd, "identify") == 0) {
        const cJSON *enable_j = cJSON_GetObjectItem(params, "enable");
        bool enable = true;
        if (enable_j && cJSON_IsBool(enable_j)) {
            enable = cJSON_IsTrue(enable_j);
        }

        esp_err_t ret;
        if (enable) {
            ret = sb_led_identify_start();
        } else {
            ret = sb_led_identify_stop();
        }

        if (ret == ESP_OK) {
            cJSON_AddBoolToObject(result, "ok", true);
            cJSON_AddBoolToObject(result, "identifying", s_ctx.identifying);
        } else {
            cJSON_AddStringToObject(result, "error", esp_err_to_name(ret));
        }

    } else if (strcmp(cmd, "flash") == 0) {
        const cJSON *on_j = cJSON_GetObjectItem(params, "on");
        bool on = true;
        if (on_j && cJSON_IsBool(on_j)) {
            on = cJSON_IsTrue(on_j);
        }

        esp_err_t ret = on ? sb_led_flash_on() : sb_led_flash_off();
        if (ret == ESP_OK) {
            cJSON_AddBoolToObject(result, "ok", true);
        } else {
            cJSON_AddStringToObject(result, "error", esp_err_to_name(ret));
        }

    } else if (strcmp(cmd, "off") == 0) {
        esp_err_t ret = sb_led_off();
        if (ret == ESP_OK) {
            cJSON_AddBoolToObject(result, "ok", true);
        } else {
            cJSON_AddStringToObject(result, "error", esp_err_to_name(ret));
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
        snprintf(hs.message, sizeof(hs.message),
                 "%d WS2812b LEDs on GPIO %d%s",
                 s_ctx.led_count, s_ctx.gpio_pin,
                 s_ctx.identifying ? " (identifying)" : "");
    }
    return hs;
}

sb_plugin_t *sb_led_ws2812_plugin(void)
{
    s_plugin.name           = "led-ws2812";
    s_plugin.version        = "1.0.0";
    s_plugin.initialize     = plugin_init;
    s_plugin.shutdown       = plugin_shutdown;
    s_plugin.get_state      = plugin_get_state;
    s_plugin.handle_command = plugin_handle_command;
    s_plugin.health_check   = plugin_health_check;
    s_plugin.ctx            = &s_ctx;
    return &s_plugin;
}
