/**
 * @file event_bus.c
 * @brief Internal event bus — stub.
 *
 * Task: F01 (stub)
 */

#include "event_bus.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "sb_events";

#define MAX_SUBSCRIBERS 32

typedef struct {
    bool active;
    char pattern[SB_EVENT_TOPIC_MAX];
    sb_event_cb_t callback;
    void *user_data;
} subscriber_t;

static subscriber_t s_subs[MAX_SUBSCRIBERS];
static bool s_initialized = false;

/**
 * Simple glob match: only supports trailing '*'.
 * e.g. "servo.*" matches "servo.position_changed"
 */
static bool topic_matches(const char *pattern, const char *topic)
{
    size_t plen = strlen(pattern);

    /* Wildcard "*" matches everything */
    if (plen == 1 && pattern[0] == '*') {
        return true;
    }

    /* "prefix.*" — match prefix up to the dot */
    if (plen >= 2 && pattern[plen - 1] == '*' && pattern[plen - 2] == '.') {
        return strncmp(pattern, topic, plen - 1) == 0;
    }

    /* Exact match */
    return strcmp(pattern, topic) == 0;
}

esp_err_t sb_event_bus_init(void)
{
    memset(s_subs, 0, sizeof(s_subs));
    s_initialized = true;
    ESP_LOGI(TAG, "Event bus initialized");
    return ESP_OK;
}

esp_err_t sb_event_publish(const char *topic, const char *data)
{
    if (!s_initialized || topic == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    sb_event_t event;
    memset(&event, 0, sizeof(event));
    strncpy(event.topic, topic, SB_EVENT_TOPIC_MAX - 1);
    if (data != NULL) {
        strncpy(event.data, data, SB_EVENT_DATA_MAX - 1);
    }
    event.timestamp_us = esp_timer_get_time();

    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (s_subs[i].active && topic_matches(s_subs[i].pattern, topic)) {
            s_subs[i].callback(&event, s_subs[i].user_data);
        }
    }

    return ESP_OK;
}

int sb_event_subscribe(const char *pattern, sb_event_cb_t callback, void *user_data)
{
    if (!s_initialized || pattern == NULL || callback == NULL) {
        return -1;
    }

    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (!s_subs[i].active) {
            s_subs[i].active = true;
            strncpy(s_subs[i].pattern, pattern, SB_EVENT_TOPIC_MAX - 1);
            s_subs[i].callback = callback;
            s_subs[i].user_data = user_data;
            return i;
        }
    }

    ESP_LOGW(TAG, "No subscriber slots available");
    return -1;
}

void sb_event_unsubscribe(int sub_id)
{
    if (sub_id >= 0 && sub_id < MAX_SUBSCRIBERS) {
        s_subs[sub_id].active = false;
    }
}
