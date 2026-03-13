/**
 * @file api_system.c
 * @brief System API endpoints — info, health, config, restart, OTA status.
 *
 * Parity with control server C10 endpoints.
 * Includes F15 OTA partition status.
 *
 * Task: F11 (system endpoints), F15 (OTA status)
 */

#include "api_system.h"
#include "api_auth.h"
#include "json_util.h"
#include "plugin_manager.h"
#include "app_config.h"
#include "ws_server.h"

#include "wifi_manager.h"
#include "oled_display.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "esp_spiffs.h"
#include "cJSON.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>

static const char *TAG = "sb_api_sys";

/* GET /api/v1/system/info */
static esp_err_t handle_info(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return sb_json_error(req, "500 Internal Server Error", "OOM");
    }

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    const esp_app_desc_t *app = esp_app_get_description();

    cJSON_AddStringToObject(resp, "platform", "esp32");
    cJSON_AddStringToObject(resp, "firmware_version", app ? app->version : "0.1.0");
    cJSON_AddStringToObject(resp, "project_name", app ? app->project_name : "arbor-esp");
    cJSON_AddStringToObject(resp, "idf_version", app ? app->idf_ver : "unknown");
    cJSON_AddStringToObject(resp, "compile_date", app ? app->date : "unknown");
    cJSON_AddStringToObject(resp, "compile_time", app ? app->time : "unknown");
    cJSON_AddNumberToObject(resp, "cores", chip.cores);
    cJSON_AddNumberToObject(resp, "revision", chip.revision);
    cJSON_AddNumberToObject(resp, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(resp, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(resp, "plugin_count", sb_plugin_count());
    cJSON_AddNumberToObject(resp, "ws_clients", sb_ws_client_count());

    /* Device name and WiFi state */
    const sb_config_t *cfg = sb_config_get();
    if (cfg && cfg->server.hostname[0]) {
        cJSON_AddStringToObject(resp, "display_name", cfg->server.hostname);
    }
    const sb_wifi_state_t *ws = sb_wifi_get_state();
    if (ws) {
        cJSON_AddStringToObject(resp, "board_id", ws->board_id);
        cJSON_AddStringToObject(resp, "ip_addr", ws->ip_addr);
        cJSON_AddBoolToObject(resp, "ap_mode", ws->ap_active);
        if (ws->ap_active) {
            cJSON_AddStringToObject(resp, "ssid", ws->ap_ssid);
        } else if (ws->sta_connected) {
            cJSON_AddStringToObject(resp, "ssid", ws->sta_ssid);
        }
    }

    /* Plugin list */
    cJSON *plugins = sb_plugin_list_json();
    if (plugins != NULL) {
        cJSON_AddItemToObject(resp, "plugins", plugins);
    }

    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

/* GET /api/v1/system/health */
static esp_err_t handle_health(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return sb_json_error(req, "500 Internal Server Error", "OOM");
    }

    /* Aggregate plugin health */
    sb_health_status_t results[16];
    uint8_t count = sb_plugin_health_check_all(results, 16);

    bool all_healthy = true;
    bool any_unhealthy = false;
    cJSON *components = cJSON_AddArrayToObject(resp, "components");

    for (uint8_t i = 0; i < count; i++) {
        cJSON *c = cJSON_CreateObject();
        const char *state_str;
        switch (results[i].state) {
            case SB_HEALTH_HEALTHY:   state_str = "healthy";   break;
            case SB_HEALTH_DEGRADED:  state_str = "degraded";  all_healthy = false; break;
            case SB_HEALTH_UNHEALTHY: state_str = "unhealthy"; all_healthy = false; any_unhealthy = true; break;
            default:                  state_str = "unknown";    break;
        }
        cJSON_AddStringToObject(c, "status", state_str);
        if (results[i].message[0] != '\0') {
            cJSON_AddStringToObject(c, "message", results[i].message);
        }
        cJSON_AddItemToArray(components, c);
    }

    const char *overall = "healthy";
    if (any_unhealthy) overall = "unhealthy";
    else if (!all_healthy) overall = "degraded";

    cJSON_AddStringToObject(resp, "status", overall);
    cJSON_AddNumberToObject(resp, "free_heap", esp_get_free_heap_size());
    cJSON_AddBoolToObject(resp, "provisioned", sb_config_is_provisioned());

    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

/* GET /api/v1/system/config */
static esp_err_t handle_config_get(httpd_req_t *req)
{
    sb_auth_result_t auth;
    sb_auth_check(req, &auth);
    if (!sb_auth_has_scope(&auth, SB_SCOPE_READ)) {
        return sb_json_error(req, "404 Not Found", "Not found");
    }

    const sb_config_t *config = sb_config_get();
    if (config == NULL) {
        return sb_json_error(req, "404 Not Found", "No configuration loaded");
    }

    cJSON *resp = (cJSON *)sb_config_to_json(config);
    if (resp == NULL) {
        return sb_json_error(req, "500 Internal Server Error", "Failed to serialize config");
    }

    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

/* PUT /api/v1/system/config */
static esp_err_t handle_config_put(httpd_req_t *req)
{
    sb_auth_result_t auth;
    sb_auth_check(req, &auth);
    if (!sb_auth_has_scope(&auth, SB_SCOPE_ADMIN)) {
        return sb_json_error(req, "404 Not Found", "Not found");
    }

    cJSON *body = sb_json_parse_body(req);
    if (body == NULL) {
        return sb_json_error(req, "400 Bad Request", "Invalid JSON body");
    }

    sb_config_t new_config;
    esp_err_t err = sb_config_load_json(body, &new_config);
    cJSON_Delete(body);

    if (err != ESP_OK) {
        return sb_json_error(req, "400 Bad Request", "Invalid configuration");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "detail", "Configuration updated and saved to NVS");
    cJSON_AddBoolToObject(resp, "restart_required", true);
    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

/* POST /api/v1/system/restart */
static esp_err_t handle_restart(httpd_req_t *req)
{
    sb_auth_result_t auth;
    sb_auth_check(req, &auth);
    if (!sb_auth_has_scope(&auth, SB_SCOPE_ADMIN)) {
        return sb_json_error(req, "404 Not Found", "Not found");
    }

    ESP_LOGI(TAG, "Restart requested");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "detail", "Restarting...");
    sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);

    /* Delay to allow response to be sent */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK; /* Never reached */
}

/* GET /api/v1/system/ota — F15 OTA partition status */
static esp_err_t handle_ota_status(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return sb_json_error(req, "500 Internal Server Error", "OOM");
    }

    /* Running partition */
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        cJSON *run = cJSON_CreateObject();
        cJSON_AddStringToObject(run, "label", running->label);
        cJSON_AddNumberToObject(run, "address", running->address);
        cJSON_AddNumberToObject(run, "size", running->size);
        cJSON_AddStringToObject(run, "type", running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ? "ota_0" : "ota_1");
        cJSON_AddItemToObject(resp, "running", run);
    }

    /* Next OTA partition */
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next != NULL) {
        cJSON *nx = cJSON_CreateObject();
        cJSON_AddStringToObject(nx, "label", next->label);
        cJSON_AddNumberToObject(nx, "address", next->address);
        cJSON_AddNumberToObject(nx, "size", next->size);
        cJSON_AddItemToObject(resp, "next_update", nx);
    }

    /* Boot partition */
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (boot != NULL) {
        cJSON *bt = cJSON_CreateObject();
        cJSON_AddStringToObject(bt, "label", boot->label);
        cJSON_AddItemToObject(resp, "boot", bt);
    }

    /* App description from running partition */
    esp_app_desc_t app_desc;
    if (esp_ota_get_partition_description(running, &app_desc) == ESP_OK) {
        cJSON_AddStringToObject(resp, "version", app_desc.version);
        cJSON_AddStringToObject(resp, "project", app_desc.project_name);
        cJSON_AddStringToObject(resp, "idf_ver", app_desc.idf_ver);
        cJSON_AddStringToObject(resp, "date", app_desc.date);
        cJSON_AddStringToObject(resp, "time", app_desc.time);
    }

    /* Partition table summary */
    cJSON *partitions = cJSON_AddArrayToObject(resp, "partitions");
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP,
                                                     ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it != NULL) {
        const esp_partition_t *p = esp_partition_get(it);
        if (p != NULL) {
            cJSON *part = cJSON_CreateObject();
            cJSON_AddStringToObject(part, "label", p->label);
            cJSON_AddNumberToObject(part, "size", p->size);
            cJSON_AddBoolToObject(part, "is_running",
                                  running != NULL && p->address == running->address);
            cJSON_AddItemToArray(partitions, part);
        }
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);

    cJSON_AddBoolToObject(resp, "ota_supported", next != NULL);

    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

/* GET /api/v1/modules — plugin list */
static esp_err_t handle_modules(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON *modules = sb_plugin_list_json();
    if (modules != NULL) {
        cJSON_AddItemToObject(resp, "modules", modules);
    } else {
        cJSON_AddItemToObject(resp, "modules", cJSON_CreateArray());
    }
    cJSON_AddNumberToObject(resp, "count", sb_plugin_count());

    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

/* GET /api/v1/system/wifi */
static esp_err_t handle_wifi_state(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    const sb_wifi_state_t *ws = sb_wifi_get_state();

    cJSON_AddStringToObject(resp, "mode", ws->ap_active ? "ap" : "sta");
    cJSON_AddStringToObject(resp, "ip", ws->ip_addr);
    cJSON_AddStringToObject(resp, "board_id", ws->board_id);
    cJSON_AddBoolToObject(resp, "connected", ws->sta_connected || ws->ap_active);

    if (ws->ap_active) {
        cJSON_AddStringToObject(resp, "ssid", ws->ap_ssid);
        cJSON_AddStringToObject(resp, "password", ws->ap_password);
    } else if (ws->sta_connected) {
        cJSON_AddStringToObject(resp, "ssid", ws->sta_ssid);
    }

    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

/* GET /api/v1/system/wifi/scan */
static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    sb_wifi_scan_entry_t entries[20];
    uint16_t count = 20;

    esp_err_t err = sb_wifi_scan(entries, &count);
    if (err != ESP_OK) {
        return sb_json_error(req, "500 Internal Server Error", "WiFi scan failed");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON *networks = cJSON_AddArrayToObject(resp, "networks");

    for (uint16_t i = 0; i < count; i++) {
        cJSON *net = cJSON_CreateObject();
        cJSON_AddStringToObject(net, "ssid", entries[i].ssid);
        cJSON_AddNumberToObject(net, "rssi", entries[i].rssi);
        const char *auth;
        switch (entries[i].authmode) {
            case 0: auth = "open"; break;
            case 1: auth = "wep"; break;
            case 2: auth = "wpa_psk"; break;
            case 3: auth = "wpa2_psk"; break;
            case 4: auth = "wpa_wpa2_psk"; break;
            default: auth = "other"; break;
        }
        cJSON_AddStringToObject(net, "auth", auth);
        cJSON_AddItemToArray(networks, net);
    }
    cJSON_AddNumberToObject(resp, "count", count);

    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

/* POST /api/v1/system/wifi/connect */
static esp_err_t handle_wifi_connect(httpd_req_t *req)
{
    cJSON *body = sb_json_parse_body(req);
    if (body == NULL) {
        return sb_json_error(req, "400 Bad Request", "Invalid JSON body");
    }

    const cJSON *ssid_j = cJSON_GetObjectItem(body, "ssid");
    const cJSON *pass_j = cJSON_GetObjectItem(body, "password");

    if (!ssid_j || !cJSON_IsString(ssid_j) || strlen(ssid_j->valuestring) == 0) {
        cJSON_Delete(body);
        return sb_json_error(req, "400 Bad Request", "Missing ssid field");
    }

    const char *password = (pass_j && cJSON_IsString(pass_j)) ? pass_j->valuestring : "";

    /* Send response before switching (we'll lose the connection) */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "detail", "Connecting to WiFi...");
    cJSON_AddStringToObject(resp, "ssid", ssid_j->valuestring);
    sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);

    /* Small delay to flush response */
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_err_t err = sb_wifi_connect_sta(ssid_j->valuestring, password);
    cJSON_Delete(body);

    if (err == ESP_OK) {
        sb_oled_update_status();
    }

    return ESP_OK;
}

/* PUT /api/v1/system/name */
static esp_err_t handle_set_name(httpd_req_t *req)
{
    cJSON *body = sb_json_parse_body(req);
    if (body == NULL) {
        return sb_json_error(req, "400 Bad Request", "Invalid JSON body");
    }

    const cJSON *name_j = cJSON_GetObjectItem(body, "name");
    if (!name_j || !cJSON_IsString(name_j) || strlen(name_j->valuestring) == 0) {
        cJSON_Delete(body);
        return sb_json_error(req, "400 Bad Request", "Missing name field");
    }

    /* Update config */
    sb_config_t cfg;
    const sb_config_t *cur = sb_config_get();
    if (cur) {
        memcpy(&cfg, cur, sizeof(cfg));
    } else {
        memset(&cfg, 0, sizeof(cfg));
    }
    strncpy(cfg.server.hostname, name_j->valuestring, sizeof(cfg.server.hostname) - 1);
    cfg.server.hostname[sizeof(cfg.server.hostname) - 1] = '\0';
    sb_config_save(&cfg);
    cJSON_Delete(body);

    /* Update OLED */
    sb_oled_update_status();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "name", cfg.server.hostname);
    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

/* ── Backup/Restore API ─────────────────────────────────────────── */

/* GET /api/v1/system/backup — download full config as JSON */
static esp_err_t handle_backup(httpd_req_t *req)
{
    sb_auth_result_t auth;
    sb_auth_check(req, &auth);
    if (!sb_auth_has_scope(&auth, SB_SCOPE_ADMIN)) {
        return sb_json_error(req, "404 Not Found", "Not found");
    }

    char *json = sb_config_export_json();
    if (json == NULL) {
        return sb_json_error(req, "500 Internal Server Error", "Failed to export config");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"arbor_config.json\"");
    esp_err_t ret = httpd_resp_sendstr(req, json);
    free(json);
    return ret;
}

/* POST /api/v1/system/restore — upload JSON config and apply */
static esp_err_t handle_restore(httpd_req_t *req)
{
    sb_auth_result_t auth;
    sb_auth_check(req, &auth);
    if (!sb_auth_has_scope(&auth, SB_SCOPE_ADMIN)) {
        return sb_json_error(req, "404 Not Found", "Not found");
    }

    cJSON *body = sb_json_parse_body(req);
    if (body == NULL) {
        return sb_json_error(req, "400 Bad Request", "Invalid JSON body");
    }

    sb_config_t new_config;
    esp_err_t err = sb_config_load_json(body, &new_config);
    cJSON_Delete(body);

    if (err != ESP_OK) {
        return sb_json_error(req, "400 Bad Request", "Invalid configuration data");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "detail",
                            "Configuration restored and saved. Restart recommended.");
    cJSON_AddBoolToObject(resp, "restart_required", true);
    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

/* ── File Manager API ──────────────────────────────────────────── */

/* GET /api/v1/system/files — list config/data files on SPIFFS */
static esp_err_t handle_files_list(httpd_req_t *req)
{
    sb_auth_result_t auth;
    sb_auth_check(req, &auth);
    if (!sb_auth_has_scope(&auth, SB_SCOPE_READ)) {
        return sb_json_error(req, "404 Not Found", "Not found");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON *files = cJSON_AddArrayToObject(resp, "files");

    DIR *dir = opendir("/spiffs");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            /* Skip WebUI static assets */
            if (strcmp(ent->d_name, "index.html") == 0) continue;
            if (strncmp(ent->d_name, "assets", 6) == 0) continue;

            cJSON *fobj = cJSON_CreateObject();
            cJSON_AddStringToObject(fobj, "name", ent->d_name);

            char path[300];
            snprintf(path, sizeof(path), "/spiffs/%s", ent->d_name);
            struct stat st;
            if (stat(path, &st) == 0) {
                cJSON_AddNumberToObject(fobj, "size", st.st_size);
            }
            cJSON_AddItemToArray(files, fobj);
        }
        closedir(dir);
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("spiffs", &total, &used);
    cJSON_AddNumberToObject(resp, "total_bytes", total);
    cJSON_AddNumberToObject(resp, "used_bytes", used);
    cJSON_AddNumberToObject(resp, "free_bytes", total > used ? total - used : 0);

    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

/* GET /api/v1/system/files/:name — download a specific file */
static esp_err_t handle_file_download(httpd_req_t *req)
{
    sb_auth_result_t auth;
    sb_auth_check(req, &auth);
    if (!sb_auth_has_scope(&auth, SB_SCOPE_READ)) {
        return sb_json_error(req, "404 Not Found", "Not found");
    }

    /* Extract filename from URI: /api/v1/system/files/FILENAME */
    const char *prefix = "/api/v1/system/files/";
    const char *filename = req->uri + strlen(prefix);
    if (filename[0] == '\0' || strchr(filename, '/') != NULL ||
        strstr(filename, "..") != NULL) {
        return sb_json_error(req, "400 Bad Request", "Invalid filename");
    }

    char path[128];
    snprintf(path, sizeof(path), "/spiffs/%s", filename);

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return sb_json_error(req, "404 Not Found", "File not found");
    }

    httpd_resp_set_type(req, "application/octet-stream");

    char buf[512];
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, read_bytes) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_OK;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* POST /api/v1/system/files/:name — upload a file to SPIFFS */
static esp_err_t handle_file_upload(httpd_req_t *req)
{
    sb_auth_result_t auth;
    sb_auth_check(req, &auth);
    if (!sb_auth_has_scope(&auth, SB_SCOPE_ADMIN)) {
        return sb_json_error(req, "404 Not Found", "Not found");
    }

    /* Extract filename */
    const char *prefix = "/api/v1/system/files/";
    const char *filename = req->uri + strlen(prefix);
    if (filename[0] == '\0' || strchr(filename, '/') != NULL ||
        strstr(filename, "..") != NULL) {
        return sb_json_error(req, "400 Bad Request", "Invalid filename");
    }

    /* Don't allow overwriting WebUI files */
    if (strcmp(filename, "index.html") == 0 ||
        strncmp(filename, "assets", 6) == 0) {
        return sb_json_error(req, "403 Forbidden", "Cannot overwrite WebUI files");
    }

    if (req->content_len <= 0 || req->content_len > 16384) {
        return sb_json_error(req, "400 Bad Request", "File too large (max 16KB)");
    }

    char path[128];
    snprintf(path, sizeof(path), "/spiffs/%s", filename);

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return sb_json_error(req, "500 Internal Server Error", "Failed to create file");
    }

    char buf[512];
    int remaining = req->content_len;
    int total = 0;

    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            fclose(f);
            unlink(path);
            return sb_json_error(req, "500 Internal Server Error", "Upload failed");
        }
        fwrite(buf, 1, (size_t)received, f);
        remaining -= received;
        total += received;
    }
    fclose(f);

    ESP_LOGI(TAG, "File uploaded: %s (%d bytes)", path, total);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "filename", filename);
    cJSON_AddNumberToObject(resp, "size", total);
    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

/* DELETE /api/v1/system/files/:name — delete a file from SPIFFS */
static esp_err_t handle_file_delete(httpd_req_t *req)
{
    sb_auth_result_t auth;
    sb_auth_check(req, &auth);
    if (!sb_auth_has_scope(&auth, SB_SCOPE_ADMIN)) {
        return sb_json_error(req, "404 Not Found", "Not found");
    }

    const char *prefix = "/api/v1/system/files/";
    const char *filename = req->uri + strlen(prefix);
    if (filename[0] == '\0' || strchr(filename, '/') != NULL ||
        strstr(filename, "..") != NULL) {
        return sb_json_error(req, "400 Bad Request", "Invalid filename");
    }

    if (strcmp(filename, "index.html") == 0 ||
        strncmp(filename, "assets", 6) == 0) {
        return sb_json_error(req, "403 Forbidden", "Cannot delete WebUI files");
    }

    char path[128];
    snprintf(path, sizeof(path), "/spiffs/%s", filename);

    if (unlink(path) != 0) {
        return sb_json_error(req, "404 Not Found", "File not found");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "detail", "File deleted");
    esp_err_t ret = sb_json_respond(req, "200 OK", resp);
    cJSON_Delete(resp);
    return ret;
}

esp_err_t sb_api_system_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/api/v1/system/info",         .method = HTTP_GET,  .handler = handle_info },
        { .uri = "/api/v1/system/health",       .method = HTTP_GET,  .handler = handle_health },
        { .uri = "/api/v1/health",              .method = HTTP_GET,  .handler = handle_health },
        { .uri = "/api/v1/system/config",       .method = HTTP_GET,  .handler = handle_config_get },
        { .uri = "/api/v1/system/config",       .method = HTTP_PUT,  .handler = handle_config_put },
        { .uri = "/api/v1/system/restart",      .method = HTTP_POST, .handler = handle_restart },
        { .uri = "/api/v1/system/ota",          .method = HTTP_GET,  .handler = handle_ota_status },
        { .uri = "/api/v1/modules",             .method = HTTP_GET,  .handler = handle_modules },
        { .uri = "/api/v1/system/wifi",         .method = HTTP_GET,  .handler = handle_wifi_state },
        { .uri = "/api/v1/system/wifi/scan",    .method = HTTP_GET,  .handler = handle_wifi_scan },
        { .uri = "/api/v1/system/wifi/connect", .method = HTTP_POST, .handler = handle_wifi_connect },
        { .uri = "/api/v1/system/name",         .method = HTTP_PUT,  .handler = handle_set_name },
        { .uri = "/api/v1/system/backup",       .method = HTTP_GET,  .handler = handle_backup },
        { .uri = "/api/v1/system/restore",      .method = HTTP_POST, .handler = handle_restore },
        { .uri = "/api/v1/system/files",        .method = HTTP_GET,  .handler = handle_files_list },
        { .uri = "/api/v1/system/files/*",      .method = HTTP_GET,  .handler = handle_file_download },
        { .uri = "/api/v1/system/files/*",      .method = HTTP_POST, .handler = handle_file_upload },
        { .uri = "/api/v1/system/files/*",      .method = HTTP_DELETE, .handler = handle_file_delete },
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t ret = httpd_register_uri_handler(server, &uris[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s: %s", uris[i].uri, esp_err_to_name(ret));
            return ret;
        }
    }

    ESP_LOGI(TAG, "System API endpoints registered (incl. OTA status)");
    return ESP_OK;
}
