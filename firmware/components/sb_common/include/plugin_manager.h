/**
 * @file plugin_manager.h
 * @brief Plugin registration and dispatch system.
 *
 * Implements the module interface contract from ARBOR_PROJECT_PLAN.md
 * Section 5. All hardware drivers are plugins with a uniform interface.
 *
 * Task: F01 (structure), F05 (implementation)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum plugin name length. */
#define SB_PLUGIN_NAME_MAX 32

/** Maximum version string length. */
#define SB_PLUGIN_VERSION_MAX 16

/** Plugin health states. */
typedef enum {
    SB_HEALTH_HEALTHY   = 0,
    SB_HEALTH_DEGRADED  = 1,
    SB_HEALTH_UNHEALTHY = 2,
    SB_HEALTH_UNKNOWN   = 3,
} sb_health_state_t;

/** Plugin health status. */
typedef struct {
    sb_health_state_t state;
    char message[128];
} sb_health_status_t;

/**
 * Plugin interface — all modules implement this contract.
 *
 * Mirrors the Python ArborPlugin ABC for API parity.
 */
typedef struct sb_plugin {
    /** Plugin metadata */
    const char *name;        /**< Unique slug (e.g. "servo-bus") */
    const char *version;     /**< Semver string */

    /** Lifecycle */
    esp_err_t (*initialize)(struct sb_plugin *self, const cJSON *config);
    esp_err_t (*shutdown)(struct sb_plugin *self);

    /** State and commands */
    cJSON *(*get_state)(struct sb_plugin *self);
    cJSON *(*handle_command)(struct sb_plugin *self, const char *cmd, const cJSON *params);

    /** Health */
    sb_health_status_t (*health_check)(struct sb_plugin *self);

    /** Plugin-private data */
    void *ctx;
} sb_plugin_t;

/**
 * Initialize the plugin manager.
 *
 * @return ESP_OK on success.
 */
esp_err_t sb_plugin_manager_init(void);

/**
 * Register a plugin with the manager.
 *
 * @param plugin  Plugin instance (must have name set).
 * @return ESP_OK on success, ESP_ERR_NO_MEM if full.
 */
esp_err_t sb_plugin_register(sb_plugin_t *plugin);

/**
 * Initialize all registered plugins.
 *
 * @param config  Root JSON config (plugins extract their own section).
 * @return ESP_OK if all plugins initialized successfully.
 */
esp_err_t sb_plugin_init_all(const cJSON *config);

/**
 * Shutdown all plugins in reverse order.
 */
void sb_plugin_shutdown_all(void);

/**
 * Find a plugin by name.
 *
 * @param name  Plugin name slug.
 * @return Plugin pointer or NULL if not found.
 */
sb_plugin_t *sb_plugin_find(const char *name);

/**
 * Get the number of registered plugins.
 */
uint8_t sb_plugin_count(void);

/**
 * Run health checks on all plugins.
 *
 * @param[out] results  Array of health statuses (caller allocates).
 * @param max           Maximum entries in results array.
 * @return Number of results written.
 */
uint8_t sb_plugin_health_check_all(sb_health_status_t *results, uint8_t max);

/**
 * Get a plugin by index (for iteration).
 *
 * @param index  Plugin index (0 to sb_plugin_count()-1).
 * @return Plugin pointer or NULL if out of range.
 */
sb_plugin_t *sb_plugin_get(uint8_t index);

/**
 * Check if a plugin has been initialized.
 *
 * @param name  Plugin name slug.
 * @return true if the plugin is registered and initialized.
 */
bool sb_plugin_is_initialized(const char *name);

/**
 * Dispatch a command to a named plugin.
 *
 * Convenience wrapper around sb_plugin_find + handle_command.
 *
 * @param plugin_name  Target plugin name.
 * @param cmd          Command string.
 * @param params       JSON parameters (may be NULL).
 * @return cJSON result (caller frees), or NULL on error.
 */
cJSON *sb_plugin_dispatch(const char *plugin_name, const char *cmd, const cJSON *params);

/**
 * Build a JSON array listing all plugins with status.
 *
 * Used by GET /api/v1/modules endpoint.
 * Caller must free with cJSON_Delete().
 *
 * @return cJSON array or NULL on error.
 */
cJSON *sb_plugin_list_json(void);

#ifdef __cplusplus
}
#endif
