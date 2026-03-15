/**
 * @file api_led.c
 * @brief LED API endpoints — state, identify, flash, color, off.
 *
 * Controls WS2812B LEDs on the Waveshare Servo Driver board.
 * No auth required for Phase 1.
 *
 * Task: F11
 */

#include "api_led.h"
#include "json_util.h"
#include "led_ws2812.h"

#include <stdbool.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "sb_api_led";

/* GET /api/v1/led — Return current LED state */
static esp_err_t handle_led_get(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return sb_json_error(req, "500 Internal Server Error", "OOM");
    }

    cJSON_AddBoolToObject(resp, "identify", sb_led_is_identifying());
    cJSON_AddNumberToObject(resp, "count", sb_led_get_count());

    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

/* POST /api/v1/led/identify — Toggle identify mode */
static esp_err_t handle_led_identify(httpd_req_t *req)
{
    cJSON *body = sb_json_parse_body(req);
    if (body == NULL) {
        return sb_json_error(req, "400 Bad Request", "Invalid JSON body");
    }

    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(body, "enabled");
    if (!cJSON_IsBool(enabled)) {
        cJSON_Delete(body);
        return sb_json_error(req, "400 Bad Request", "Missing or invalid 'enabled' field (bool)");
    }

    /* Save value before freeing body — enabled is a child of body */
    const bool is_enabled = cJSON_IsTrue(enabled);
    cJSON_Delete(body);

    if (is_enabled) {
        sb_led_identify_start();
    } else {
        sb_led_identify_stop();
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return sb_json_error(req, "500 Internal Server Error", "OOM");
    }

    cJSON_AddBoolToObject(resp, "identify", sb_led_is_identifying());

    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

/* POST /api/v1/led/flash — Flash all LEDs on or off */
static esp_err_t handle_led_flash(httpd_req_t *req)
{
    cJSON *body = sb_json_parse_body(req);
    if (body == NULL) {
        return sb_json_error(req, "400 Bad Request", "Invalid JSON body");
    }

    cJSON *on = cJSON_GetObjectItemCaseSensitive(body, "on");
    if (!cJSON_IsBool(on)) {
        cJSON_Delete(body);
        return sb_json_error(req, "400 Bad Request", "Missing or invalid 'on' field (bool)");
    }

    /* Save value before freeing body — on is a child of body */
    const bool is_on = cJSON_IsTrue(on);

    if (is_on) {
        sb_led_flash_on();
    } else {
        sb_led_flash_off();
    }

    cJSON_Delete(body);

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return sb_json_error(req, "500 Internal Server Error", "OOM");
    }

    cJSON_AddStringToObject(resp, "detail", is_on ? "Flash on" : "Flash off");

    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

/* PUT /api/v1/led/color — Set LED color by index or all */
static esp_err_t handle_led_color(httpd_req_t *req)
{
    cJSON *body = sb_json_parse_body(req);
    if (body == NULL) {
        return sb_json_error(req, "400 Bad Request", "Invalid JSON body");
    }

    cJSON *r_val = cJSON_GetObjectItemCaseSensitive(body, "r");
    cJSON *g_val = cJSON_GetObjectItemCaseSensitive(body, "g");
    cJSON *b_val = cJSON_GetObjectItemCaseSensitive(body, "b");

    if (!cJSON_IsNumber(r_val) || !cJSON_IsNumber(g_val) || !cJSON_IsNumber(b_val)) {
        cJSON_Delete(body);
        return sb_json_error(req, "400 Bad Request", "Missing or invalid 'r', 'g', 'b' fields (int)");
    }

    uint8_t r = (uint8_t)r_val->valueint;
    uint8_t g = (uint8_t)g_val->valueint;
    uint8_t b = (uint8_t)b_val->valueint;

    cJSON *all = cJSON_GetObjectItemCaseSensitive(body, "all");
    cJSON *index = cJSON_GetObjectItemCaseSensitive(body, "index");

    if (cJSON_IsBool(all) && cJSON_IsTrue(all)) {
        sb_led_set_all(r, g, b);
        sb_led_show();
    } else if (cJSON_IsNumber(index)) {
        int idx = index->valueint;
        if (idx < 0 || idx >= (int)sb_led_get_count()) {
            cJSON_Delete(body);
            return sb_json_error(req, "400 Bad Request", "LED index out of range");
        }
        sb_led_set_pixel((uint8_t)idx, r, g, b);
        sb_led_show();
    } else {
        cJSON_Delete(body);
        return sb_json_error(req, "400 Bad Request", "Must provide 'index' (int) or 'all' (true)");
    }

    cJSON_Delete(body);

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return sb_json_error(req, "500 Internal Server Error", "OOM");
    }

    cJSON_AddStringToObject(resp, "detail", "Color set");

    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

/* POST /api/v1/led/off — Turn all LEDs off */
static esp_err_t handle_led_off(httpd_req_t *req)
{
    sb_led_off();

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return sb_json_error(req, "500 Internal Server Error", "OOM");
    }

    cJSON_AddStringToObject(resp, "detail", "All LEDs off");

    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

esp_err_t sb_api_led_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/api/v1/led",          .method = HTTP_GET,  .handler = handle_led_get },
        { .uri = "/api/v1/led/identify", .method = HTTP_POST, .handler = handle_led_identify },
        { .uri = "/api/v1/led/flash",    .method = HTTP_POST, .handler = handle_led_flash },
        { .uri = "/api/v1/led/color",    .method = HTTP_PUT,  .handler = handle_led_color },
        { .uri = "/api/v1/led/off",      .method = HTTP_POST, .handler = handle_led_off },
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t ret = httpd_register_uri_handler(server, &uris[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s: %s", uris[i].uri, esp_err_to_name(ret));
            return ret;
        }
    }

    ESP_LOGI(TAG, "LED API endpoints registered");
    return ESP_OK;
}
