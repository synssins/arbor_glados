/**
 * @file api_servo.c
 * @brief Servo API endpoints — state, position, speed, torque, sync, scan.
 *
 * Dispatches to the servo-bus plugin via plugin_manager.
 * Parity with control server C11 endpoints.
 *
 * Task: F12
 */

#include "api_servo.h"
#include "api_auth.h"
#include "api_cors.h"
#include "json_util.h"
#include "plugin_manager.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "sb_api_servo";

/**
 * Extract servo ID from a URI like /api/v1/servo/5/state.
 * Returns -1 on parse failure.
 */
static int extract_servo_id(const char *uri)
{
    /* Find the segment after "/api/v1/servo/" */
    const char *prefix = "/api/v1/servo/";
    const char *p = strstr(uri, prefix);
    if (p == NULL) return -1;
    p += strlen(prefix);

    char *end = NULL;
    long id = strtol(p, &end, 10);
    if (end == p || id < 0 || id > 253) return -1;
    return (int)id;
}

/**
 * Extract the action from a URI like /api/v1/servo/5/state → "state".
 */
static const char *extract_action(const char *uri)
{
    /* Find last '/' */
    const char *last = strrchr(uri, '/');
    if (last == NULL) return NULL;
    return last + 1;
}

/* GET /api/v1/servo/{id}/state */
static esp_err_t handle_servo_state(httpd_req_t *req, int servo_id)
{
    cJSON *params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "id", servo_id);
    cJSON *result = sb_plugin_dispatch("servo-bus", "get_state", params);
    cJSON_Delete(params);

    if (result == NULL) {
        return sb_json_error(req, "502 Bad Gateway", "Servo bus not available");
    }

    esp_err_t ret = sb_json_respond(req, "200 OK", result);
    cJSON_Delete(result);
    return ret;
}

/* PUT /api/v1/servo/{id}/position
 *
 * Body: { "position": 2048 }                     — move at max speed
 *   or: { "position": 2048, "speed": 500 }       — move with speed limit
 *   or: { "position": 2048, "time": 1000 }       — arrive in 1000ms
 *   or: { "position": 2048, "time": 1000, "speed": 500 }  — both
 *
 * When speed or time is provided, the firmware writes registers 42-47
 * (position + time + speed) atomically in one bus transaction.
 */
static esp_err_t handle_servo_position(httpd_req_t *req, int servo_id)
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

    const cJSON *position = cJSON_GetObjectItem(body, "position");
    if (!position || !cJSON_IsNumber(position)) {
        cJSON_Delete(body);
        return sb_json_error(req, "400 Bad Request", "Missing position field");
    }

    /* Extract required + optional fields into locals before freeing body */
    int pos_val = position->valueint;

    const cJSON *speed_j = cJSON_GetObjectItem(body, "speed");
    const cJSON *time_j  = cJSON_GetObjectItem(body, "time");
    int speed_val = (speed_j && cJSON_IsNumber(speed_j)) ? speed_j->valueint : 0;
    int time_val  = (time_j && cJSON_IsNumber(time_j))   ? time_j->valueint  : 0;
    cJSON_Delete(body);

    cJSON *params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "id", servo_id);
    cJSON_AddNumberToObject(params, "position", pos_val);
    if (speed_val > 0) {
        cJSON_AddNumberToObject(params, "speed", speed_val);
    }
    if (time_val > 0) {
        cJSON_AddNumberToObject(params, "time", time_val);
    }

    cJSON *result = sb_plugin_dispatch("servo-bus", "set_position", params);
    cJSON_Delete(params);

    if (result == NULL) {
        return sb_json_error(req, "502 Bad Gateway", "Servo bus not available");
    }

    esp_err_t ret = sb_json_respond(req, "200 OK", result);
    cJSON_Delete(result);
    return ret;
}

/* PUT /api/v1/servo/{id}/speed */
static esp_err_t handle_servo_speed(httpd_req_t *req, int servo_id)
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

    const cJSON *speed = cJSON_GetObjectItem(body, "speed");
    if (!speed || !cJSON_IsNumber(speed)) {
        cJSON_Delete(body);
        return sb_json_error(req, "400 Bad Request", "Missing speed field");
    }

    cJSON *params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "id", servo_id);
    cJSON_AddNumberToObject(params, "speed", speed->valueint);
    cJSON_Delete(body);

    cJSON *result = sb_plugin_dispatch("servo-bus", "set_speed", params);
    cJSON_Delete(params);

    if (result == NULL) {
        return sb_json_error(req, "502 Bad Gateway", "Servo bus not available");
    }

    esp_err_t ret = sb_json_respond(req, "200 OK", result);
    cJSON_Delete(result);
    return ret;
}

/* PUT /api/v1/servo/{id}/torque */
static esp_err_t handle_servo_torque(httpd_req_t *req, int servo_id)
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

    const cJSON *enabled = cJSON_GetObjectItem(body, "enabled");
    if (!enabled || !cJSON_IsBool(enabled)) {
        cJSON_Delete(body);
        return sb_json_error(req, "400 Bad Request", "Missing enabled field");
    }

    cJSON *params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "id", servo_id);
    cJSON_AddBoolToObject(params, "enabled", cJSON_IsTrue(enabled));
    cJSON_Delete(body);

    cJSON *result = sb_plugin_dispatch("servo-bus", "set_torque", params);
    cJSON_Delete(params);

    if (result == NULL) {
        return sb_json_error(req, "502 Bad Gateway", "Servo bus not available");
    }

    esp_err_t ret = sb_json_respond(req, "200 OK", result);
    cJSON_Delete(result);
    return ret;
}

/* POST /api/v1/servo/sync */
static esp_err_t handle_servo_sync(httpd_req_t *req)
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

    cJSON *result = sb_plugin_dispatch("servo-bus", "sync_move", body);
    cJSON_Delete(body);

    if (result == NULL) {
        return sb_json_error(req, "502 Bad Gateway", "Servo bus not available");
    }

    esp_err_t ret = sb_json_respond(req, "200 OK", result);
    cJSON_Delete(result);
    return ret;
}

/* POST /api/v1/servo/set-id */
static esp_err_t handle_servo_set_id(httpd_req_t *req)
{
    cJSON *body = sb_json_parse_body(req);
    if (body == NULL) {
        return sb_json_error(req, "400 Bad Request", "Invalid JSON body");
    }

    const cJSON *cur_id = cJSON_GetObjectItem(body, "current_id");
    const cJSON *new_id = cJSON_GetObjectItem(body, "new_id");
    if (!cur_id || !new_id || !cJSON_IsNumber(cur_id) || !cJSON_IsNumber(new_id)) {
        cJSON_Delete(body);
        return sb_json_error(req, "400 Bad Request", "Missing current_id or new_id");
    }

    cJSON *result = sb_plugin_dispatch("servo-bus", "set_id", body);
    cJSON_Delete(body);

    if (result == NULL) {
        return sb_json_error(req, "502 Bad Gateway", "Servo bus not available");
    }

    esp_err_t ret = sb_json_respond(req, "200 OK", result);
    cJSON_Delete(result);
    return ret;
}

/* GET /api/v1/servo/{id}/registers — read raw registers in small chunks.
 * ST3215 servos can't reliably return 50+ bytes in one read.
 * Use 25-byte chunks (same as the backup function which works reliably).
 * Addresses 50-54 are a gap in the ST3215 memory map — filled with zeros. */
static esp_err_t handle_servo_read_reg(httpd_req_t *req, int servo_id)
{
    /* Read in 25-byte chunks: 0-24, 25-49, 55-70 */
    static const struct { uint8_t addr; uint8_t count; } chunks[] = {
        {  0, 25 },   /* EEPROM block 1 */
        { 25, 25 },   /* EEPROM block 2 */
        { 55, 16 },   /* SRAM: Lock through Current (skip gap 50-54) */
    };
    static const int NUM_CHUNKS = 3;

    uint8_t regbuf[71];
    memset(regbuf, 0, sizeof(regbuf));

    for (int c = 0; c < NUM_CHUNKS; c++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "id", servo_id);
        cJSON_AddNumberToObject(p, "addr", chunks[c].addr);
        cJSON_AddNumberToObject(p, "count", chunks[c].count);
        cJSON *r = sb_plugin_dispatch("servo-bus", "read_reg", p);
        cJSON_Delete(p);

        if (r == NULL) {
            return sb_json_error(req, "502 Bad Gateway", "Servo bus not available");
        }
        cJSON *d = cJSON_GetObjectItem(r, "data");
        if (!d || cJSON_GetArraySize(d) < chunks[c].count) {
            cJSON_Delete(r);
            char msg[64];
            snprintf(msg, sizeof(msg), "Register read failed at addr %d", chunks[c].addr);
            return sb_json_error(req, "502 Bad Gateway", msg);
        }
        for (int i = 0; i < chunks[c].count; i++) {
            regbuf[chunks[c].addr + i] = (uint8_t)cJSON_GetArrayItem(d, i)->valuedouble;
        }
        cJSON_Delete(r);
    }

    /* Build JSON array [0..70] */
    cJSON *result = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(result, "data");
    for (int i = 0; i < 71; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(regbuf[i]));
    }
    cJSON_AddBoolToObject(result, "ok", true);
    esp_err_t ret = sb_json_respond(req, "200 OK", result);
    cJSON_Delete(result);
    return ret;
}

/* PUT /api/v1/servo/{id}/register — write register(s) */
static esp_err_t handle_servo_write_reg(httpd_req_t *req, int servo_id)
{
    cJSON *body = sb_json_parse_body(req);
    if (body == NULL) {
        return sb_json_error(req, "400 Bad Request", "Invalid JSON body");
    }
    cJSON_AddNumberToObject(body, "id", servo_id);

    cJSON *result = sb_plugin_dispatch("servo-bus", "write_reg", body);
    cJSON_Delete(body);

    if (result == NULL) {
        return sb_json_error(req, "502 Bad Gateway", "Servo bus not available");
    }
    esp_err_t ret = sb_json_respond(req, "200 OK", result);
    cJSON_Delete(result);
    return ret;
}

/* POST /api/v1/servo/{id}/factory-reset — restore all EEPROM registers to ST3215 defaults */
static esp_err_t handle_servo_factory_reset(httpd_req_t *req, int servo_id)
{
    sb_auth_result_t auth;
    sb_auth_check(req, &auth);
    if (!sb_auth_has_scope(&auth, SB_SCOPE_WRITE)) {
        return sb_json_error(req, "404 Not Found", "Not found");
    }

    cJSON *params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "id", servo_id);
    cJSON *result = sb_plugin_dispatch("servo-bus", "factory_reset", params);
    cJSON_Delete(params);

    if (result == NULL) {
        return sb_json_error(req, "502 Bad Gateway", "Servo bus not available");
    }

    esp_err_t ret = sb_json_respond(req, "200 OK", result);
    cJSON_Delete(result);
    return ret;
}

/* GET /api/v1/servo/{id}/backup — read all EEPROM registers */
static esp_err_t handle_servo_backup(httpd_req_t *req, int servo_id)
{
    cJSON *params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "id", servo_id);
    cJSON *result = sb_plugin_dispatch("servo-bus", "backup", params);
    cJSON_Delete(params);

    if (result == NULL) {
        return sb_json_error(req, "502 Bad Gateway", "Servo bus not available");
    }

    esp_err_t ret = sb_json_respond(req, "200 OK", result);
    cJSON_Delete(result);
    return ret;
}

/* POST /api/v1/servo/{id}/restore — write EEPROM registers from backup */
static esp_err_t handle_servo_restore(httpd_req_t *req, int servo_id)
{
    cJSON *body = sb_json_parse_body(req);
    if (body == NULL) {
        return sb_json_error(req, "400 Bad Request", "Invalid JSON body");
    }

    cJSON_AddNumberToObject(body, "id", servo_id);

    cJSON *result = sb_plugin_dispatch("servo-bus", "restore", body);
    cJSON_Delete(body);

    if (result == NULL) {
        return sb_json_error(req, "502 Bad Gateway", "Servo bus not available");
    }

    esp_err_t ret = sb_json_respond(req, "200 OK", result);
    cJSON_Delete(result);
    return ret;
}

/* GET /api/v1/servo/scan */
static esp_err_t handle_servo_scan(httpd_req_t *req)
{
    sb_cors_set_headers(req);
    sb_auth_result_t auth;
    sb_auth_check(req, &auth);
    if (!sb_auth_has_scope(&auth, SB_SCOPE_READ)) {
        return sb_json_error(req, "404 Not Found", "Not found");
    }

    cJSON *result = sb_plugin_dispatch("servo-bus", "scan", NULL);
    if (result == NULL) {
        return sb_json_error(req, "502 Bad Gateway", "Servo bus not available");
    }

    esp_err_t ret = sb_json_respond(req, "200 OK", result);
    cJSON_Delete(result);
    return ret;
}

/**
 * Wildcard handler for /api/v1/servo/ paths.
 * Dispatches based on HTTP method and path suffix.
 */
static esp_err_t handle_servo_wildcard(httpd_req_t *req)
{
    int servo_id = extract_servo_id(req->uri);
    const char *action = extract_action(req->uri);

    if (servo_id < 0 || action == NULL) {
        return sb_json_error(req, "400 Bad Request", "Invalid servo URI");
    }

    if (req->method == HTTP_GET && strcmp(action, "state") == 0) {
        return handle_servo_state(req, servo_id);
    } else if (req->method == HTTP_GET && strcmp(action, "backup") == 0) {
        return handle_servo_backup(req, servo_id);
    } else if (req->method == HTTP_GET && strcmp(action, "registers") == 0) {
        return handle_servo_read_reg(req, servo_id);
    } else if (req->method == HTTP_PUT && strcmp(action, "position") == 0) {
        return handle_servo_position(req, servo_id);
    } else if (req->method == HTTP_PUT && strcmp(action, "speed") == 0) {
        return handle_servo_speed(req, servo_id);
    } else if (req->method == HTTP_PUT && strcmp(action, "torque") == 0) {
        return handle_servo_torque(req, servo_id);
    } else if (req->method == HTTP_PUT && strcmp(action, "register") == 0) {
        return handle_servo_write_reg(req, servo_id);
    } else if (req->method == HTTP_POST && strcmp(action, "restore") == 0) {
        return handle_servo_restore(req, servo_id);
    } else if (req->method == HTTP_POST && strcmp(action, "factory-reset") == 0) {
        return handle_servo_factory_reset(req, servo_id);
    }

    return sb_json_error(req, "404 Not Found", "Not found");
}

esp_err_t sb_api_servo_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Fixed-path endpoints first (more specific matches) */
    const httpd_uri_t scan_uri = {
        .uri = "/api/v1/servo/scan", .method = HTTP_GET, .handler = handle_servo_scan,
    };
    const httpd_uri_t sync_uri = {
        .uri = "/api/v1/servo/sync", .method = HTTP_POST, .handler = handle_servo_sync,
    };
    const httpd_uri_t setid_uri = {
        .uri = "/api/v1/servo/set-id", .method = HTTP_POST, .handler = handle_servo_set_id,
    };

    esp_err_t ret;

    ret = httpd_register_uri_handler(server, &scan_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register scan: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(server, &sync_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register sync: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = httpd_register_uri_handler(server, &setid_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register set-id: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Wildcard handler for /api/v1/servo/{id}/{action} — GET, PUT, POST */
    const httpd_uri_t wildcard_get = {
        .uri = "/api/v1/servo/*", .method = HTTP_GET, .handler = handle_servo_wildcard,
    };
    const httpd_uri_t wildcard_put = {
        .uri = "/api/v1/servo/*", .method = HTTP_PUT, .handler = handle_servo_wildcard,
    };
    const httpd_uri_t wildcard_post = {
        .uri = "/api/v1/servo/*", .method = HTTP_POST, .handler = handle_servo_wildcard,
    };

    ret = httpd_register_uri_handler(server, &wildcard_get);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Wildcard GET registration: %s", esp_err_to_name(ret));
    }

    ret = httpd_register_uri_handler(server, &wildcard_put);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Wildcard PUT registration: %s", esp_err_to_name(ret));
    }

    ret = httpd_register_uri_handler(server, &wildcard_post);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Wildcard POST registration: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Servo API endpoints registered");
    return ESP_OK;
}
