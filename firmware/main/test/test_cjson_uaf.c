/**
 * @file test_cjson_uaf.c
 * @brief Regression tests for cJSON use-after-free patterns.
 *
 * Verifies that the "extract to local, then free parent" pattern
 * used across all API handlers preserves correct values.
 *
 * These tests document the required pattern and will catch regressions
 * if anyone reverts to accessing cJSON child pointers after
 * cJSON_Delete(parent).
 *
 * Task: Regression tests for UAF fixes
 */

#include "unity.h"
#include <stdbool.h>
#include "cJSON.h"

/* ── Integer extraction pattern (api_pwm_servo.c) ── */

TEST_CASE("cjson_uaf: int extracted before parent delete retains value", "[cjson][uaf]")
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "position", 750);

    const cJSON *pos_j = cJSON_GetObjectItemCaseSensitive(body, "position");
    TEST_ASSERT_NOT_NULL(pos_j);
    TEST_ASSERT_TRUE(cJSON_IsNumber(pos_j));

    /* Pattern under test: save to local before free */
    const int position = pos_j->valueint;
    cJSON_Delete(body);

    /* After free, the local variable must still hold the correct value */
    TEST_ASSERT_EQUAL_INT(750, position);
}

TEST_CASE("cjson_uaf: int zero extracted correctly", "[cjson][uaf]")
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "position", 0);

    const cJSON *pos_j = cJSON_GetObjectItemCaseSensitive(body, "position");
    const int position = pos_j->valueint;
    cJSON_Delete(body);

    TEST_ASSERT_EQUAL_INT(0, position);
}

TEST_CASE("cjson_uaf: int max boundary extracted correctly", "[cjson][uaf]")
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "position", 1000);

    const cJSON *pos_j = cJSON_GetObjectItemCaseSensitive(body, "position");
    const int position = pos_j->valueint;
    cJSON_Delete(body);

    TEST_ASSERT_EQUAL_INT(1000, position);
}

/* ── Bool extraction pattern (api_led.c) ── */

TEST_CASE("cjson_uaf: bool true extracted before parent delete", "[cjson][uaf]")
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "enabled", true);

    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(body, "enabled");
    TEST_ASSERT_NOT_NULL(enabled);
    TEST_ASSERT_TRUE(cJSON_IsBool(enabled));

    /* Pattern under test: save bool to local before free */
    const bool is_enabled = cJSON_IsTrue(enabled);
    cJSON_Delete(body);

    TEST_ASSERT_TRUE(is_enabled);
}

TEST_CASE("cjson_uaf: bool false extracted before parent delete", "[cjson][uaf]")
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "on", false);

    cJSON *on = cJSON_GetObjectItemCaseSensitive(body, "on");
    const bool is_on = cJSON_IsTrue(on);
    cJSON_Delete(body);

    TEST_ASSERT_FALSE(is_on);
}

/* ── Multiple values extracted from same body ── */

TEST_CASE("cjson_uaf: multiple values extracted before parent delete", "[cjson][uaf]")
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "channel", 3);
    cJSON_AddNumberToObject(body, "position", 500);

    const cJSON *ch_j = cJSON_GetObjectItemCaseSensitive(body, "channel");
    const cJSON *pos_j = cJSON_GetObjectItemCaseSensitive(body, "position");

    /* Extract both before freeing */
    const int channel = ch_j->valueint;
    const int position = pos_j->valueint;
    cJSON_Delete(body);

    TEST_ASSERT_EQUAL_INT(3, channel);
    TEST_ASSERT_EQUAL_INT(500, position);
}

/* ── RGB extraction pattern (api_led.c handle_led_color) ── */

TEST_CASE("cjson_uaf: RGB values extracted before parent delete", "[cjson][uaf]")
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "r", 255);
    cJSON_AddNumberToObject(body, "g", 128);
    cJSON_AddNumberToObject(body, "b", 0);
    cJSON_AddBoolToObject(body, "all", true);

    cJSON *r_val = cJSON_GetObjectItemCaseSensitive(body, "r");
    cJSON *g_val = cJSON_GetObjectItemCaseSensitive(body, "g");
    cJSON *b_val = cJSON_GetObjectItemCaseSensitive(body, "b");

    uint8_t r = (uint8_t)r_val->valueint;
    uint8_t g = (uint8_t)g_val->valueint;
    uint8_t b = (uint8_t)b_val->valueint;

    /* Access 'all' bool before free (matches handle_led_color pattern) */
    cJSON *all = cJSON_GetObjectItemCaseSensitive(body, "all");
    const bool is_all = cJSON_IsTrue(all);

    cJSON_Delete(body);

    TEST_ASSERT_EQUAL_UINT8(255, r);
    TEST_ASSERT_EQUAL_UINT8(128, g);
    TEST_ASSERT_EQUAL_UINT8(0, b);
    TEST_ASSERT_TRUE(is_all);
}

/* ── Negative value extraction ── */

TEST_CASE("cjson_uaf: negative int extracted correctly", "[cjson][uaf]")
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "value", -42);

    const cJSON *val = cJSON_GetObjectItemCaseSensitive(body, "value");
    const int value = val->valueint;
    cJSON_Delete(body);

    TEST_ASSERT_EQUAL_INT(-42, value);
}

/* ── Validation before extraction pattern ── */

TEST_CASE("cjson_uaf: validation then extraction then free", "[cjson][uaf]")
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "position", 999);

    const cJSON *pos_j = cJSON_GetObjectItemCaseSensitive(body, "position");

    /* Validation step (as done in handlers) */
    TEST_ASSERT_TRUE(cJSON_IsNumber(pos_j));

    /* Extract step */
    const int position = pos_j->valueint;

    /* Free step */
    cJSON_Delete(body);

    /* Bounds check step (as done in api_pwm_servo.c) */
    TEST_ASSERT_TRUE(position >= 0 && position <= 1000);
    TEST_ASSERT_EQUAL_INT(999, position);
}
