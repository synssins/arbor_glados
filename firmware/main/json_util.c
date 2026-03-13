/**
 * @file json_util.c
 * @brief JSON utility helpers for API responses.
 *
 * Task: F01
 */

#include "json_util.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG __attribute__((unused)) = "sb_json";

#ifndef CONFIG_SB_JSON_BUFFER_SIZE
#define CONFIG_SB_JSON_BUFFER_SIZE 4096
#endif

esp_err_t sb_json_respond(httpd_req_t *req, const char *status, const cJSON *json)
{
    char *body = cJSON_PrintUnformatted(json);
    if (body == NULL) {
        return sb_json_error(req, "500 Internal Server Error", "JSON serialization failed");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status);
    esp_err_t ret = httpd_resp_send(req, body, strlen(body));
    cJSON_free(body);
    return ret;
}

esp_err_t sb_json_error(httpd_req_t *req, const char *status, const char *detail)
{
    cJSON *err = cJSON_CreateObject();
    if (err == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"detail\":\"OOM\"}", 16);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(err, "detail", detail);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status);

    char *body = cJSON_PrintUnformatted(err);
    esp_err_t ret = ESP_FAIL;
    if (body != NULL) {
        ret = httpd_resp_send(req, body, strlen(body));
        cJSON_free(body);
    }
    cJSON_Delete(err);
    return ret;
}

cJSON *sb_json_parse_body(httpd_req_t *req)
{
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > CONFIG_SB_JSON_BUFFER_SIZE) {
        sb_json_error(req, "422 Unprocessable Entity", "Invalid body size");
        return NULL;
    }

    char *buf = malloc(content_len + 1);
    if (buf == NULL) {
        sb_json_error(req, "500 Internal Server Error", "Out of memory");
        return NULL;
    }

    int received = httpd_req_recv(req, buf, content_len);
    if (received != content_len) {
        free(buf);
        sb_json_error(req, "400 Bad Request", "Incomplete body");
        return NULL;
    }
    buf[content_len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);

    if (json == NULL) {
        sb_json_error(req, "422 Unprocessable Entity", "Invalid JSON");
        return NULL;
    }

    return json;
}
