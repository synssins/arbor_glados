/**
 * @file api_sensor.c
 * @brief Sensor API endpoints — reading, history, all sensors.
 *
 * Dispatches to sensor-temp and sensor-endstop plugins.
 * Parity with control server C12 endpoints.
 *
 * Task: F13
 */

#include "api_sensor.h"
#include "api_auth.h"
#include "json_util.h"
#include "plugin_manager.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "sb_api_sensor";

/**
 * Extract sensor ID from URI like /api/v1/sensor/temp-0/reading.
 * Returns a pointer into the URI (not a copy).
 */
static const char *extract_sensor_id(const char *uri, char *buf, size_t buf_size)
{
    const char *prefix = "/api/v1/sensor/";
    const char *p = strstr(uri, prefix);
    if (p == NULL) return NULL;
    p += strlen(prefix);

    const char *slash = strchr(p, '/');
    size_t len;
    if (slash != NULL) {
        len = (size_t)(slash - p);
    } else {
        len = strlen(p);
    }

    if (len == 0 || len >= buf_size) return NULL;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return buf;
}

/**
 * Map a sensor ID to a plugin name and index.
 * Convention: "temp-0" → plugin "sensor-temp", "endstop-2" → plugin "sensor-endstop"
 */
static const char *sensor_id_to_plugin(const char *sensor_id)
{
    if (strncmp(sensor_id, "temp", 4) == 0) {
        return "sensor-temp";
    } else if (strncmp(sensor_id, "endstop", 7) == 0) {
        return "sensor-endstop";
    }
    return NULL;
}

/* GET /api/v1/sensors — list all sensor readings */
static esp_err_t handle_all_sensors(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON *sensors = cJSON_AddArrayToObject(resp, "sensors");

    /* Query sensor-temp plugin */
    sb_plugin_t *temp = sb_plugin_find("sensor-temp");
    if (temp != NULL && temp->get_state != NULL && sb_plugin_is_initialized("sensor-temp")) {
        cJSON *state = temp->get_state(temp);
        if (state != NULL) {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "id", "temp-0");
            cJSON_AddStringToObject(entry, "type", "temperature");
            cJSON_AddItemToObject(entry, "state", state);
            cJSON_AddItemToArray(sensors, entry);
        }
    }

    /* Query sensor-endstop plugin */
    sb_plugin_t *endstop = sb_plugin_find("sensor-endstop");
    if (endstop != NULL && endstop->get_state != NULL && sb_plugin_is_initialized("sensor-endstop")) {
        cJSON *state = endstop->get_state(endstop);
        if (state != NULL) {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "id", "endstop-0");
            cJSON_AddStringToObject(entry, "type", "endstop");
            cJSON_AddItemToObject(entry, "state", state);
            cJSON_AddItemToArray(sensors, entry);
        }
    }

    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

/* GET /api/v1/sensor/{id}/reading */
static esp_err_t handle_sensor_reading(httpd_req_t *req)
{
    char sensor_id[32];
    if (extract_sensor_id(req->uri, sensor_id, sizeof(sensor_id)) == NULL) {
        return sb_json_error(req, "400 Bad Request", "Invalid sensor URI");
    }

    const char *plugin_name = sensor_id_to_plugin(sensor_id);
    if (plugin_name == NULL) {
        return sb_json_error(req, "404 Not Found", "Unknown sensor type");
    }

    cJSON *result = sb_plugin_dispatch(plugin_name, "get_reading", NULL);
    if (result == NULL) {
        return sb_json_error(req, "502 Bad Gateway", "Sensor plugin not available");
    }

    cJSON_AddStringToObject(result, "sensor_id", sensor_id);
    esp_err_t ret = sb_json_respond(req, "200 OK", result);
    cJSON_Delete(result);
    return ret;
}

/* GET /api/v1/sensor/{id}/history */
static esp_err_t handle_sensor_history(httpd_req_t *req)
{
    char sensor_id[32];
    if (extract_sensor_id(req->uri, sensor_id, sizeof(sensor_id)) == NULL) {
        return sb_json_error(req, "400 Bad Request", "Invalid sensor URI");
    }

    const char *plugin_name = sensor_id_to_plugin(sensor_id);
    if (plugin_name == NULL) {
        return sb_json_error(req, "404 Not Found", "Unknown sensor type");
    }

    /* Parse optional limit query parameter */
    cJSON *params = cJSON_CreateObject();
    const char *query = strchr(req->uri, '?');
    if (query != NULL) {
        const char *lim = strstr(query, "limit=");
        if (lim != NULL) {
            int limit = atoi(lim + 6);
            if (limit > 0) {
                cJSON_AddNumberToObject(params, "limit", limit);
            }
        }
    }

    cJSON *result = sb_plugin_dispatch(plugin_name, "get_history", params);
    cJSON_Delete(params);

    if (result == NULL) {
        return sb_json_error(req, "502 Bad Gateway", "Sensor plugin not available");
    }

    cJSON_AddStringToObject(result, "sensor_id", sensor_id);
    esp_err_t ret = sb_json_respond(req, "200 OK", result);
    cJSON_Delete(result);
    return ret;
}

/**
 * Wildcard handler for /api/v1/sensor/ paths.
 */
static esp_err_t handle_sensor_wildcard(httpd_req_t *req)
{
    const char *action = strrchr(req->uri, '/');
    if (action == NULL) {
        return sb_json_error(req, "404 Not Found", "Not found");
    }
    action++;

    /* Strip query string for comparison */
    char action_buf[32];
    const char *qmark = strchr(action, '?');
    if (qmark != NULL) {
        size_t len = (size_t)(qmark - action);
        if (len >= sizeof(action_buf)) len = sizeof(action_buf) - 1;
        memcpy(action_buf, action, len);
        action_buf[len] = '\0';
        action = action_buf;
    }

    if (strcmp(action, "reading") == 0) {
        return handle_sensor_reading(req);
    } else if (strcmp(action, "history") == 0) {
        return handle_sensor_history(req);
    }

    return sb_json_error(req, "404 Not Found", "Not found");
}

esp_err_t sb_api_sensor_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t all_sensors_uri = {
        .uri = "/api/v1/sensors", .method = HTTP_GET, .handler = handle_all_sensors,
    };

    esp_err_t ret = httpd_register_uri_handler(server, &all_sensors_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /sensors: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Wildcard handler for /api/v1/sensor/{id}/... */
    const httpd_uri_t sensor_wildcard = {
        .uri = "/api/v1/sensor/*", .method = HTTP_GET, .handler = handle_sensor_wildcard,
    };

    ret = httpd_register_uri_handler(server, &sensor_wildcard);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Wildcard sensor GET: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Sensor API endpoints registered");
    return ESP_OK;
}
