/**
 * @file api_auth.h
 * @brief API key authentication middleware.
 *
 * Validates Bearer tokens against stored API key hashes.
 * Enforces scopes per endpoint. Rate limiting per key.
 *
 * Task: F01 (structure), F04 (implementation)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** API key scopes (bitfield). */
typedef enum {
    SB_SCOPE_READ   = (1 << 0),
    SB_SCOPE_WRITE  = (1 << 1),
    SB_SCOPE_ADMIN  = (1 << 2),
    SB_SCOPE_STREAM = (1 << 3),
    SB_SCOPE_ALL    = 0x0F,
} sb_scope_t;

/** Authentication result. */
typedef struct {
    bool authenticated;
    uint8_t scopes;            /**< Bitmask of sb_scope_t */
    char key_id[64];           /**< Key identifier (prefix) */
} sb_auth_result_t;

/**
 * Initialize the auth system (load keys from NVS).
 *
 * @return ESP_OK on success.
 */
esp_err_t sb_auth_init(void);

/**
 * Authenticate an HTTP request.
 *
 * Extracts the Bearer token from the Authorization header
 * and validates against stored key hashes.
 *
 * @param req     HTTP request.
 * @param[out] result  Authentication result.
 * @return ESP_OK if authenticated, ESP_ERR_INVALID_STATE if not.
 */
esp_err_t sb_auth_check(httpd_req_t *req, sb_auth_result_t *result);

/**
 * Check if a request has the required scope.
 *
 * @param result          Auth result from sb_auth_check.
 * @param required_scope  Required scope bitmask.
 * @return true if authorized.
 */
bool sb_auth_has_scope(const sb_auth_result_t *result, sb_scope_t required_scope);

/**
 * Check rate limit for the authenticated key.
 *
 * @param key_id  Key identifier.
 * @return true if within limit, false if rate exceeded.
 */
bool sb_auth_rate_check(const char *key_id);

#ifdef __cplusplus
}
#endif
