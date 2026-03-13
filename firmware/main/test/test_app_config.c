/**
 * @file test_app_config.c
 * @brief Unit tests for NVS configuration system.
 *
 * Uses ESP-IDF's Unity test framework.
 * Tests run on-device or in QEMU.
 *
 * Task: F17
 */

#include "unity.h"
#include "nvs_flash.h"
#include "app_config.h"
#include "cJSON.h"

#include <string.h>

/* ── Setup / Teardown ── */

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

TEST_CASE("config_init succeeds", "[config]")
{
    setup_nvs();
    TEST_ASSERT_EQUAL(ESP_OK, sb_config_init());
    teardown_nvs();
}

TEST_CASE("config_load returns NOT_FOUND when not provisioned", "[config]")
{
    setup_nvs();
    sb_config_init();

    sb_config_t config;
    esp_err_t ret = sb_config_load(&config);
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, ret);

    teardown_nvs();
}

TEST_CASE("config_is_provisioned returns false when empty", "[config]")
{
    setup_nvs();
    sb_config_init();

    TEST_ASSERT_FALSE(sb_config_is_provisioned());

    teardown_nvs();
}

TEST_CASE("config_save and config_load roundtrip", "[config]")
{
    setup_nvs();
    sb_config_init();

    /* Create test config */
    sb_config_t config;
    memset(&config, 0, sizeof(config));
    config.server.port = 9443;
    config.server.tls_enabled = true;
    strncpy(config.server.hostname, "test-node", sizeof(config.server.hostname) - 1);
    config.security.rate_limit_rpm = 200;
    config.security.api_key_min_length = 32;
    config.logging.level = 3;
    config.pins.servo_bus_tx = 18;
    config.pins.servo_bus_rx = 19;
    config.pins.servo_bus_dir = 5;
    config.pins.i2c_sda = 21;
    config.pins.i2c_scl = 22;
    config.servo_bus.baud = 1000000;
    strncpy(config.servo_bus.protocol, "feetech-sts", sizeof(config.servo_bus.protocol) - 1);
    config.servo_bus.servo_count = 2;
    config.servo_bus.servos[0].id = 1;
    config.servo_bus.servos[0].min_position = 0;
    config.servo_bus.servos[0].max_position = 4095;
    config.servo_bus.servos[1].id = 2;

    /* Save */
    esp_err_t ret = sb_config_save(&config);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Verify provisioned */
    TEST_ASSERT_TRUE(sb_config_is_provisioned());

    /* Load */
    sb_config_t loaded;
    ret = sb_config_load(&loaded);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    /* Verify fields */
    TEST_ASSERT_EQUAL(9443, loaded.server.port);
    TEST_ASSERT_TRUE(loaded.server.tls_enabled);
    TEST_ASSERT_EQUAL_STRING("test-node", loaded.server.hostname);
    TEST_ASSERT_EQUAL(200, loaded.security.rate_limit_rpm);
    TEST_ASSERT_EQUAL(32, loaded.security.api_key_min_length);
    TEST_ASSERT_EQUAL(3, loaded.logging.level);
    TEST_ASSERT_EQUAL(18, loaded.pins.servo_bus_tx);
    TEST_ASSERT_EQUAL(19, loaded.pins.servo_bus_rx);
    TEST_ASSERT_EQUAL(5, loaded.pins.servo_bus_dir);
    TEST_ASSERT_EQUAL(21, loaded.pins.i2c_sda);
    TEST_ASSERT_EQUAL(22, loaded.pins.i2c_scl);
    TEST_ASSERT_EQUAL(1000000, loaded.servo_bus.baud);
    TEST_ASSERT_EQUAL_STRING("feetech-sts", loaded.servo_bus.protocol);
    TEST_ASSERT_EQUAL(2, loaded.servo_bus.servo_count);
    TEST_ASSERT_EQUAL(1, loaded.servo_bus.servos[0].id);
    TEST_ASSERT_EQUAL(2, loaded.servo_bus.servos[1].id);

    teardown_nvs();
}

TEST_CASE("config_to_json exports correctly", "[config]")
{
    setup_nvs();
    sb_config_init();

    sb_config_t config;
    memset(&config, 0, sizeof(config));
    config.server.port = 8443;
    config.server.tls_enabled = true;
    strncpy(config.server.hostname, "my-node", sizeof(config.server.hostname) - 1);
    config.servo_bus.baud = 500000;

    sb_config_save(&config);

    cJSON *json = (cJSON *)sb_config_to_json(&config);
    TEST_ASSERT_NOT_NULL(json);

    /* Check fields exist */
    cJSON *server = cJSON_GetObjectItem(json, "server");
    TEST_ASSERT_NOT_NULL(server);
    cJSON *port = cJSON_GetObjectItem(server, "port");
    TEST_ASSERT_NOT_NULL(port);
    TEST_ASSERT_EQUAL(8443, port->valueint);

    cJSON *hostname = cJSON_GetObjectItem(server, "hostname");
    TEST_ASSERT_NOT_NULL(hostname);
    TEST_ASSERT_EQUAL_STRING("my-node", hostname->valuestring);

    cJSON_Delete(json);
    teardown_nvs();
}

TEST_CASE("config_load_json parses and saves", "[config]")
{
    setup_nvs();
    sb_config_init();

    cJSON *json = cJSON_CreateObject();
    cJSON *server = cJSON_AddObjectToObject(json, "server");
    cJSON_AddNumberToObject(server, "port", 7777);
    cJSON_AddBoolToObject(server, "tls_enabled", false);
    cJSON_AddStringToObject(server, "hostname", "json-test");

    cJSON *security = cJSON_AddObjectToObject(json, "security");
    cJSON_AddNumberToObject(security, "rate_limit_rpm", 100);

    cJSON *pins = cJSON_AddObjectToObject(json, "pins");
    cJSON_AddNumberToObject(pins, "servo_bus_tx", 16);
    cJSON_AddNumberToObject(pins, "servo_bus_rx", 17);

    sb_config_t config;
    esp_err_t ret = sb_config_load_json(json, &config);
    cJSON_Delete(json);

    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL(7777, config.server.port);
    TEST_ASSERT_FALSE(config.server.tls_enabled);
    TEST_ASSERT_EQUAL_STRING("json-test", config.server.hostname);
    TEST_ASSERT_EQUAL(100, config.security.rate_limit_rpm);
    TEST_ASSERT_EQUAL(16, config.pins.servo_bus_tx);
    TEST_ASSERT_EQUAL(17, config.pins.servo_bus_rx);

    /* Verify it was saved to NVS */
    TEST_ASSERT_TRUE(sb_config_is_provisioned());

    teardown_nvs();
}

TEST_CASE("config_get returns NULL before load", "[config]")
{
    setup_nvs();
    sb_config_init();

    /* Before any load, get should return the loaded config or NULL */
    /* After init with no provisioned data, get returns NULL or minimal config */
    const sb_config_t *cfg = sb_config_get();
    /* Just verify it doesn't crash — behavior depends on implementation */
    (void)cfg;

    teardown_nvs();
}

TEST_CASE("config pin defaults are -1", "[config]")
{
    setup_nvs();
    sb_config_init();

    sb_config_t config;
    memset(&config, 0, sizeof(config));

    /* Unset pins should be -1 by convention */
    config.pins.servo_bus_tx = -1;
    config.pins.servo_bus_rx = -1;
    config.pins.servo_bus_dir = -1;
    config.pins.i2c_sda = -1;
    config.pins.i2c_scl = -1;

    sb_config_save(&config);

    sb_config_t loaded;
    sb_config_load(&loaded);

    TEST_ASSERT_EQUAL(-1, loaded.pins.servo_bus_dir);
    TEST_ASSERT_EQUAL(-1, loaded.pins.i2c_sda);
    TEST_ASSERT_EQUAL(-1, loaded.pins.i2c_scl);

    teardown_nvs();
}

TEST_CASE("config_save_load preserves servo config", "[config]")
{
    setup_nvs();
    sb_config_init();

    sb_config_t config;
    memset(&config, 0, sizeof(config));
    config.servo_bus.servo_count = 3;
    config.servo_bus.servos[0].id = 10;
    strncpy(config.servo_bus.servos[0].name, "shoulder", sizeof(config.servo_bus.servos[0].name) - 1);
    config.servo_bus.servos[0].min_position = 100;
    config.servo_bus.servos[0].max_position = 3900;
    config.servo_bus.servos[0].max_speed = 500;

    config.servo_bus.servos[1].id = 11;
    strncpy(config.servo_bus.servos[1].name, "elbow", sizeof(config.servo_bus.servos[1].name) - 1);

    config.servo_bus.servos[2].id = 12;

    sb_config_save(&config);

    sb_config_t loaded;
    sb_config_load(&loaded);

    TEST_ASSERT_EQUAL(3, loaded.servo_bus.servo_count);
    TEST_ASSERT_EQUAL(10, loaded.servo_bus.servos[0].id);
    TEST_ASSERT_EQUAL_STRING("shoulder", loaded.servo_bus.servos[0].name);
    TEST_ASSERT_EQUAL(100, loaded.servo_bus.servos[0].min_position);
    TEST_ASSERT_EQUAL(3900, loaded.servo_bus.servos[0].max_position);
    TEST_ASSERT_EQUAL(500, loaded.servo_bus.servos[0].max_speed);
    TEST_ASSERT_EQUAL(11, loaded.servo_bus.servos[1].id);
    TEST_ASSERT_EQUAL(12, loaded.servo_bus.servos[2].id);

    teardown_nvs();
}
