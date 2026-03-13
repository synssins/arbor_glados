/**
 * @file servo_bus.c
 * @brief Feetech STS/SCS serial bus servo driver.
 *
 * Half-duplex UART at configurable baud (default 1Mbps).
 * Packet format: [0xFF][0xFF][ID][Len][Instruction][Params...][Checksum]
 *
 * Pin assignments come from NVS config — no hardcoded GPIOs.
 *
 * Task: F06
 */

#include "servo_bus.h"
#include "plugin_manager.h"
#include "app_config.h"
#include "event_bus.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "cJSON.h"

static const char *TAG = "servo_bus";

/* ── Feetech STS/SCS Protocol Constants ── */

#define STS_HEADER_0          0xFF
#define STS_HEADER_1          0xFF

/* Instructions */
#define STS_INST_PING         0x01
#define STS_INST_READ         0x02
#define STS_INST_WRITE        0x03
#define STS_INST_REG_WRITE    0x04
#define STS_INST_ACTION       0x05
#define STS_INST_RESET        0x06
#define STS_INST_SYNC_WRITE   0x83

/* Register addresses — ST3215 memory table v3.6 */
#define STS_REG_ID              5   /* 1 byte, EEPROM */
#define STS_REG_MODE            33  /* 1 byte, EEPROM: 0=pos, 1=speed, 2=PWM, 3=step */
#define STS_REG_TORQUE_ENABLE   40
#define STS_REG_GOAL_POSITION   42  /* 2 bytes */
#define STS_REG_GOAL_SPEED      46  /* 2 bytes */
#define STS_REG_TORQUE_LIMIT    48  /* 2 bytes, SRAM */
#define STS_REG_LOCK            55  /* 1 byte, SRAM: 0=unlock EEPROM, 1=lock */
#define STS_REG_PRESENT_POS     56  /* 2 bytes */
#define STS_REG_PRESENT_SPEED   58  /* 2 bytes */
#define STS_REG_PRESENT_LOAD    60  /* 2 bytes */
#define STS_REG_PRESENT_VOLTAGE 62  /* 1 byte */
#define STS_REG_PRESENT_TEMP    63  /* 1 byte */
#define STS_REG_MOVING          66  /* 1 byte */

/* UART config */
#define UART_NUM              UART_NUM_1
#define UART_BUF_SIZE         256
#define TX_TIMEOUT_MS         10
#define RX_TIMEOUT_MS         20

/* Maximum servos on a single bus */
#define MAX_BUS_SERVOS        32

/* ── Private State ── */

typedef struct {
    bool initialized;
    int tx_pin;
    int rx_pin;
    int dir_pin;          /* -1 if not used (software half-duplex) */
    uint32_t baud;
    uint8_t servo_ids[MAX_BUS_SERVOS];
    uint8_t servo_count;
    sb_servo_state_t servo_states[MAX_BUS_SERVOS];
} servo_bus_ctx_t;

static servo_bus_ctx_t s_ctx;
static sb_plugin_t s_plugin;

/* ── Low-Level Protocol ── */

/**
 * Compute Feetech checksum: ~(ID + Length + Instruction + Params...) & 0xFF
 */
static uint8_t compute_checksum(const uint8_t *packet, size_t len)
{
    uint8_t sum = 0;
    /* Sum from ID field onward (skip the two 0xFF headers) */
    for (size_t i = 2; i < len; i++) {
        sum += packet[i];
    }
    return ~sum;
}

/**
 * Set direction pin for half-duplex TX.
 */
static void set_direction_tx(void)
{
    if (s_ctx.dir_pin >= 0) {
        gpio_set_level((gpio_num_t)s_ctx.dir_pin, 1);
    }
}

/**
 * Set direction pin for half-duplex RX.
 */
static void set_direction_rx(void)
{
    if (s_ctx.dir_pin >= 0) {
        gpio_set_level((gpio_num_t)s_ctx.dir_pin, 0);
    }
}

/**
 * Send a raw packet over UART.
 * Packet must include headers and checksum.
 */
static esp_err_t bus_send(const uint8_t *data, size_t len)
{
    set_direction_tx();
    /* Small delay for direction pin settling */
    esp_rom_delay_us(5);

    int written = uart_write_bytes(UART_NUM, data, len);
    if (written < 0 || (size_t)written != len) {
        set_direction_rx();
        return ESP_FAIL;
    }

    /* Wait for TX to complete */
    esp_err_t ret = uart_wait_tx_done(UART_NUM, pdMS_TO_TICKS(TX_TIMEOUT_MS));
    set_direction_rx();
    return ret;
}

/**
 * Receive a response packet from a servo.
 * Returns the number of bytes read, or 0 on timeout/error.
 */
static int bus_recv(uint8_t *buf, size_t max_len)
{
    int total = 0;

    /* Read header bytes */
    int n = uart_read_bytes(UART_NUM, buf, 4, pdMS_TO_TICKS(RX_TIMEOUT_MS));
    if (n < 4 || buf[0] != STS_HEADER_0 || buf[1] != STS_HEADER_1) {
        return 0;
    }
    total = 4;

    /* buf[3] is Length field — remaining bytes = Length - 1 (status already in buf[4..]) + checksum */
    /* Feetech: response Length = num_params + 2. Remaining after header+id+len = Length bytes total.
     * We already read 4 bytes (H, H, ID, Len). Need to read Len more bytes (error + params + checksum). */
    if (buf[3] > (max_len - 4)) {
        uart_flush_input(UART_NUM);
        return 0;
    }

    n = uart_read_bytes(UART_NUM, buf + 4, buf[3], pdMS_TO_TICKS(RX_TIMEOUT_MS));
    if (n < buf[3]) {
        return 0;
    }
    total += n;

    /* Verify checksum */
    uint8_t expected = compute_checksum(buf, total - 1);
    if (buf[total - 1] != expected) {
        ESP_LOGW(TAG, "Checksum mismatch: got 0x%02x, expected 0x%02x", buf[total - 1], expected);
        return 0;
    }

    return total;
}

/**
 * Build and send an instruction packet.
 * Returns ESP_OK on successful send.
 */
static esp_err_t send_instruction(uint8_t id, uint8_t instruction,
                                  const uint8_t *params, uint8_t param_count)
{
    uint8_t pkt[256];
    uint8_t len = param_count + 2; /* instruction + params + checksum = Length field */

    pkt[0] = STS_HEADER_0;
    pkt[1] = STS_HEADER_1;
    pkt[2] = id;
    pkt[3] = len;
    pkt[4] = instruction;
    if (param_count > 0 && params != NULL) {
        memcpy(&pkt[5], params, param_count);
    }
    pkt[5 + param_count] = compute_checksum(pkt, 5 + param_count);

    /* Flush RX before sending to clear stale data */
    uart_flush_input(UART_NUM);

    return bus_send(pkt, 6 + param_count);
}

/**
 * Ping a servo ID. Returns true if it responds.
 */
static bool ping_servo(uint8_t id)
{
    esp_err_t ret = send_instruction(id, STS_INST_PING, NULL, 0);
    if (ret != ESP_OK) return false;

    uint8_t resp[16];
    int n = bus_recv(resp, sizeof(resp));
    return (n >= 6 && resp[2] == id);
}

/**
 * Read registers from a servo.
 */
static int read_registers(uint8_t id, uint8_t start_addr, uint8_t count,
                          uint8_t *out, size_t out_size)
{
    uint8_t params[2] = { start_addr, count };
    esp_err_t ret = send_instruction(id, STS_INST_READ, params, 2);
    if (ret != ESP_OK) return -1;

    uint8_t resp[64];
    int n = bus_recv(resp, sizeof(resp));
    if (n < 6) return -1;

    /* Response: [FF FF ID Len Error Param0 Param1 ... Checksum]
     * The Error byte is the servo's current alarm status bitmask, NOT a
     * "read failed" indicator.  Data is still valid when alarms are active.
     *   Bit 0: Voltage     Bit 1: Angle sensor   Bit 2: Overheating
     *   Bit 3: Range       Bit 4: Checksum error  Bit 5: Overload
     *   Bit 6: Instruction error
     * Only bits 4 (checksum) and 6 (instruction) indicate the response
     * data itself is unreliable.  Alarm bits (0-3, 5) are informational. */
    uint8_t error = resp[4];
    if (error & 0x50) { /* bit 4 or bit 6: actual protocol failure */
        ESP_LOGW(TAG, "Servo %d protocol error: 0x%02x", id, error);
        return -1;
    }
    if (error) {
        ESP_LOGD(TAG, "Servo %d alarm status: 0x%02x", id, error);
    }

    uint8_t data_count = resp[3] - 2; /* Length - error - checksum */
    if (data_count > out_size) data_count = (uint8_t)out_size;
    memcpy(out, &resp[5], data_count);
    return data_count;
}

/**
 * Write registers to a servo.
 */
static esp_err_t write_registers(uint8_t id, uint8_t start_addr,
                                 const uint8_t *data, uint8_t count)
{
    uint8_t params[64];
    params[0] = start_addr;
    memcpy(&params[1], data, count);

    return send_instruction(id, STS_INST_WRITE, params, count + 1);
}

/**
 * Write a 16-bit value (little-endian) to a register pair.
 */
static esp_err_t write_u16(uint8_t id, uint8_t addr, uint16_t value)
{
    uint8_t data[2] = { (uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF) };
    return write_registers(id, addr, data, 2);
}

/* ── High-Level Servo Operations ── */

/**
 * Read full state from a single servo.
 */
static esp_err_t read_servo_state(uint8_t id, sb_servo_state_t *state)
{
    /* Read position through temperature in one block (56..63 = 8 bytes) */
    uint8_t data[8];
    int n = read_registers(id, STS_REG_PRESENT_POS, 8, data, sizeof(data));
    if (n < 8) return ESP_FAIL;

    state->position    = (uint16_t)(data[0] | (data[1] << 8));
    state->speed       = (int16_t)(data[2] | (data[3] << 8));
    state->load        = (uint16_t)(data[4] | (data[5] << 8));
    state->voltage     = data[6];
    state->temperature = data[7];

    /* Read torque enable separately */
    uint8_t torque;
    n = read_registers(id, STS_REG_TORQUE_ENABLE, 1, &torque, 1);
    state->torque_on = (n >= 1 && torque != 0);

    return ESP_OK;
}

/**
 * Set goal position for a servo.
 */
static esp_err_t set_position(uint8_t id, uint16_t position)
{
    return write_u16(id, STS_REG_GOAL_POSITION, position);
}

/**
 * Set moving speed for a servo.
 */
static esp_err_t set_speed(uint8_t id, uint16_t speed)
{
    return write_u16(id, STS_REG_GOAL_SPEED, speed);
}

/**
 * Enable or disable torque.
 */
static esp_err_t set_torque(uint8_t id, bool enable)
{
    uint8_t val = enable ? 1 : 0;
    return write_registers(id, STS_REG_TORQUE_ENABLE, &val, 1);
}

/**
 * Sync write: write same register to multiple servos in one packet.
 */
static esp_err_t sync_write_positions(const uint8_t *ids, const uint16_t *positions,
                                      uint8_t count)
{
    if (count == 0 || count > MAX_BUS_SERVOS) return ESP_ERR_INVALID_ARG;

    /* Sync write format:
     * [FF FF FE Len SYNC_WRITE StartAddr DataLen ID1 Data1L Data1H ID2 ... Checksum] */
    uint8_t pkt[256];
    uint8_t data_len = 2; /* bytes per servo (position is 2 bytes) */
    uint8_t param_count = (count * (data_len + 1)) + 2; /* +2 for start_addr and data_len */
    uint8_t total_len = param_count + 2; /* instruction + params + checksum */

    pkt[0] = STS_HEADER_0;
    pkt[1] = STS_HEADER_1;
    pkt[2] = 0xFE; /* Broadcast ID */
    pkt[3] = total_len;
    pkt[4] = STS_INST_SYNC_WRITE;
    pkt[5] = STS_REG_GOAL_POSITION;
    pkt[6] = data_len;

    uint8_t idx = 7;
    for (uint8_t i = 0; i < count; i++) {
        pkt[idx++] = ids[i];
        pkt[idx++] = (uint8_t)(positions[i] & 0xFF);
        pkt[idx++] = (uint8_t)((positions[i] >> 8) & 0xFF);
    }
    pkt[idx] = compute_checksum(pkt, idx);

    uart_flush_input(UART_NUM);
    return bus_send(pkt, idx + 1);
}

/**
 * Emergency stop: disable torque on ALL known servos immediately.
 */
static esp_err_t emergency_stop_all(void)
{
    esp_err_t last_err = ESP_OK;
    for (uint8_t i = 0; i < s_ctx.servo_count; i++) {
        esp_err_t err = set_torque(s_ctx.servo_ids[i], false);
        if (err != ESP_OK) {
            last_err = err;
            ESP_LOGE(TAG, "Failed to disable torque on servo %d", s_ctx.servo_ids[i]);
        }
    }
    return last_err;
}

/**
 * Scan the bus for connected servos (ping IDs 0-253).
 */
static cJSON *scan_bus(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) return NULL;

    for (uint8_t id = 0; id < 254; id++) {
        if (ping_servo(id)) {
            cJSON_AddItemToArray(arr, cJSON_CreateNumber(id));
            ESP_LOGI(TAG, "Found servo at ID %d", id);
        }
    }
    return arr;
}

/* ── Plugin Interface ── */

static esp_err_t plugin_init(sb_plugin_t *self, const cJSON *config)
{
    (void)self;
    memset(&s_ctx, 0, sizeof(s_ctx));

    const sb_config_t *app_cfg = sb_config_get();
    if (app_cfg == NULL) {
        ESP_LOGE(TAG, "No app config — cannot initialize servo bus");
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.tx_pin  = app_cfg->pins.servo_bus_tx;
    s_ctx.rx_pin  = app_cfg->pins.servo_bus_rx;
    s_ctx.dir_pin = app_cfg->pins.servo_bus_dir;
    s_ctx.baud    = app_cfg->servo_bus.baud > 0 ? app_cfg->servo_bus.baud : 1000000;

    if (s_ctx.tx_pin < 0 || s_ctx.rx_pin < 0) {
        ESP_LOGE(TAG, "Servo bus TX/RX pins not configured");
        return ESP_ERR_INVALID_STATE;
    }

    /* Configure UART */
    uart_config_t uart_cfg = {
        .baud_rate  = (int)s_ctx.baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_param_config(UART_NUM, &uart_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_set_pin(UART_NUM, s_ctx.tx_pin, s_ctx.rx_pin,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART pin set failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_driver_install(UART_NUM, UART_BUF_SIZE * 2, UART_BUF_SIZE * 2,
                              0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure direction pin if used */
    if (s_ctx.dir_pin >= 0) {
        gpio_config_t io_cfg = {
            .pin_bit_mask = (1ULL << s_ctx.dir_pin),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_cfg);
        set_direction_rx();
    }

    /* Load servo IDs from config */
    s_ctx.servo_count = app_cfg->servo_bus.servo_count;
    if (s_ctx.servo_count > MAX_BUS_SERVOS) {
        s_ctx.servo_count = MAX_BUS_SERVOS;
    }
    for (uint8_t i = 0; i < s_ctx.servo_count; i++) {
        s_ctx.servo_ids[i] = app_cfg->servo_bus.servos[i].id;
    }

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "Servo bus initialized: TX=%d RX=%d DIR=%d baud=%lu, %d servos",
             s_ctx.tx_pin, s_ctx.rx_pin, s_ctx.dir_pin,
             (unsigned long)s_ctx.baud, s_ctx.servo_count);

    return ESP_OK;
}

static esp_err_t plugin_shutdown(sb_plugin_t *self)
{
    (void)self;
    if (s_ctx.initialized) {
        /* Disable torque on all servos before shutting down */
        emergency_stop_all();
        uart_driver_delete(UART_NUM);
        s_ctx.initialized = false;
        ESP_LOGI(TAG, "Servo bus shut down");
    }
    return ESP_OK;
}

static cJSON *plugin_get_state(sb_plugin_t *self)
{
    (void)self;
    cJSON *state = cJSON_CreateObject();
    if (state == NULL) return NULL;

    cJSON *servos = cJSON_AddArrayToObject(state, "servos");
    for (uint8_t i = 0; i < s_ctx.servo_count; i++) {
        sb_servo_state_t st;
        if (read_servo_state(s_ctx.servo_ids[i], &st) == ESP_OK) {
            s_ctx.servo_states[i] = st;
            cJSON *s = cJSON_CreateObject();
            cJSON_AddNumberToObject(s, "id", s_ctx.servo_ids[i]);
            cJSON_AddNumberToObject(s, "position", st.position);
            cJSON_AddNumberToObject(s, "speed", st.speed);
            cJSON_AddNumberToObject(s, "load", st.load);
            cJSON_AddNumberToObject(s, "temperature", st.temperature);
            cJSON_AddNumberToObject(s, "voltage", st.voltage);
            cJSON_AddBoolToObject(s, "torque_on", st.torque_on);
            cJSON_AddItemToArray(servos, s);
        }
    }
    return state;
}

static cJSON *plugin_handle_command(sb_plugin_t *self, const char *cmd, const cJSON *params)
{
    (void)self;
    cJSON *result = cJSON_CreateObject();
    if (result == NULL) return NULL;

    if (strcmp(cmd, "get_state") == 0) {
        int id_val = -1;
        const cJSON *id_json = cJSON_GetObjectItem(params, "id");
        if (id_json != NULL && cJSON_IsNumber(id_json)) {
            id_val = id_json->valueint;
        }

        if (id_val < 0) {
            cJSON_Delete(result);
            return plugin_get_state(self);
        }

        sb_servo_state_t st;
        if (read_servo_state((uint8_t)id_val, &st) == ESP_OK) {
            cJSON_AddNumberToObject(result, "id", id_val);
            cJSON_AddNumberToObject(result, "position", st.position);
            cJSON_AddNumberToObject(result, "speed", st.speed);
            cJSON_AddNumberToObject(result, "load", st.load);
            cJSON_AddNumberToObject(result, "temperature", st.temperature);
            cJSON_AddNumberToObject(result, "voltage", st.voltage);
            cJSON_AddBoolToObject(result, "torque_on", st.torque_on);
        } else {
            cJSON_AddStringToObject(result, "error", "Failed to read servo state");
        }

    } else if (strcmp(cmd, "set_position") == 0) {
        const cJSON *id_j = cJSON_GetObjectItem(params, "id");
        const cJSON *pos_j = cJSON_GetObjectItem(params, "position");
        if (id_j && pos_j && cJSON_IsNumber(id_j) && cJSON_IsNumber(pos_j)) {
            esp_err_t err = set_position((uint8_t)id_j->valueint, (uint16_t)pos_j->valueint);
            cJSON_AddBoolToObject(result, "ok", err == ESP_OK);
            if (err == ESP_OK) {
                char ev[128];
                snprintf(ev, sizeof(ev), "{\"id\":%d,\"position\":%d}",
                         id_j->valueint, pos_j->valueint);
                sb_event_publish("servo.position_changed", ev);
            }
        } else {
            cJSON_AddStringToObject(result, "error", "Missing id or position");
        }

    } else if (strcmp(cmd, "set_speed") == 0) {
        const cJSON *id_j = cJSON_GetObjectItem(params, "id");
        const cJSON *spd_j = cJSON_GetObjectItem(params, "speed");
        if (id_j && spd_j && cJSON_IsNumber(id_j) && cJSON_IsNumber(spd_j)) {
            esp_err_t err = set_speed((uint8_t)id_j->valueint, (uint16_t)spd_j->valueint);
            cJSON_AddBoolToObject(result, "ok", err == ESP_OK);
        } else {
            cJSON_AddStringToObject(result, "error", "Missing id or speed");
        }

    } else if (strcmp(cmd, "set_torque") == 0) {
        const cJSON *id_j = cJSON_GetObjectItem(params, "id");
        const cJSON *en_j = cJSON_GetObjectItem(params, "enabled");
        if (id_j && en_j && cJSON_IsNumber(id_j) && cJSON_IsBool(en_j)) {
            esp_err_t err = set_torque((uint8_t)id_j->valueint, cJSON_IsTrue(en_j));
            cJSON_AddBoolToObject(result, "ok", err == ESP_OK);
        } else {
            cJSON_AddStringToObject(result, "error", "Missing id or enabled");
        }

    } else if (strcmp(cmd, "sync_move") == 0) {
        const cJSON *moves = cJSON_GetObjectItem(params, "moves");
        if (moves != NULL && cJSON_IsArray(moves)) {
            int count = cJSON_GetArraySize(moves);
            if (count > 0 && count <= MAX_BUS_SERVOS) {
                uint8_t ids[MAX_BUS_SERVOS];
                uint16_t positions[MAX_BUS_SERVOS];
                bool valid = true;
                for (int i = 0; i < count; i++) {
                    cJSON *m = cJSON_GetArrayItem(moves, i);
                    cJSON *id_item = m ? cJSON_GetObjectItem(m, "id") : NULL;
                    cJSON *pos_item = m ? cJSON_GetObjectItem(m, "position") : NULL;
                    if (!id_item || !pos_item) { valid = false; break; }
                    ids[i] = (uint8_t)id_item->valueint;
                    positions[i] = (uint16_t)pos_item->valueint;
                }
                if (!valid) {
                    cJSON_AddStringToObject(result, "error", "Invalid move entry");
                    return result;
                }
                esp_err_t err = sync_write_positions(ids, positions, (uint8_t)count);
                cJSON_AddBoolToObject(result, "ok", err == ESP_OK);
                cJSON_AddNumberToObject(result, "count", count);
                sb_event_publish("servo.sync_move", "{\"count\":1}");
            } else {
                cJSON_AddStringToObject(result, "error", "Invalid moves array");
            }
        } else {
            cJSON_AddStringToObject(result, "error", "Missing moves array");
        }

    } else if (strcmp(cmd, "scan") == 0) {
        cJSON_Delete(result);
        cJSON *scan_result = cJSON_CreateObject();
        cJSON *found = scan_bus();
        cJSON_AddItemToObject(scan_result, "found_ids", found ? found : cJSON_CreateArray());
        cJSON_AddNumberToObject(scan_result, "count", found ? cJSON_GetArraySize(found) : 0);
        return scan_result;

    } else if (strcmp(cmd, "set_id") == 0) {
        const cJSON *cur_j = cJSON_GetObjectItem(params, "current_id");
        const cJSON *new_j = cJSON_GetObjectItem(params, "new_id");
        if (cur_j && new_j && cJSON_IsNumber(cur_j) && cJSON_IsNumber(new_j)) {
            uint8_t cur_id = (uint8_t)cur_j->valueint;
            uint8_t new_id = (uint8_t)new_j->valueint;
            if (new_id > 253) {
                cJSON_AddStringToObject(result, "error", "new_id must be 0-253");
            } else if (!ping_servo(cur_id)) {
                cJSON_AddStringToObject(result, "error", "Servo not found at current_id");
            } else if (cur_id != new_id && ping_servo(new_id)) {
                cJSON_AddStringToObject(result, "error", "new_id already in use");
            } else {
                /* Unlock EEPROM, write new ID, lock EEPROM.
                 * Each write needs settling time and UART flush. */
                uint8_t resp[16];
                uint8_t unlock = 0;
                write_registers(cur_id, STS_REG_LOCK, &unlock, 1);
                vTaskDelay(pdMS_TO_TICKS(50));
                bus_recv(resp, sizeof(resp)); /* consume write response */
                uart_flush_input(UART_NUM);

                uint8_t id_val = new_id;
                esp_err_t err = write_registers(cur_id, STS_REG_ID, &id_val, 1);
                vTaskDelay(pdMS_TO_TICKS(100)); /* EEPROM write settling */
                bus_recv(resp, sizeof(resp)); /* consume write response */
                uart_flush_input(UART_NUM);

                uint8_t lock = 1;
                write_registers(new_id, STS_REG_LOCK, &lock, 1);
                vTaskDelay(pdMS_TO_TICKS(50));
                bus_recv(resp, sizeof(resp)); /* consume write response */
                uart_flush_input(UART_NUM);

                /* Verify the change */
                bool verified = ping_servo(new_id);
                cJSON_AddBoolToObject(result, "ok", err == ESP_OK && verified);
                cJSON_AddBoolToObject(result, "verified", verified);
                cJSON_AddNumberToObject(result, "old_id", cur_id);
                cJSON_AddNumberToObject(result, "new_id", new_id);
                if (verified) {
                    ESP_LOGI(TAG, "Servo ID changed and verified: %d -> %d", cur_id, new_id);
                } else {
                    ESP_LOGW(TAG, "Servo ID change %d -> %d: write ok but verify failed", cur_id, new_id);
                }
            }
        } else {
            cJSON_AddStringToObject(result, "error", "Missing current_id or new_id");
        }

    } else if (strcmp(cmd, "read_reg") == 0) {
        const cJSON *id_j = cJSON_GetObjectItem(params, "id");
        const cJSON *addr_j = cJSON_GetObjectItem(params, "addr");
        const cJSON *cnt_j = cJSON_GetObjectItem(params, "count");
        if (!id_j || !addr_j || !cJSON_IsNumber(id_j) || !cJSON_IsNumber(addr_j)) {
            cJSON_AddStringToObject(result, "error", "Missing id or addr");
            return result;
        }
        uint8_t id = (uint8_t)id_j->valueint;
        uint8_t addr = (uint8_t)addr_j->valueint;
        uint8_t cnt = (cnt_j && cJSON_IsNumber(cnt_j)) ? (uint8_t)cnt_j->valueint : 1;
        if (cnt > 128) cnt = 128;

        uint8_t data[128];
        int n = read_registers(id, addr, cnt, data, sizeof(data));
        if (n < cnt) {
            cJSON_AddStringToObject(result, "error", "Read failed");
            return result;
        }
        cJSON *vals = cJSON_AddArrayToObject(result, "data");
        for (int i = 0; i < cnt; i++) {
            cJSON_AddItemToArray(vals, cJSON_CreateNumber(data[i]));
        }
        cJSON_AddBoolToObject(result, "ok", true);

    } else if (strcmp(cmd, "write_reg") == 0) {
        const cJSON *id_j = cJSON_GetObjectItem(params, "id");
        const cJSON *addr_j = cJSON_GetObjectItem(params, "addr");
        const cJSON *data_j = cJSON_GetObjectItem(params, "data");
        const cJSON *unlock_j = cJSON_GetObjectItem(params, "unlock_eeprom");
        if (!id_j || !addr_j || !data_j || !cJSON_IsNumber(id_j) ||
            !cJSON_IsNumber(addr_j) || !cJSON_IsArray(data_j)) {
            cJSON_AddStringToObject(result, "error", "Missing id, addr, or data[]");
            return result;
        }
        uint8_t id = (uint8_t)id_j->valueint;
        uint8_t addr = (uint8_t)addr_j->valueint;
        bool unlock_eeprom = (unlock_j && cJSON_IsTrue(unlock_j));
        int cnt = cJSON_GetArraySize(data_j);
        if (cnt <= 0 || cnt > 50) {
            cJSON_AddStringToObject(result, "error", "data[] must be 1-50 bytes");
            return result;
        }
        uint8_t data[50];
        for (int i = 0; i < cnt; i++) {
            data[i] = (uint8_t)cJSON_GetArrayItem(data_j, i)->valueint;
        }

        uint8_t resp[16];
        if (unlock_eeprom) {
            uint8_t unlock = 0;
            write_registers(id, STS_REG_LOCK, &unlock, 1);
            vTaskDelay(pdMS_TO_TICKS(20));
            bus_recv(resp, sizeof(resp));
            uart_flush_input(UART_NUM);
        }

        esp_err_t err = write_registers(id, addr, data, (uint8_t)cnt);
        vTaskDelay(pdMS_TO_TICKS(unlock_eeprom ? 50 : 10));
        bus_recv(resp, sizeof(resp));
        uart_flush_input(UART_NUM);

        if (unlock_eeprom) {
            uint8_t lock = 1;
            write_registers(id, STS_REG_LOCK, &lock, 1);
            vTaskDelay(pdMS_TO_TICKS(20));
            bus_recv(resp, sizeof(resp));
            uart_flush_input(UART_NUM);
        }
        cJSON_AddBoolToObject(result, "ok", err == ESP_OK);

    } else if (strcmp(cmd, "backup") == 0) {
        const cJSON *id_j = cJSON_GetObjectItem(params, "id");
        if (!id_j || !cJSON_IsNumber(id_j)) {
            cJSON_AddStringToObject(result, "error", "Missing id");
            return result;
        }
        uint8_t id = (uint8_t)id_j->valueint;
        if (!ping_servo(id)) {
            cJSON_AddStringToObject(result, "error", "Servo not found");
            return result;
        }
        /* Read EEPROM registers 0-49 (covers all writable EEPROM + lock) */
        uint8_t eeprom[50];
        memset(eeprom, 0, sizeof(eeprom));
        /* Read in two chunks to stay within response buffer limits */
        int n1 = read_registers(id, 0, 25, eeprom, 25);
        int n2 = read_registers(id, 25, 25, eeprom + 25, 25);
        if (n1 < 25 || n2 < 25) {
            cJSON_AddStringToObject(result, "error", "Failed to read EEPROM");
            return result;
        }
        cJSON_AddNumberToObject(result, "id", id);
        cJSON *regs = cJSON_AddArrayToObject(result, "eeprom");
        for (int i = 0; i < 50; i++) {
            cJSON_AddItemToArray(regs, cJSON_CreateNumber(eeprom[i]));
        }
        cJSON_AddBoolToObject(result, "ok", true);
        ESP_LOGI(TAG, "Backup servo %d: 50 EEPROM bytes read", id);

    } else if (strcmp(cmd, "restore") == 0) {
        const cJSON *id_j = cJSON_GetObjectItem(params, "id");
        const cJSON *eeprom_j = cJSON_GetObjectItem(params, "eeprom");
        const cJSON *include_id_j = cJSON_GetObjectItem(params, "include_id");
        bool include_id = (include_id_j && cJSON_IsTrue(include_id_j));

        if (!id_j || !cJSON_IsNumber(id_j) || !eeprom_j || !cJSON_IsArray(eeprom_j)) {
            cJSON_AddStringToObject(result, "error", "Missing id or eeprom array");
            return result;
        }
        uint8_t id = (uint8_t)id_j->valueint;
        int arr_size = cJSON_GetArraySize(eeprom_j);
        if (arr_size < 48) {
            cJSON_AddStringToObject(result, "error", "eeprom array too short (need >= 48)");
            return result;
        }
        if (!ping_servo(id)) {
            cJSON_AddStringToObject(result, "error", "Servo not found");
            return result;
        }

        uint8_t eeprom[50];
        for (int i = 0; i < arr_size && i < 50; i++) {
            eeprom[i] = (uint8_t)cJSON_GetArrayItem(eeprom_j, i)->valueint;
        }

        /* If include_id is set, check for a new_id field that overrides eeprom[5].
         * This lets users edit the JSON "id" or "new_id" to change the target ID. */
        const cJSON *new_id_j = cJSON_GetObjectItem(params, "new_id");
        if (include_id && new_id_j && cJSON_IsNumber(new_id_j)) {
            eeprom[5] = (uint8_t)new_id_j->valueint;
            ESP_LOGI(TAG, "Restore: overriding EEPROM ID byte to %d", eeprom[5]);
        }

        /* Unlock EEPROM */
        uint8_t unlock = 0;
        write_registers(id, STS_REG_LOCK, &unlock, 1);
        vTaskDelay(pdMS_TO_TICKS(10));

        /* Write writable EEPROM regions, skipping read-only regs 0-4 (firmware/HW version)
         * and reg 5 (ID) unless include_id is true.
         * Write regs 5-47 in small chunks for EEPROM reliability. */
        int start = include_id ? 5 : 6;
        /* Write in chunks of 10 bytes max for EEPROM safety */
        for (int pos = start; pos < 48; pos += 10) {
            int chunk = 48 - pos;
            if (chunk > 10) chunk = 10;
            write_registers(id, (uint8_t)pos, &eeprom[pos], (uint8_t)chunk);
            vTaskDelay(pdMS_TO_TICKS(30)); /* EEPROM write settling */
        }

        /* Lock EEPROM */
        uint8_t lock = 1;
        write_registers(id, STS_REG_LOCK, &lock, 1);

        cJSON_AddBoolToObject(result, "ok", true);
        cJSON_AddNumberToObject(result, "id", id);
        cJSON_AddNumberToObject(result, "bytes_written", 48 - start);
        ESP_LOGI(TAG, "Restore servo %d: EEPROM written (regs %d-47)", id, start);

    } else if (strcmp(cmd, "factory_reset") == 0) {
        /* Use the Feetech native RESET instruction (0x06) to restore the
         * servo's own factory defaults.  This is handled entirely by the
         * servo's firmware, so the correct defaults are applied for the
         * specific variant (7.4V vs 12V, etc.).
         * Note: the native RESET also resets the servo ID to 1. We save
         * the current ID beforehand and re-write it after the reset. */
        const cJSON *id_j = cJSON_GetObjectItem(params, "id");
        if (!id_j || !cJSON_IsNumber(id_j)) {
            cJSON_AddStringToObject(result, "error", "Missing id");
            return result;
        }
        uint8_t id = (uint8_t)id_j->valueint;
        if (!ping_servo(id)) {
            cJSON_AddStringToObject(result, "error", "Servo not found");
            return result;
        }

        /* Send native RESET instruction (0x06) — no parameters */
        esp_err_t err = send_instruction(id, STS_INST_RESET, NULL, 0);
        if (err != ESP_OK) {
            cJSON_AddStringToObject(result, "error", "Failed to send RESET instruction");
            return result;
        }
        /* Allow EEPROM write time — servo resets all registers */
        vTaskDelay(pdMS_TO_TICKS(500));
        uint8_t resp[16];
        bus_recv(resp, sizeof(resp));
        uart_flush_input(UART_NUM);

        /* The native RESET resets ID to 1.  If this servo was not ID 1,
         * re-write the original ID so it stays addressable on the bus. */
        if (id != 1) {
            /* Unlock EEPROM on the now-ID-1 servo */
            uint8_t unlock = 0;
            write_registers(1, STS_REG_LOCK, &unlock, 1);
            vTaskDelay(pdMS_TO_TICKS(20));
            bus_recv(resp, sizeof(resp));
            uart_flush_input(UART_NUM);

            /* Write original ID */
            write_registers(1, STS_REG_ID, &id, 1);
            vTaskDelay(pdMS_TO_TICKS(50));
            bus_recv(resp, sizeof(resp));
            uart_flush_input(UART_NUM);

            /* Lock EEPROM */
            uint8_t lock = 1;
            write_registers(id, STS_REG_LOCK, &lock, 1);
            vTaskDelay(pdMS_TO_TICKS(20));
            bus_recv(resp, sizeof(resp));
            uart_flush_input(UART_NUM);
        }

        cJSON_AddBoolToObject(result, "ok", true);
        cJSON_AddNumberToObject(result, "id", id);
        cJSON_AddStringToObject(result, "detail",
            "Native RESET applied — servo restored to its own factory defaults. ID preserved.");
        ESP_LOGI(TAG, "Factory reset servo %d via native RESET instruction", id);

    } else if (strcmp(cmd, "emergency_stop") == 0) {
        esp_err_t err = emergency_stop_all();
        cJSON_AddBoolToObject(result, "stopped", true);
        cJSON_AddBoolToObject(result, "ok", err == ESP_OK);
        sb_event_publish("servo.emergency_stop", "{\"stopped\":true}");

    } else {
        cJSON_AddStringToObject(result, "error", "Unknown command");
        cJSON_AddStringToObject(result, "command", cmd);
    }

    return result;
}

static sb_health_status_t plugin_health_check(sb_plugin_t *self)
{
    (void)self;
    sb_health_status_t hs;

    if (!s_ctx.initialized) {
        hs.state = SB_HEALTH_UNHEALTHY;
        snprintf(hs.message, sizeof(hs.message), "Not initialized");
        return hs;
    }

    /* Try pinging the first servo as a health indicator */
    if (s_ctx.servo_count > 0 && !ping_servo(s_ctx.servo_ids[0])) {
        hs.state = SB_HEALTH_DEGRADED;
        snprintf(hs.message, sizeof(hs.message), "Servo %d not responding",
                 s_ctx.servo_ids[0]);
        return hs;
    }

    hs.state = SB_HEALTH_HEALTHY;
    snprintf(hs.message, sizeof(hs.message), "%d servos on bus", s_ctx.servo_count);
    return hs;
}

sb_plugin_t *sb_servo_bus_plugin(void)
{
    s_plugin.name           = "servo-bus";
    s_plugin.version        = "1.0.0";
    s_plugin.initialize     = plugin_init;
    s_plugin.shutdown       = plugin_shutdown;
    s_plugin.get_state      = plugin_get_state;
    s_plugin.handle_command = plugin_handle_command;
    s_plugin.health_check   = plugin_health_check;
    s_plugin.ctx            = &s_ctx;
    return &s_plugin;
}
