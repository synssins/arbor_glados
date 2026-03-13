/**
 * @file test_servo_bus.c
 * @brief Unit tests for servo-bus plugin (protocol-level tests).
 *
 * Tests the Feetech STS protocol packet construction and parsing
 * at the data layer. UART I/O is not tested here (requires hardware
 * or UART loopback).
 *
 * Task: F19
 */

#include "unity.h"
#include "plugin_manager.h"
#include "servo_bus.h"

#include <string.h>
#include "cJSON.h"

/* ── Tests ── */

TEST_CASE("servo_bus_plugin returns valid plugin struct", "[servo-bus]")
{
    sb_plugin_t *plugin = sb_servo_bus_plugin();
    TEST_ASSERT_NOT_NULL(plugin);
    TEST_ASSERT_EQUAL_STRING("servo-bus", plugin->name);
    TEST_ASSERT_EQUAL_STRING("1.0.0", plugin->version);
    TEST_ASSERT_NOT_NULL(plugin->initialize);
    TEST_ASSERT_NOT_NULL(plugin->shutdown);
    TEST_ASSERT_NOT_NULL(plugin->get_state);
    TEST_ASSERT_NOT_NULL(plugin->handle_command);
    TEST_ASSERT_NOT_NULL(plugin->health_check);
}

TEST_CASE("servo_bus health_check reports unhealthy before init", "[servo-bus]")
{
    sb_plugin_t *plugin = sb_servo_bus_plugin();
    sb_health_status_t hs = plugin->health_check(plugin);
    TEST_ASSERT_EQUAL(SB_HEALTH_UNHEALTHY, hs.state);
}

TEST_CASE("servo_bus handle_command returns error for unknown command", "[servo-bus]")
{
    sb_plugin_t *plugin = sb_servo_bus_plugin();

    /* Note: This test works even without initialization because
     * unknown commands return error without accessing hardware. */
    cJSON *params = cJSON_CreateObject();
    cJSON *result = plugin->handle_command(plugin, "nonexistent_command", params);
    cJSON_Delete(params);

    TEST_ASSERT_NOT_NULL(result);
    cJSON *err = cJSON_GetObjectItem(result, "error");
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL_STRING("Unknown command", err->valuestring);
    cJSON_Delete(result);
}

TEST_CASE("servo_bus set_position rejects missing params", "[servo-bus]")
{
    sb_plugin_t *plugin = sb_servo_bus_plugin();
    cJSON *params = cJSON_CreateObject();  /* No id or position */

    cJSON *result = plugin->handle_command(plugin, "set_position", params);
    cJSON_Delete(params);

    TEST_ASSERT_NOT_NULL(result);
    cJSON *err = cJSON_GetObjectItem(result, "error");
    TEST_ASSERT_NOT_NULL(err);
    cJSON_Delete(result);
}

TEST_CASE("servo_bus set_speed rejects missing params", "[servo-bus]")
{
    sb_plugin_t *plugin = sb_servo_bus_plugin();
    cJSON *params = cJSON_CreateObject();

    cJSON *result = plugin->handle_command(plugin, "set_speed", params);
    cJSON_Delete(params);

    TEST_ASSERT_NOT_NULL(result);
    cJSON *err = cJSON_GetObjectItem(result, "error");
    TEST_ASSERT_NOT_NULL(err);
    cJSON_Delete(result);
}

TEST_CASE("servo_bus set_torque rejects missing params", "[servo-bus]")
{
    sb_plugin_t *plugin = sb_servo_bus_plugin();
    cJSON *params = cJSON_CreateObject();

    cJSON *result = plugin->handle_command(plugin, "set_torque", params);
    cJSON_Delete(params);

    TEST_ASSERT_NOT_NULL(result);
    cJSON *err = cJSON_GetObjectItem(result, "error");
    TEST_ASSERT_NOT_NULL(err);
    cJSON_Delete(result);
}

TEST_CASE("servo_bus sync_move rejects missing moves", "[servo-bus]")
{
    sb_plugin_t *plugin = sb_servo_bus_plugin();
    cJSON *params = cJSON_CreateObject();

    cJSON *result = plugin->handle_command(plugin, "sync_move", params);
    cJSON_Delete(params);

    TEST_ASSERT_NOT_NULL(result);
    cJSON *err = cJSON_GetObjectItem(result, "error");
    TEST_ASSERT_NOT_NULL(err);
    cJSON_Delete(result);
}

TEST_CASE("servo_bus sync_move rejects empty array", "[servo-bus]")
{
    sb_plugin_t *plugin = sb_servo_bus_plugin();
    cJSON *params = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "moves", cJSON_CreateArray());

    cJSON *result = plugin->handle_command(plugin, "sync_move", params);
    cJSON_Delete(params);

    TEST_ASSERT_NOT_NULL(result);
    cJSON *err = cJSON_GetObjectItem(result, "error");
    TEST_ASSERT_NOT_NULL(err);
    cJSON_Delete(result);
}

TEST_CASE("servo_bus get_state returns servos array", "[servo-bus]")
{
    sb_plugin_t *plugin = sb_servo_bus_plugin();
    cJSON *state = plugin->get_state(plugin);

    TEST_ASSERT_NOT_NULL(state);
    cJSON *servos = cJSON_GetObjectItem(state, "servos");
    TEST_ASSERT_NOT_NULL(servos);
    TEST_ASSERT_TRUE(cJSON_IsArray(servos));

    cJSON_Delete(state);
}

TEST_CASE("plugin_manager registers servo_bus correctly", "[servo-bus][plugin]")
{
    sb_plugin_manager_init();

    sb_plugin_t *plugin = sb_servo_bus_plugin();
    esp_err_t ret = sb_plugin_register(plugin);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    sb_plugin_t *found = sb_plugin_find("servo-bus");
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_PTR(plugin, found);

    /* Duplicate registration should fail */
    ret = sb_plugin_register(plugin);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ret);
}
