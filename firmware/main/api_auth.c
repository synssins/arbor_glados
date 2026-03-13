/**
 * @file api_auth.c
 * @brief API key authentication middleware.
 *
 * Validates Bearer tokens from the Authorization header against
 * SHA-256 hashes stored in NVS. (Argon2id is too heavy for ESP32
 * real-time use — SHA-256 with per-key salt is the ESP32 compromise.
 * The control server uses Argon2id; the node uses SHA-256.)
 *
 * Scopes are enforced per endpoint via bitmask.
 * Rate limiting uses a sliding-window counter per key.
 *
 * Special paths (health, emergency-stop) bypass auth or rate limits.
 *
 * Task: F04
 */

#include "api_auth.h"

#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include "app_config.h"

static const char *TAG = "sb_auth";

/** Maximum stored API keys. */
#define MAX_API_KEYS 8

/** SHA-256 digest length. */
#define SHA256_LEN 32

/** Rate limit window in seconds. */
#define RATE_WINDOW_SEC 60

/** Stored API key record. */
typedef struct {
    char key_id[64];                /**< Key identifier (prefix) */
    uint8_t hash[SHA256_LEN];       /**< SHA-256(salt + key) */
    uint8_t salt[16];               /**< Per-key salt */
    uint8_t scopes;                 /**< Scope bitmask */
    bool active;                    /**< Not revoked */
} api_key_record_t;

/** Per-key rate limit state. */
typedef struct {
    char key_id[64];
    uint32_t request_timestamps[512]; /**< Circular buffer of timestamps */
    uint16_t head;
    uint16_t count;
} rate_state_t;

static api_key_record_t s_keys[MAX_API_KEYS];
static uint8_t s_key_count = 0;

static rate_state_t s_rate[MAX_API_KEYS];
static bool s_initialized = false;

/** Paths that don't require authentication. */
static bool is_public_path(const char *uri)
{
    if (uri == NULL) return false;
    if (strcmp(uri, "/api/v1/system/health") == 0) return true;
    if (strcmp(uri, "/api/v1/health") == 0) return true;
    return false;
}

/** Paths exempt from rate limiting. */
static bool is_rate_exempt(const char *uri)
{
    if (uri == NULL) return false;
    if (strcmp(uri, "/api/v1/emergency-stop") == 0) return true;
    return false;
}

/**
 * Compute SHA-256(salt + key).
 */
static void compute_hash(const uint8_t *salt, size_t salt_len,
                         const char *key, size_t key_len,
                         uint8_t out[SHA256_LEN])
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); /* 0 = SHA-256 (not 224) */
    mbedtls_sha256_update(&ctx, salt, salt_len);
    mbedtls_sha256_update(&ctx, (const uint8_t *)key, key_len);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

/**
 * Extract Bearer token from Authorization header.
 * Returns pointer into the header buffer (not a copy).
 */
static const char *extract_bearer(httpd_req_t *req, char *buf, size_t buf_size)
{
    int hdr_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdr_len <= 0 || (size_t)hdr_len >= buf_size) {
        return NULL;
    }

    esp_err_t ret = httpd_req_get_hdr_value_str(req, "Authorization", buf, buf_size);
    if (ret != ESP_OK) {
        return NULL;
    }

    /* Must start with "Bearer " */
    if (strncmp(buf, "Bearer ", 7) != 0) {
        return NULL;
    }

    return buf + 7;
}

esp_err_t sb_auth_init(void)
{
    memset(s_keys, 0, sizeof(s_keys));
    memset(s_rate, 0, sizeof(s_rate));
    s_key_count = 0;

    /* Load keys from NVS */
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(CONFIG_SB_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No NVS namespace — auth running without keys");
        s_initialized = true;
        return ESP_OK;
    }

    /* Keys are stored as blobs: "akey_0", "akey_1", etc. */
    for (uint8_t i = 0; i < MAX_API_KEYS; i++) {
        char nvs_key[16];
        snprintf(nvs_key, sizeof(nvs_key), "akey_%d", i);
        size_t len = sizeof(api_key_record_t);
        ret = nvs_get_blob(nvs, nvs_key, &s_keys[i], &len);
        if (ret == ESP_OK && s_keys[i].active) {
            s_key_count++;
            ESP_LOGI(TAG, "Loaded API key: %s (scopes=0x%02x)",
                     s_keys[i].key_id, s_keys[i].scopes);
        }
    }

    nvs_close(nvs);
    s_initialized = true;

    ESP_LOGI(TAG, "Auth initialized: %d keys loaded", s_key_count);
    return ESP_OK;
}

esp_err_t sb_auth_check(httpd_req_t *req, sb_auth_result_t *result)
{
    if (req == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));

    /* Public paths bypass auth */
    if (is_public_path(req->uri)) {
        result->authenticated = true;
        result->scopes = SB_SCOPE_READ;
        strncpy(result->key_id, "public", sizeof(result->key_id) - 1);
        return ESP_OK;
    }

    /* No keys loaded — provisioning mode, allow all */
    if (s_key_count == 0) {
        result->authenticated = true;
        result->scopes = SB_SCOPE_ALL;
        strncpy(result->key_id, "provisioning", sizeof(result->key_id) - 1);
        return ESP_OK;
    }

    /* Extract Bearer token */
    char auth_buf[512];
    const char *token = extract_bearer(req, auth_buf, sizeof(auth_buf));
    if (token == NULL) {
        /* No auth header — return 404 per SECURITY.md (not 401/403) */
        result->authenticated = false;
        return ESP_ERR_INVALID_STATE;
    }

    size_t token_len = strlen(token);

    /* Check against all stored keys */
    for (uint8_t i = 0; i < MAX_API_KEYS; i++) {
        if (!s_keys[i].active) {
            continue;
        }

        uint8_t hash[SHA256_LEN];
        compute_hash(s_keys[i].salt, sizeof(s_keys[i].salt),
                     token, token_len, hash);

        if (memcmp(hash, s_keys[i].hash, SHA256_LEN) == 0) {
            /* Match! */
            result->authenticated = true;
            result->scopes = s_keys[i].scopes;
            strncpy(result->key_id, s_keys[i].key_id, sizeof(result->key_id) - 1);

            /* Rate check (unless exempt path) */
            if (!is_rate_exempt(req->uri) && !sb_auth_rate_check(result->key_id)) {
                result->authenticated = false;
                ESP_LOGW(TAG, "Rate limit exceeded for key '%s'", result->key_id);
                return ESP_ERR_INVALID_STATE;
            }

            return ESP_OK;
        }
    }

    /* No match */
    result->authenticated = false;
    return ESP_ERR_INVALID_STATE;
}

bool sb_auth_has_scope(const sb_auth_result_t *result, sb_scope_t required_scope)
{
    if (result == NULL || !result->authenticated) {
        return false;
    }

    /* Admin implies all scopes */
    if (result->scopes & SB_SCOPE_ADMIN) {
        return true;
    }

    return (result->scopes & required_scope) != 0;
}

bool sb_auth_rate_check(const char *key_id)
{
    if (key_id == NULL) {
        return true;
    }

    const sb_config_t *config = sb_config_get();
    uint16_t rpm_limit = config ? config->security.rate_limit_rpm : 300;

    /* Find or create rate state for this key */
    rate_state_t *rs = NULL;
    for (uint8_t i = 0; i < MAX_API_KEYS; i++) {
        if (s_rate[i].key_id[0] != '\0' &&
            strcmp(s_rate[i].key_id, key_id) == 0) {
            rs = &s_rate[i];
            break;
        }
    }

    if (rs == NULL) {
        /* Find empty slot */
        for (uint8_t i = 0; i < MAX_API_KEYS; i++) {
            if (s_rate[i].key_id[0] == '\0') {
                rs = &s_rate[i];
                strncpy(rs->key_id, key_id, sizeof(rs->key_id) - 1);
                rs->head = 0;
                rs->count = 0;
                break;
            }
        }
    }

    if (rs == NULL) {
        /* No slots — allow (shouldn't happen) */
        return true;
    }

    /* Current time in seconds (use esp_timer for monotonic time) */
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t window_start = now - RATE_WINDOW_SEC;

    /* Count requests in the current window */
    uint16_t in_window = 0;
    for (uint16_t i = 0; i < rs->count && i < 512; i++) {
        if (rs->request_timestamps[i] >= window_start) {
            in_window++;
        }
    }

    if (in_window >= rpm_limit) {
        return false;
    }

    /* Record this request */
    rs->request_timestamps[rs->head] = now;
    rs->head = (rs->head + 1) % 512;
    if (rs->count < 512) {
        rs->count++;
    }

    return true;
}
