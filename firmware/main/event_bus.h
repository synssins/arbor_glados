/**
 * @file event_bus.h
 * @brief Internal event bus for inter-module communication.
 *
 * Plugins publish events (e.g. servo position changed), and
 * the WebSocket server + internal consumers subscribe to topics.
 *
 * Task: F01 (structure)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum topic string length. */
#define SB_EVENT_TOPIC_MAX 64

/** Maximum event data payload size. */
#define SB_EVENT_DATA_MAX 1024

/** Event structure. */
typedef struct {
    char topic[SB_EVENT_TOPIC_MAX];
    char data[SB_EVENT_DATA_MAX];   /**< JSON string payload */
    int64_t timestamp_us;            /**< Microsecond timestamp */
} sb_event_t;

/** Callback type for event subscribers. */
typedef void (*sb_event_cb_t)(const sb_event_t *event, void *user_data);

/**
 * Initialize the event bus.
 *
 * @return ESP_OK on success.
 */
esp_err_t sb_event_bus_init(void);

/**
 * Publish an event to the bus.
 *
 * @param topic  Topic string (e.g. "servo.position_changed").
 * @param data   JSON payload string.
 * @return ESP_OK on success.
 */
esp_err_t sb_event_publish(const char *topic, const char *data);

/**
 * Subscribe to events matching a topic pattern.
 *
 * @param pattern    Glob pattern (e.g. "servo.*", "*").
 * @param callback   Function called when matching event fires.
 * @param user_data  Opaque pointer passed to callback.
 * @return Subscription ID (>= 0) on success, negative on error.
 */
int sb_event_subscribe(const char *pattern, sb_event_cb_t callback, void *user_data);

/**
 * Unsubscribe from events.
 *
 * @param sub_id  Subscription ID from sb_event_subscribe.
 */
void sb_event_unsubscribe(int sub_id);

#ifdef __cplusplus
}
#endif
