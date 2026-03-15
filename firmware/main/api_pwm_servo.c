/**
 * @file api_pwm_servo.c
 * @brief PWM Servo API endpoints — list, state, position.
 *
 * Dispatches to the servo-pwm plugin via plugin_manager.
 * Follows the same pattern as api_servo.c.
 *
 * Task: PWM Servo Support
 */

#include "api_pwm_servo.h"
#include "api_auth.h"
#include "api_cors.h"
#include "json_util.h"
#include "plugin_manager.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "sb_api_pwm_servo";

/**
 * Extract channel number from a URI like /api/v1/pwm-servo/3/state.
 * Returns -1 on parse failure.
 */
static int extract_channel(const char *uri)
{
    const char *prefix = "/api/v1/pwm-servo/";
    const char *p = strstr(uri, prefix);
    if (p == NULL) return -1;
    p += strlen(prefix);

    /* Skip "list" — it's not a channel number */
    if (strncmp(p, "list", 4) == 0) return -1;

    char *end = NULL;
    long ch = strtol(p, &end, 10);
    if (end == p || ch < 0 || ch > 7) return -1;
    return (int)ch;
}

/**
 * Extract the action from a URI like /api/v1/pwm-servo/3/state → "state".
 */
static const char *extract_action(const char *uri)
{
    const char *last = strrchr(uri, '/');
    if (last == NULL) return NULL;
    return last + 1;
}

/* GET /api/v1/pwm-servo/list — list all active PWM channels */
static esp_err_t handle_pwm_list(httpd_req_t *req)
{
    sb_cors_set_headers(req);
    sb_auth_result_t auth;
    sb_auth_check(req, &auth);
    if (!sb_auth_has_scope(&auth, SB_SCOPE_READ)) {
        return sb_json_error(req, "404 Not Found", "Not found");
    }

    cJSON *result = sb_plugin_dispatch("servo-pwm", "get_state", NULL);
    if (result == NULL) {
        return sb_json_error(req, "502 Bad Gateway", "PWM servo plugin not available");
    }

    esp_err_t ret = sb_json_respond(req, "200 OK", result);
    cJSON_Delete(result);
    return ret;
}

/* GET /api/v1/pwm-servo/{channel}/state */
static esp_err_t handle_pwm_state(httpd_req_t *req, int channel)
{
    cJSON *params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "channel", channel);
    cJSON *result = sb_plugin_dispatch("servo-pwm", "get_state", params);
    cJSON_Delete(params);

    if (result == NULL) {
        return sb_json_error(req, "502 Bad Gateway", "PWM servo plugin not available");
    }

    /* Extract just this channel's data from the servos array */
    cJSON *servos = cJSON_GetObjectItem(result, "servos");
    if (servos && cJSON_IsArray(servos)) {
        int count = cJSON_GetArraySize(servos);
        for (int i = 0; i < count; i++) {
            cJSON *s = cJSON_GetArrayItem(servos, i);
            cJSON *ch_j = cJSON_GetObjectItem(s, "channel");
            if (ch_j && ch_j->valueint == channel) {
                /* Return just this servo's state */
                cJSON *single = cJSON_Duplicate(s, true);
                cJSON_Delete(result);
                esp_err_t ret = sb_json_respond(req, "200 OK", single);
                cJSON_Delete(single);
                return ret;
            }
        }
    }

    cJSON_Delete(result);
    return sb_json_error(req, "404 Not Found", "Channel not found");
}

/* PUT /api/v1/pwm-servo/{channel}/position */
static esp_err_t handle_pwm_position(httpd_req_t *req, int channel)
{
    sb_auth_result_t auth;
    sb_auth_check(req, &auth);
    if (!sb_auth_has_scope(&auth, SB_SCOPE_WRITE)) {
        return sb_json_error(req, "404 Not Found", "Not found");
    }

    cJSON *body = sb_json_parse_body(req);
    if (body == NULL) {
        return sb_json_error(req, "400 Bad Request", "Invalid JSON body");
    }

    const cJSON *pos_j = cJSON_GetObjectItem(body, "position");
    if (!pos_j || !cJSON_IsNumber(pos_j)) {
        cJSON_Delete(body);
        return sb_json_error(req, "400 Bad Request", "Missing 'position' field");
    }

    /* Save value before freeing body — pos_j is a child of body */
    const int position = pos_j->valueint;
    cJSON_Delete(body);

    if (position < 0 || position > 1000) {
        return sb_json_error(req, "400 Bad Request",
                             "Position must be 0-1000");
    }

    cJSON *params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "channel", channel);
    cJSON_AddNumberToObject(params, "position", position);

    cJSON *result = sb_plugin_dispatch("servo-pwm", "set_position", params);
    cJSON_Delete(params);

    if (result == NULL) {
        return sb_json_error(req, "502 Bad Gateway", "PWM servo plugin not available");
    }

    cJSON_AddNumberToObject(result, "position", position);
    esp_err_t ret = sb_json_respond(req, "200 OK", result);
    cJSON_Delete(result);
    return ret;
}

/**
 * Wildcard handler for /api/v1/pwm-servo/ paths.
 */
static esp_err_t handle_pwm_wildcard(httpd_req_t *req)
{
    sb_cors_set_headers(req);

    int channel = extract_channel(req->uri);
    const char *action = extract_action(req->uri);

    if (channel < 0 || action == NULL) {
        return sb_json_error(req, "400 Bad Request", "Invalid PWM servo URI");
    }

    sb_auth_result_t auth;
    sb_auth_check(req, &auth);
    if (!sb_auth_has_scope(&auth, SB_SCOPE_READ)) {
        return sb_json_error(req, "404 Not Found", "Not found");
    }

    if (req->method == HTTP_GET && strcmp(action, "state") == 0) {
        return handle_pwm_state(req, channel);
    } else if (req->method == HTTP_PUT && strcmp(action, "position") == 0) {
        return handle_pwm_position(req, channel);
    }

    return sb_json_error(req, "404 Not Found", "Not found");
}

esp_err_t sb_api_pwm_servo_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Fixed-path endpoint: list */
    const httpd_uri_t list_uri = {
        .uri = "/api/v1/pwm-servo/list",
        .method = HTTP_GET,
        .handler = handle_pwm_list,
    };

    esp_err_t ret = httpd_register_uri_handler(server, &list_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register pwm-servo list: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Wildcard handlers for /api/v1/pwm-servo/{channel}/{action} */
    const httpd_uri_t wildcard_get = {
        .uri = "/api/v1/pwm-servo/*",
        .method = HTTP_GET,
        .handler = handle_pwm_wildcard,
    };
    const httpd_uri_t wildcard_put = {
        .uri = "/api/v1/pwm-servo/*",
        .method = HTTP_PUT,
        .handler = handle_pwm_wildcard,
    };

    ret = httpd_register_uri_handler(server, &wildcard_get);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PWM wildcard GET registration: %s", esp_err_to_name(ret));
    }

    ret = httpd_register_uri_handler(server, &wildcard_put);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PWM wildcard PUT registration: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "PWM Servo API endpoints registered");
    return ESP_OK;
}
