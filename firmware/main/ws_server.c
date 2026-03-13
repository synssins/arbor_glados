/**
 * @file ws_server.c
 * @brief WebSocket server — subscribe/publish protocol.
 *
 * Runs on top of the HTTP server with WebSocket upgrade at /api/v1/ws.
 * Supports topic subscription with glob patterns and command dispatch.
 *
 * Protocol (JSON text frames):
 *   Client → Server:
 *     {"type":"subscribe","topic":"servo.*"}
 *     {"type":"unsubscribe","topic":"servo.*"}
 *     {"type":"command","plugin":"servo-bus","cmd":"get_state","params":{}}
 *
 *   Server → Client:
 *     {"type":"event","topic":"servo.position_changed","data":{...}}
 *     {"type":"result","cmd":"get_state","data":{...}}
 *     {"type":"error","detail":"..."}
 *
 * Task: F10
 */

#include "ws_server.h"
#include "event_bus.h"
#include "plugin_manager.h"

#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "sb_ws";

/* ── Configuration ── */

#ifndef CONFIG_SB_MAX_WS_CLIENTS
#define CONFIG_SB_MAX_WS_CLIENTS 4
#endif

#define MAX_SUBSCRIPTIONS  8   /* Per client */
#define WS_MAX_FRAME_SIZE  2048

/* ── Client Tracking ── */

typedef struct {
    int fd;                                   /* Socket file descriptor */
    bool connected;
    char subscriptions[MAX_SUBSCRIPTIONS][SB_EVENT_TOPIC_MAX];
    uint8_t sub_count;
} ws_client_t;

static ws_client_t s_clients[CONFIG_SB_MAX_WS_CLIENTS];
static httpd_handle_t s_server = NULL;
static int s_event_sub_id = -1;  /* Event bus subscription */

/* ── Glob Pattern Matching ── */

/**
 * Simple glob match: supports * (match any) and prefix.* patterns.
 */
static bool topic_matches(const char *pattern, const char *topic)
{
    if (strcmp(pattern, "*") == 0) return true;

    const char *star = strchr(pattern, '*');
    if (star == NULL) {
        return strcmp(pattern, topic) == 0;
    }

    /* "prefix.*" or "prefix*" */
    size_t prefix_len = (size_t)(star - pattern);
    return strncmp(pattern, topic, prefix_len) == 0;
}

/* ── Send Helpers ── */

/**
 * Send a text frame to a specific client.
 */
static esp_err_t ws_send_text(int fd, const char *data, size_t len)
{
    if (s_server == NULL) return ESP_FAIL;

    httpd_ws_frame_t frame = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)data,
        .len     = len,
        .final   = true,
    };

    return httpd_ws_send_frame_async(s_server, fd, &frame);
}

/**
 * Send a JSON object as a text frame to a client.
 */
static void ws_send_json(int fd, cJSON *json)
{
    char *str = cJSON_PrintUnformatted(json);
    if (str != NULL) {
        ws_send_text(fd, str, strlen(str));
        cJSON_free(str);
    }
}

/**
 * Send an error message to a client.
 */
static void ws_send_error(int fd, const char *detail)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "error");
    cJSON_AddStringToObject(msg, "detail", detail);
    ws_send_json(fd, msg);
    cJSON_Delete(msg);
}

/* ── Client Management ── */

static ws_client_t *find_client(int fd)
{
    for (int i = 0; i < CONFIG_SB_MAX_WS_CLIENTS; i++) {
        if (s_clients[i].connected && s_clients[i].fd == fd) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static ws_client_t *alloc_client(int fd)
{
    for (int i = 0; i < CONFIG_SB_MAX_WS_CLIENTS; i++) {
        if (!s_clients[i].connected) {
            memset(&s_clients[i], 0, sizeof(ws_client_t));
            s_clients[i].fd = fd;
            s_clients[i].connected = true;
            return &s_clients[i];
        }
    }
    return NULL;
}

static void remove_client(int fd)
{
    ws_client_t *c = find_client(fd);
    if (c != NULL) {
        c->connected = false;
        c->fd = -1;
        c->sub_count = 0;
        ESP_LOGI(TAG, "Client disconnected (fd=%d)", fd);
    }
}

/* ── Command Handlers ── */

static void handle_subscribe(ws_client_t *client, const cJSON *msg)
{
    const cJSON *topic = cJSON_GetObjectItem(msg, "topic");
    if (!topic || !cJSON_IsString(topic) || topic->valuestring[0] == '\0') {
        ws_send_error(client->fd, "Missing or invalid topic");
        return;
    }

    if (client->sub_count >= MAX_SUBSCRIPTIONS) {
        ws_send_error(client->fd, "Too many subscriptions");
        return;
    }

    /* Check for duplicate */
    for (uint8_t i = 0; i < client->sub_count; i++) {
        if (strcmp(client->subscriptions[i], topic->valuestring) == 0) {
            return; /* Already subscribed */
        }
    }

    strncpy(client->subscriptions[client->sub_count],
            topic->valuestring, SB_EVENT_TOPIC_MAX - 1);
    client->sub_count++;

    ESP_LOGD(TAG, "Client fd=%d subscribed to '%s'", client->fd, topic->valuestring);
}

static void handle_unsubscribe(ws_client_t *client, const cJSON *msg)
{
    const cJSON *topic = cJSON_GetObjectItem(msg, "topic");
    if (!topic || !cJSON_IsString(topic)) return;

    for (uint8_t i = 0; i < client->sub_count; i++) {
        if (strcmp(client->subscriptions[i], topic->valuestring) == 0) {
            /* Shift remaining subscriptions down */
            for (uint8_t j = i; j < client->sub_count - 1; j++) {
                memcpy(client->subscriptions[j], client->subscriptions[j + 1],
                       SB_EVENT_TOPIC_MAX);
            }
            client->sub_count--;
            break;
        }
    }
}

static void handle_command(ws_client_t *client, const cJSON *msg)
{
    const cJSON *plugin_name = cJSON_GetObjectItem(msg, "plugin");
    const cJSON *cmd = cJSON_GetObjectItem(msg, "cmd");

    if (!plugin_name || !cJSON_IsString(plugin_name) ||
        !cmd || !cJSON_IsString(cmd)) {
        ws_send_error(client->fd, "Missing plugin or cmd");
        return;
    }

    const cJSON *params = cJSON_GetObjectItem(msg, "params");
    cJSON *result = sb_plugin_dispatch(plugin_name->valuestring,
                                       cmd->valuestring, params);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "type", "result");
    cJSON_AddStringToObject(response, "cmd", cmd->valuestring);
    if (result != NULL) {
        cJSON_AddItemToObject(response, "data", result);
    } else {
        cJSON_AddNullToObject(response, "data");
        cJSON_AddStringToObject(response, "error", "Command failed or plugin not found");
    }

    ws_send_json(client->fd, response);
    cJSON_Delete(response);
}

/* ── Event Bus Callback ── */

/**
 * Called by the event bus when any event fires.
 * Forwards to subscribed WebSocket clients.
 */
static void event_bus_callback(const sb_event_t *event, void *user_data)
{
    (void)user_data;
    if (event == NULL || s_server == NULL) return;

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "event");
    cJSON_AddStringToObject(msg, "topic", event->topic);

    /* Parse data as JSON if possible, otherwise send as string */
    cJSON *data = cJSON_Parse(event->data);
    if (data != NULL) {
        cJSON_AddItemToObject(msg, "data", data);
    } else {
        cJSON_AddStringToObject(msg, "data", event->data);
    }

    char *str = cJSON_PrintUnformatted(msg);
    if (str == NULL) {
        cJSON_Delete(msg);
        return;
    }
    size_t str_len = strlen(str);

    for (int i = 0; i < CONFIG_SB_MAX_WS_CLIENTS; i++) {
        if (!s_clients[i].connected) continue;

        /* Check if any subscription matches */
        for (uint8_t j = 0; j < s_clients[i].sub_count; j++) {
            if (topic_matches(s_clients[i].subscriptions[j], event->topic)) {
                ws_send_text(s_clients[i].fd, str, str_len);
                break; /* Don't send duplicates to same client */
            }
        }
    }

    cJSON_free(str);
    cJSON_Delete(msg);
}

/* ── WebSocket URI Handler ── */

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* WebSocket handshake — accept the upgrade */
        int fd = httpd_req_to_sockfd(req);
        ws_client_t *client = alloc_client(fd);
        if (client == NULL) {
            ESP_LOGW(TAG, "Max WebSocket clients reached, rejecting");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "WebSocket client connected (fd=%d)", fd);
        return ESP_OK;
    }

    /* Receive WebSocket frame */
    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;

    /* First call with len=0 to get the frame length */
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;

    if (frame.len == 0) return ESP_OK;
    if (frame.len > WS_MAX_FRAME_SIZE) {
        int fd = httpd_req_to_sockfd(req);
        ws_send_error(fd, "Frame too large");
        return ESP_OK;
    }

    uint8_t *buf = calloc(1, frame.len + 1);
    if (buf == NULL) return ESP_ERR_NO_MEM;

    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }

    /* Handle close frame */
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        remove_client(fd);
        free(buf);
        return ESP_OK;
    }

    /* Parse JSON text frame */
    if (frame.type != HTTPD_WS_TYPE_TEXT) {
        free(buf);
        return ESP_OK;
    }

    int fd = httpd_req_to_sockfd(req);
    ws_client_t *client = find_client(fd);
    if (client == NULL) {
        /* Late-connect: might have missed the GET handshake tracking */
        client = alloc_client(fd);
        if (client == NULL) {
            free(buf);
            return ESP_OK;
        }
    }

    cJSON *msg = cJSON_Parse((char *)buf);
    free(buf);

    if (msg == NULL) {
        ws_send_error(fd, "Invalid JSON");
        return ESP_OK;
    }

    const cJSON *type = cJSON_GetObjectItem(msg, "type");
    if (!type || !cJSON_IsString(type)) {
        ws_send_error(fd, "Missing type field");
        cJSON_Delete(msg);
        return ESP_OK;
    }

    if (strcmp(type->valuestring, "subscribe") == 0) {
        handle_subscribe(client, msg);
    } else if (strcmp(type->valuestring, "unsubscribe") == 0) {
        handle_unsubscribe(client, msg);
    } else if (strcmp(type->valuestring, "command") == 0) {
        handle_command(client, msg);
    } else {
        ws_send_error(fd, "Unknown message type");
    }

    cJSON_Delete(msg);
    return ESP_OK;
}

/* ── Public API ── */

esp_err_t sb_ws_init(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_server = server;
    memset(s_clients, 0, sizeof(s_clients));
    for (int i = 0; i < CONFIG_SB_MAX_WS_CLIENTS; i++) {
        s_clients[i].fd = -1;
    }

    /* Register WebSocket URI handler */
    const httpd_uri_t ws_uri = {
        .uri          = "/api/v1/ws",
        .method       = HTTP_GET,
        .handler      = ws_handler,
        .is_websocket = true,
    };

    esp_err_t ret = httpd_register_uri_handler(server, &ws_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WS handler: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Subscribe to all events for forwarding to WebSocket clients */
    s_event_sub_id = sb_event_subscribe("*", event_bus_callback, NULL);
    if (s_event_sub_id < 0) {
        ESP_LOGW(TAG, "Failed to subscribe to event bus");
    }

    ESP_LOGI(TAG, "WebSocket server initialized at /api/v1/ws (max %d clients)",
             CONFIG_SB_MAX_WS_CLIENTS);
    return ESP_OK;
}

int sb_ws_broadcast(const char *topic, const char *data)
{
    if (topic == NULL || data == NULL || s_server == NULL) return 0;

    /* Build event JSON */
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "event");
    cJSON_AddStringToObject(msg, "topic", topic);

    cJSON *parsed = cJSON_Parse(data);
    if (parsed != NULL) {
        cJSON_AddItemToObject(msg, "data", parsed);
    } else {
        cJSON_AddStringToObject(msg, "data", data);
    }

    char *str = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (str == NULL) return 0;

    size_t str_len = strlen(str);
    int sent = 0;

    for (int i = 0; i < CONFIG_SB_MAX_WS_CLIENTS; i++) {
        if (!s_clients[i].connected) continue;

        for (uint8_t j = 0; j < s_clients[i].sub_count; j++) {
            if (topic_matches(s_clients[i].subscriptions[j], topic)) {
                if (ws_send_text(s_clients[i].fd, str, str_len) == ESP_OK) {
                    sent++;
                }
                break;
            }
        }
    }

    cJSON_free(str);
    return sent;
}

int sb_ws_client_count(void)
{
    int count = 0;
    for (int i = 0; i < CONFIG_SB_MAX_WS_CLIENTS; i++) {
        if (s_clients[i].connected) count++;
    }
    return count;
}
