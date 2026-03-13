/**
 * @file test_api_auth.c
 * @brief Unit tests for API key auth middleware.
 *
 * Tests the auth check logic, scope enforcement, and rate limiting.
 * Uses ESP-IDF Unity framework.
 *
 * Task: F18
 */

#include "unity.h"
#include "nvs_flash.h"
#include "api_auth.h"
#include "app_config.h"

#include <string.h>
#include "mbedtls/sha256.h"

/* ── Setup ── */

static void setup_nvs(void)
{
    nvs_flash_erase();
    nvs_flash_init();
}

static void teardown_nvs(void)
{
    nvs_flash_deinit();
}

/* ── Tests ── */

TEST_CASE("auth_init succeeds with empty NVS", "[auth]")
{
    setup_nvs();
    TEST_ASSERT_EQUAL(ESP_OK, sb_auth_init());
    teardown_nvs();
}

TEST_CASE("auth_has_scope returns false for NULL result", "[auth]")
{
    TEST_ASSERT_FALSE(sb_auth_has_scope(NULL, SB_SCOPE_READ));
}

TEST_CASE("auth_has_scope returns false for unauthenticated", "[auth]")
{
    sb_auth_result_t result;
    memset(&result, 0, sizeof(result));
    result.authenticated = false;
    result.scopes = SB_SCOPE_ALL;

    TEST_ASSERT_FALSE(sb_auth_has_scope(&result, SB_SCOPE_READ));
}

TEST_CASE("auth_has_scope checks bitmask correctly", "[auth]")
{
    sb_auth_result_t result;
    memset(&result, 0, sizeof(result));
    result.authenticated = true;
    result.scopes = SB_SCOPE_READ;

    TEST_ASSERT_TRUE(sb_auth_has_scope(&result, SB_SCOPE_READ));
    TEST_ASSERT_FALSE(sb_auth_has_scope(&result, SB_SCOPE_WRITE));
    TEST_ASSERT_FALSE(sb_auth_has_scope(&result, SB_SCOPE_ADMIN));
}

TEST_CASE("auth_has_scope ADMIN implies all scopes", "[auth]")
{
    sb_auth_result_t result;
    memset(&result, 0, sizeof(result));
    result.authenticated = true;
    result.scopes = SB_SCOPE_ADMIN;

    TEST_ASSERT_TRUE(sb_auth_has_scope(&result, SB_SCOPE_READ));
    TEST_ASSERT_TRUE(sb_auth_has_scope(&result, SB_SCOPE_WRITE));
    TEST_ASSERT_TRUE(sb_auth_has_scope(&result, SB_SCOPE_STREAM));
    TEST_ASSERT_TRUE(sb_auth_has_scope(&result, SB_SCOPE_ADMIN));
}

TEST_CASE("auth_has_scope READ+WRITE combo", "[auth]")
{
    sb_auth_result_t result;
    memset(&result, 0, sizeof(result));
    result.authenticated = true;
    result.scopes = SB_SCOPE_READ | SB_SCOPE_WRITE;

    TEST_ASSERT_TRUE(sb_auth_has_scope(&result, SB_SCOPE_READ));
    TEST_ASSERT_TRUE(sb_auth_has_scope(&result, SB_SCOPE_WRITE));
    TEST_ASSERT_FALSE(sb_auth_has_scope(&result, SB_SCOPE_ADMIN));
    TEST_ASSERT_FALSE(sb_auth_has_scope(&result, SB_SCOPE_STREAM));
}

TEST_CASE("rate_check allows requests under limit", "[auth]")
{
    setup_nvs();
    sb_config_init();

    /* Set rate limit to 10 RPM for testing */
    sb_config_t config;
    memset(&config, 0, sizeof(config));
    config.security.rate_limit_rpm = 10;
    sb_config_save(&config);

    sb_auth_init();

    /* First 10 requests should pass */
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_TRUE(sb_auth_rate_check("test-key"));
    }

    teardown_nvs();
}

TEST_CASE("rate_check blocks after limit exceeded", "[auth]")
{
    setup_nvs();
    sb_config_init();

    sb_config_t config;
    memset(&config, 0, sizeof(config));
    config.security.rate_limit_rpm = 5;
    sb_config_save(&config);

    sb_auth_init();

    /* Use up the quota */
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_TRUE(sb_auth_rate_check("rate-test-key"));
    }

    /* Next request should be blocked */
    TEST_ASSERT_FALSE(sb_auth_rate_check("rate-test-key"));

    teardown_nvs();
}

TEST_CASE("rate_check NULL key always passes", "[auth]")
{
    TEST_ASSERT_TRUE(sb_auth_rate_check(NULL));
}

TEST_CASE("rate_check separate keys have independent limits", "[auth]")
{
    setup_nvs();
    sb_config_init();

    sb_config_t config;
    memset(&config, 0, sizeof(config));
    config.security.rate_limit_rpm = 3;
    sb_config_save(&config);

    sb_auth_init();

    /* Exhaust key-a */
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(sb_auth_rate_check("key-a"));
    }
    TEST_ASSERT_FALSE(sb_auth_rate_check("key-a"));

    /* key-b should still work */
    TEST_ASSERT_TRUE(sb_auth_rate_check("key-b"));

    teardown_nvs();
}

TEST_CASE("auth_check returns INVALID_ARG for NULL inputs", "[auth]")
{
    sb_auth_result_t result;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sb_auth_check(NULL, &result));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sb_auth_check(NULL, NULL));
}
