/**
 * @file api_emergency.c
 * @brief Emergency stop endpoint — immediate all-servo torque disable.
 *
 * This endpoint is EXEMPT from rate limiting and authentication.
 * Must complete within 100ms per project plan.
 *
 * Task: F14
 */

#include "api_emergency.h"
#include "json_util.h"
#include "plugin_manager.h"
#include "event_bus.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "sb_api_estop";

/* POST /api/v1/emergency-stop */
static esp_err_t handle_emergency_stop(httpd_req_t *req)
{
    int64_t start = esp_timer_get_time();
    ESP_LOGW(TAG, "*** EMERGENCY STOP TRIGGERED ***");

    bool any_stopped = false;
    cJSON *results = cJSON_CreateArray();

    /* Iterate ALL plugins that might have servos and send emergency_stop */
    uint8_t plugin_count = sb_plugin_count();
    for (uint8_t i = 0; i < plugin_count; i++) {
        sb_plugin_t *p = sb_plugin_get(i);
        if (p == NULL || p->handle_command == NULL) continue;
        if (!sb_plugin_is_initialized(p->name)) continue;

        /* Try emergency_stop command on every plugin — servo plugins handle it,
         * others will return error/null which we ignore. */
        cJSON *params = cJSON_CreateObject();
        cJSON_AddBoolToObject(params, "all", true);
        cJSON *result = p->handle_command(p, "emergency_stop", params);
        cJSON_Delete(params);

        if (result != NULL) {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "plugin", p->name);

            const cJSON *stopped = cJSON_GetObjectItem(result, "stopped");
            if (stopped != NULL && cJSON_IsTrue(stopped)) {
                any_stopped = true;
                cJSON_AddBoolToObject(entry, "stopped", true);
            } else {
                cJSON_AddBoolToObject(entry, "stopped", false);
            }

            cJSON_AddItemToArray(results, entry);
            cJSON_Delete(result);
        }
    }

    int64_t elapsed_us = esp_timer_get_time() - start;

    /* Publish event regardless of outcome */
    char ev[128];
    snprintf(ev, sizeof(ev),
             "{\"stopped\":%s,\"elapsed_us\":%lld}",
             any_stopped ? "true" : "false",
             (long long)elapsed_us);
    sb_event_publish("emergency-stop.command", ev);

    /* Build response */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "stopped", any_stopped || plugin_count == 0);
    cJSON_AddNumberToObject(resp, "elapsed_us", (double)elapsed_us);
    cJSON_AddItemToObject(resp, "plugins", results);

    if (elapsed_us > 100000) { /* > 100ms */
        ESP_LOGW(TAG, "Emergency stop took %lld us (> 100ms deadline!)",
                 (long long)elapsed_us);
        cJSON_AddBoolToObject(resp, "deadline_met", false);
    } else {
        cJSON_AddBoolToObject(resp, "deadline_met", true);
    }

    ESP_LOGW(TAG, "Emergency stop complete in %lld us", (long long)elapsed_us);

    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

esp_err_t sb_api_emergency_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t uri = {
        .uri = "/api/v1/emergency-stop",
        .method = HTTP_POST,
        .handler = handle_emergency_stop,
    };

    esp_err_t ret = httpd_register_uri_handler(server, &uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register emergency-stop: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Emergency stop endpoint registered (NO rate limit, NO auth)");
    return ESP_OK;
}
