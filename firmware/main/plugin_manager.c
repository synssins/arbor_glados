/**
 * @file plugin_manager.c
 * @brief Plugin registration and dispatch system.
 *
 * Manages the lifecycle of all hardware driver plugins:
 *   1. Registration — plugins register themselves at startup
 *   2. Initialization — config JSON pushed to each plugin
 *   3. Command dispatch — route commands to the right plugin
 *   4. Health checks — aggregate plugin health for system status
 *   5. Shutdown — reverse-order teardown
 *
 * Task: F05
 */

#include "plugin_manager.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "sb_plugins";

/** Per-plugin state tracking. */
typedef struct {
    sb_plugin_t *plugin;
    bool initialized;
} plugin_entry_t;

static plugin_entry_t s_plugins[CONFIG_SB_MAX_PLUGINS];
static uint8_t s_count = 0;
static bool s_manager_initialized = false;

/* ── Command Timing Thresholds ── */

/** Commands exceeding this are logged at WARNING level.
 *  Covers normal operations (get_state, set_position, etc.) which
 *  should complete in < 100ms.  Admin operations (factory_reset,
 *  restore, set_id) legitimately take 200-800ms and will trigger
 *  the warning — this is intentional for visibility. */
#define CMD_WARN_THRESHOLD_US   200000   /* 200ms */

/** Commands exceeding this are logged at ERROR level.
 *  Any command taking this long suggests a hardware issue,
 *  bus contention, or a stuck operation. */
#define CMD_ERROR_THRESHOLD_US  2000000  /* 2s */

esp_err_t sb_plugin_manager_init(void)
{
    memset(s_plugins, 0, sizeof(s_plugins));
    s_count = 0;
    s_manager_initialized = true;
    ESP_LOGI(TAG, "Plugin manager initialized (max %d plugins)", CONFIG_SB_MAX_PLUGINS);
    return ESP_OK;
}

esp_err_t sb_plugin_register(sb_plugin_t *plugin)
{
    if (!s_manager_initialized || plugin == NULL || plugin->name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_count >= CONFIG_SB_MAX_PLUGINS) {
        ESP_LOGE(TAG, "Plugin limit reached (%d)", CONFIG_SB_MAX_PLUGINS);
        return ESP_ERR_NO_MEM;
    }

    /* Check for duplicates */
    for (uint8_t i = 0; i < s_count; i++) {
        if (strcmp(s_plugins[i].plugin->name, plugin->name) == 0) {
            ESP_LOGE(TAG, "Plugin '%s' already registered", plugin->name);
            return ESP_ERR_INVALID_STATE;
        }
    }

    s_plugins[s_count].plugin = plugin;
    s_plugins[s_count].initialized = false;
    s_count++;

    ESP_LOGI(TAG, "Plugin registered: %s v%s (%d/%d)",
             plugin->name, plugin->version ? plugin->version : "?",
             s_count, CONFIG_SB_MAX_PLUGINS);
    return ESP_OK;
}

esp_err_t sb_plugin_init_all(const cJSON *config)
{
    uint8_t success = 0;
    uint8_t failed = 0;

    for (uint8_t i = 0; i < s_count; i++) {
        sb_plugin_t *p = s_plugins[i].plugin;

        if (s_plugins[i].initialized) {
            ESP_LOGW(TAG, "Plugin '%s' already initialized, skipping", p->name);
            continue;
        }

        if (p->initialize == NULL) {
            /* No init function — mark as initialized */
            s_plugins[i].initialized = true;
            success++;
            continue;
        }

        /* Extract plugin-specific config section */
        const cJSON *plugin_config = NULL;
        if (config != NULL) {
            plugin_config = cJSON_GetObjectItem(config, p->name);
        }

        esp_err_t err = p->initialize(p, plugin_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Plugin '%s' init failed: %s",
                     p->name, esp_err_to_name(err));
            failed++;
            /* Continue initializing other plugins — don't abort */
        } else {
            s_plugins[i].initialized = true;
            success++;
            ESP_LOGI(TAG, "Plugin '%s' initialized", p->name);
        }
    }

    ESP_LOGI(TAG, "Plugin init complete: %d ok, %d failed, %d total",
             success, failed, s_count);
    return (failed > 0) ? ESP_FAIL : ESP_OK;
}

void sb_plugin_shutdown_all(void)
{
    /* Shutdown in reverse order */
    for (int i = (int)s_count - 1; i >= 0; i--) {
        if (!s_plugins[i].initialized) {
            continue;
        }

        sb_plugin_t *p = s_plugins[i].plugin;
        if (p->shutdown != NULL) {
            esp_err_t err = p->shutdown(p);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Plugin '%s' shutdown error: %s",
                         p->name, esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "Plugin '%s' shut down", p->name);
            }
        }
        s_plugins[i].initialized = false;
    }

    ESP_LOGI(TAG, "All plugins shut down");
}

sb_plugin_t *sb_plugin_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (uint8_t i = 0; i < s_count; i++) {
        if (strcmp(s_plugins[i].plugin->name, name) == 0) {
            return s_plugins[i].plugin;
        }
    }
    return NULL;
}

sb_plugin_t *sb_plugin_get(uint8_t index)
{
    if (index >= s_count) {
        return NULL;
    }
    return s_plugins[index].plugin;
}

uint8_t sb_plugin_count(void)
{
    return s_count;
}

bool sb_plugin_is_initialized(const char *name)
{
    if (name == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < s_count; i++) {
        if (strcmp(s_plugins[i].plugin->name, name) == 0) {
            return s_plugins[i].initialized;
        }
    }
    return false;
}

uint8_t sb_plugin_health_check_all(sb_health_status_t *results, uint8_t max)
{
    uint8_t written = 0;
    for (uint8_t i = 0; i < s_count && written < max; i++) {
        sb_plugin_t *p = s_plugins[i].plugin;

        if (!s_plugins[i].initialized) {
            results[written].state = SB_HEALTH_UNHEALTHY;
            snprintf(results[written].message, sizeof(results[written].message),
                     "%s: not initialized", p->name);
            written++;
            continue;
        }

        if (p->health_check != NULL) {
            results[written] = p->health_check(p);
        } else {
            results[written].state = SB_HEALTH_UNKNOWN;
            snprintf(results[written].message, sizeof(results[written].message),
                     "%s: no health check", p->name);
        }
        written++;
    }
    return written;
}

cJSON *sb_plugin_dispatch(const char *plugin_name, const char *cmd, const cJSON *params)
{
    if (plugin_name == NULL || cmd == NULL) {
        return NULL;
    }

    sb_plugin_t *p = sb_plugin_find(plugin_name);
    if (p == NULL) {
        ESP_LOGW(TAG, "Dispatch: plugin '%s' not found", plugin_name);
        return NULL;
    }

    if (!sb_plugin_is_initialized(plugin_name)) {
        ESP_LOGW(TAG, "Dispatch: plugin '%s' not initialized", plugin_name);
        return NULL;
    }

    if (p->handle_command == NULL) {
        ESP_LOGW(TAG, "Dispatch: plugin '%s' has no command handler", plugin_name);
        return NULL;
    }

    /* ── Command timing watchdog ──
     * Measures wall-clock time for every plugin command.
     * Adds elapsed_us to the response JSON for upstream monitoring.
     * Logs warnings for slow commands — does NOT enforce a deadline,
     * because killing a command mid-EEPROM-write could brick a servo. */
    int64_t start_us = esp_timer_get_time();

    ESP_LOGD(TAG, "Dispatching '%s' to plugin '%s'", cmd, plugin_name);
    cJSON *result = p->handle_command(p, cmd, params);

    int64_t elapsed_us = esp_timer_get_time() - start_us;

    /* Log based on elapsed time */
    if (elapsed_us > CMD_ERROR_THRESHOLD_US) {
        ESP_LOGE(TAG, "VERY SLOW COMMAND: %s.%s took %lld us (%.1f ms) — "
                 "possible bus hang or hardware issue",
                 plugin_name, cmd, (long long)elapsed_us,
                 (double)elapsed_us / 1000.0);
    } else if (elapsed_us > CMD_WARN_THRESHOLD_US) {
        ESP_LOGW(TAG, "Slow command: %s.%s took %lld us (%.1f ms)",
                 plugin_name, cmd, (long long)elapsed_us,
                 (double)elapsed_us / 1000.0);
    } else {
        ESP_LOGD(TAG, "Command %s.%s completed in %lld us",
                 plugin_name, cmd, (long long)elapsed_us);
    }

    /* Inject timing into response JSON so HTTP callers get visibility.
     * Only add if result is a JSON object (not array or null).
     * Guard against collision if a plugin already set its own elapsed_us.
     *
     * NOTE: Timing side-channel consideration — elapsed_us leaks
     * server-side execution timing to all API callers.  Acceptable
     * for Phase 1 (LAN-only, grade-school STEMMA classes).  Gate
     * or remove before any internet-facing deployment.
     *
     * NOTE: api_emergency.c intentionally bypasses sb_plugin_dispatch()
     * and calls p->handle_command() directly for performance.  It has
     * its own timing instrumentation (100ms deadline).  This watchdog
     * does NOT cover the E-stop path — by design. */
    if (result != NULL && cJSON_IsObject(result)) {
        if (!cJSON_HasObjectItem(result, "elapsed_us")) {
            cJSON_AddNumberToObject(result, "elapsed_us", (double)elapsed_us);
        } else {
            /* Plugin already reported its own timing — use a distinct key */
            cJSON_AddNumberToObject(result, "dispatch_elapsed_us", (double)elapsed_us);
        }
    }

    return result;
}

cJSON *sb_plugin_list_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) {
        return NULL;
    }

    for (uint8_t i = 0; i < s_count; i++) {
        sb_plugin_t *p = s_plugins[i].plugin;
        cJSON *obj = cJSON_CreateObject();
        if (obj == NULL) {
            continue;
        }

        cJSON_AddStringToObject(obj, "name", p->name);
        cJSON_AddStringToObject(obj, "version", p->version ? p->version : "unknown");
        cJSON_AddBoolToObject(obj, "initialized", s_plugins[i].initialized);

        /* Include health status */
        if (s_plugins[i].initialized && p->health_check != NULL) {
            sb_health_status_t hs = p->health_check(p);
            const char *state_str;
            switch (hs.state) {
                case SB_HEALTH_HEALTHY:   state_str = "healthy";   break;
                case SB_HEALTH_DEGRADED:  state_str = "degraded";  break;
                case SB_HEALTH_UNHEALTHY: state_str = "unhealthy"; break;
                default:                  state_str = "unknown";   break;
            }
            cJSON_AddStringToObject(obj, "health", state_str);
            if (hs.message[0] != '\0') {
                cJSON_AddStringToObject(obj, "health_message", hs.message);
            }
        } else {
            cJSON_AddStringToObject(obj, "health",
                                    s_plugins[i].initialized ? "unknown" : "not_initialized");
        }

        cJSON_AddItemToArray(arr, obj);
    }

    return arr;
}
