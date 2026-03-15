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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
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
#define STS_REG_GOAL_TIME       44  /* 2 bytes, ms — time to reach goal position */
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
    SemaphoreHandle_t bus_mutex;  /* Protects all UART bus access */
    volatile bool abort_scan;     /* E-stop sets this to abort in-progress scan */
    volatile bool scan_in_progress; /* True while async scan task is running */
} servo_bus_ctx_t;

static servo_bus_ctx_t s_ctx;
static sb_plugin_t s_plugin;

/* ── Bus Mutex ── */

/**
 * Acquire exclusive access to the UART bus.
 * Uses FreeRTOS mutex with priority inheritance to prevent priority inversion.
 * Returns true if lock acquired, false on timeout.
 */
static bool bus_lock(TickType_t timeout)
{
    if (s_ctx.bus_mutex == NULL) return true; /* pre-init fallback */
    return xSemaphoreTake(s_ctx.bus_mutex, timeout) == pdTRUE;
}

/**
 * Release exclusive access to the UART bus.
 */
static void bus_unlock(void)
{
    if (s_ctx.bus_mutex != NULL) {
        xSemaphoreGive(s_ctx.bus_mutex);
    }
}

/* Default timeout for bus operations (covers scan: 254 pings × ~30ms worst case) */
#define BUS_LOCK_TIMEOUT_MS   10000
/* Short timeout for health check — don't block diagnostics on long ops */
#define BUS_HEALTH_TIMEOUT_MS 100
/* E-stop timeout — scan releases lock within one ping cycle (~30ms) */
#define BUS_ESTOP_TIMEOUT_MS  500
/* Async scan task configuration */
#define SCAN_TASK_STACK_SIZE  4096
#define SCAN_TASK_PRIORITY    (tskIDLE_PRIORITY + 1)

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
 * Set goal position for a servo (position only).
 */
static esp_err_t set_position(uint8_t id, uint16_t position)
{
    return write_u16(id, STS_REG_GOAL_POSITION, position);
}

/**
 * Set goal position with optional time and speed — atomic 6-byte write.
 *
 * ST3215 registers 42-47 are contiguous:
 *   42-43: Goal Position (0-4095)
 *   44-45: Goal Time     (ms, 0 = use speed instead)
 *   46-47: Goal Speed    (0-4095 steps/s, 0 = max speed)
 *
 * Writing all three atomically in one bus packet ensures the servo
 * applies position+time+speed together.  If only position is given
 * (time=0, speed=0), the servo moves at max speed.
 */
static esp_err_t set_position_ext(uint8_t id, uint16_t position,
                                   uint16_t time_ms, uint16_t speed)
{
    uint8_t data[6];
    data[0] = (uint8_t)(position & 0xFF);
    data[1] = (uint8_t)((position >> 8) & 0xFF);
    data[2] = (uint8_t)(time_ms & 0xFF);
    data[3] = (uint8_t)((time_ms >> 8) & 0xFF);
    data[4] = (uint8_t)(speed & 0xFF);
    data[5] = (uint8_t)((speed >> 8) & 0xFF);
    return write_registers(id, STS_REG_GOAL_POSITION, data, 6);
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
 * Sync write position only — 2 bytes per servo starting at reg 42.
 * Used when no speed/time parameters are provided (backward compatible).
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

    /* Safety: check packet won't overflow buffer */
    if (7 + count * (data_len + 1) > sizeof(pkt) - 1) return ESP_ERR_INVALID_ARG;

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
 * Sync write position + time + speed — 6 bytes per servo starting at reg 42.
 * All three params are applied atomically per servo in one bus transaction.
 *
 * Registers per servo: Position(2) + Time(2) + Speed(2) = 6 bytes.
 */
static esp_err_t sync_write_positions_ext(const uint8_t *ids,
                                           const uint16_t *positions,
                                           const uint16_t *times,
                                           const uint16_t *speeds,
                                           uint8_t count)
{
    if (count == 0 || count > MAX_BUS_SERVOS) return ESP_ERR_INVALID_ARG;

    uint8_t pkt[256];
    uint8_t data_len = 6; /* position(2) + time(2) + speed(2) per servo */
    uint8_t param_count = (count * (data_len + 1)) + 2;
    uint8_t total_len = param_count + 2;

    /* Safety: check packet won't overflow buffer.
     * Max: 7 header + 32 servos × 7 bytes = 7 + 224 = 231 — fits in 256. */
    if (7 + count * (data_len + 1) > sizeof(pkt) - 1) return ESP_ERR_INVALID_ARG;

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
        pkt[idx++] = (uint8_t)(times[i] & 0xFF);
        pkt[idx++] = (uint8_t)((times[i] >> 8) & 0xFF);
        pkt[idx++] = (uint8_t)(speeds[i] & 0xFF);
        pkt[idx++] = (uint8_t)((speeds[i] >> 8) & 0xFF);
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
 *
 * Acquires/releases bus mutex per-ping so E-stop can interleave.
 * Checks abort_scan between each ping for cooperative cancellation.
 * Called from scan_bus_task() on a dedicated FreeRTOS task — NOT from httpd.
 */
static cJSON *scan_bus(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) return NULL;

    for (uint8_t id = 0; id < 254; id++) {
        /* Check abort flag before each ping — allows E-stop to cancel scan */
        if (s_ctx.abort_scan) {
            ESP_LOGW(TAG, "Scan aborted at ID %d", id);
            break;
        }

        /* Acquire/release mutex per-ping so E-stop can interleave.
         * Holding the lock for the entire 254-ping scan would block
         * E-stop for ~8 seconds — violating the 100ms deadline. */
        if (!bus_lock(pdMS_TO_TICKS(1000))) {
            ESP_LOGW(TAG, "Scan: lock timeout at ID %d, skipping", id);
            continue;
        }
        bool found = ping_servo(id);
        bus_unlock();

        if (found) {
            cJSON_AddItemToArray(arr, cJSON_CreateNumber(id));
            ESP_LOGI(TAG, "Found servo at ID %d", id);
        }

        /* Yield between pings — lets httpd process E-stop and other requests */
        vTaskDelay(1);
    }
    return arr;
}

/**
 * FreeRTOS task: runs bus scan asynchronously.
 *
 * Decoupled from the httpd thread so E-stop requests can be processed
 * during the scan.  Publishes results via event bus (WebSocket delivery).
 */
static void scan_bus_task(void *arg)
{
    (void)arg;

    cJSON *found = scan_bus();
    int count = found ? cJSON_GetArraySize(found) : 0;
    bool was_aborted = s_ctx.abort_scan;

    /* Publish result via event bus — WebUI receives via WebSocket */
    cJSON *ev_obj = cJSON_CreateObject();
    if (ev_obj) {
        /* Transfer found array into event object (ownership moves) */
        cJSON_AddItemToObject(ev_obj, "found_ids",
                              found ? found : cJSON_CreateArray());
        found = NULL; /* ownership transferred */
        cJSON_AddNumberToObject(ev_obj, "count", count);
        cJSON_AddBoolToObject(ev_obj, "aborted", was_aborted);

        char *json_str = cJSON_PrintUnformatted(ev_obj);
        if (json_str) {
            sb_event_publish("servo.scan_complete", json_str);
            cJSON_free(json_str);
        }
        cJSON_Delete(ev_obj);
    } else if (found) {
        cJSON_Delete(found);
    }

    ESP_LOGI(TAG, "Scan complete: %d servos found%s", count,
             was_aborted ? " (aborted)" : "");

    s_ctx.abort_scan = false;
    s_ctx.scan_in_progress = false;

    vTaskDelete(NULL);
}

/* ── Plugin Interface ── */

static esp_err_t plugin_init(sb_plugin_t *self, const cJSON *config)
{
    (void)self;
    memset(&s_ctx, 0, sizeof(s_ctx));

    /* Create bus mutex before any UART operations */
    s_ctx.bus_mutex = xSemaphoreCreateMutex();
    if (s_ctx.bus_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create bus mutex");
        return ESP_ERR_NO_MEM;
    }

    const sb_config_t *app_cfg = sb_config_get();
    if (app_cfg == NULL) {
        ESP_LOGE(TAG, "No app config — cannot initialize servo bus");
        vSemaphoreDelete(s_ctx.bus_mutex);
        s_ctx.bus_mutex = NULL;
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
        /* Abort any in-progress scan and wait for it to finish */
        if (s_ctx.scan_in_progress) {
            s_ctx.abort_scan = true;
            ESP_LOGI(TAG, "Waiting for scan to abort before shutdown...");
            for (int i = 0; i < 50 && s_ctx.scan_in_progress; i++) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (s_ctx.scan_in_progress) {
                ESP_LOGW(TAG, "Scan did not finish in time — proceeding with shutdown");
            }
        }

        /* Disable torque on all servos before shutting down.
         * Lock the bus for the entire shutdown sequence. */
        if (bus_lock(pdMS_TO_TICKS(BUS_LOCK_TIMEOUT_MS))) {
            emergency_stop_all();
            uart_driver_delete(UART_NUM);
            bus_unlock();
        } else {
            ESP_LOGE(TAG, "Could not acquire bus lock for shutdown — forcing UART delete");
            uart_driver_delete(UART_NUM);
        }
        if (s_ctx.bus_mutex != NULL) {
            vSemaphoreDelete(s_ctx.bus_mutex);
            s_ctx.bus_mutex = NULL;
        }
        s_ctx.initialized = false;
        ESP_LOGI(TAG, "Servo bus shut down");
    }
    return ESP_OK;
}

/**
 * Build state JSON for all servos — caller must already hold bus_mutex.
 */
static cJSON *get_state_unlocked(void)
{
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

static cJSON *plugin_get_state(sb_plugin_t *self)
{
    (void)self;
    if (!bus_lock(pdMS_TO_TICKS(BUS_LOCK_TIMEOUT_MS))) {
        ESP_LOGW(TAG, "Could not acquire bus lock for get_state");
        cJSON *empty = cJSON_CreateObject();
        if (empty) cJSON_AddArrayToObject(empty, "servos");
        return empty;
    }
    cJSON *state = get_state_unlocked();
    bus_unlock();
    return state;
}

static cJSON *plugin_handle_command(sb_plugin_t *self, const char *cmd, const cJSON *params)
{
    (void)self;
    cJSON *result = cJSON_CreateObject();
    if (result == NULL) return NULL;

    /* ── Emergency stop fast-path ──────────────────────────────────────
     * E-stop gets its own dedicated path BEFORE the general bus lock.
     * If a scan is running on the scan task, we signal it to abort first,
     * then acquire the lock with a short timeout.  The scan releases its
     * per-ping lock within ~30ms, so E-stop typically waits < 50ms.
     *
     * Safety fallback: if the lock STILL times out, we force the stop
     * without the lock.  Minor UART corruption is acceptable vs. not
     * stopping the servos.  Hardware safety > mutex correctness. */
    if (strcmp(cmd, "emergency_stop") == 0) {
        if (s_ctx.scan_in_progress) {
            s_ctx.abort_scan = true;
            ESP_LOGW(TAG, "E-STOP: aborting in-progress scan");
        }

        if (!bus_lock(pdMS_TO_TICKS(BUS_ESTOP_TIMEOUT_MS))) {
            ESP_LOGE(TAG, "CRITICAL: Bus lock timeout for E-STOP — forcing stop without lock");
            emergency_stop_all();
            cJSON_AddBoolToObject(result, "stopped", true);
            cJSON_AddBoolToObject(result, "ok", false);
            cJSON_AddStringToObject(result, "warning",
                                    "Lock timeout — forced stop without lock");
            sb_event_publish("servo.emergency_stop",
                             "{\"stopped\":true,\"forced\":true}");
            return result;
        }

        esp_err_t err = emergency_stop_all();
        bus_unlock();

        cJSON_AddBoolToObject(result, "stopped", true);
        cJSON_AddBoolToObject(result, "ok", err == ESP_OK);
        sb_event_publish("servo.emergency_stop", "{\"stopped\":true}");
        return result;
    }

    /* Acquire bus mutex for all other commands.
     * All commands touch the UART — direction pin, TX, RX must be atomic. */
    if (!bus_lock(pdMS_TO_TICKS(BUS_LOCK_TIMEOUT_MS))) {
        ESP_LOGE(TAG, "Bus lock timeout for command: %s", cmd);
        cJSON_AddStringToObject(result, "error", "Bus busy — try again");
        return result;
    }

    if (strcmp(cmd, "get_state") == 0) {
        int id_val = -1;
        const cJSON *id_json = cJSON_GetObjectItem(params, "id");
        if (id_json != NULL && cJSON_IsNumber(id_json)) {
            id_val = id_json->valueint;
        }

        if (id_val < 0) {
            /* Lock is already held — use unlocked variant to avoid deadlock */
            cJSON_Delete(result);
            result = get_state_unlocked();
            bus_unlock();
            return result;
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
            uint8_t  servo_id = (uint8_t)id_j->valueint;

            /* Clamp position to valid ST3215 range (0-4095, 12-bit) */
            int raw_pos = pos_j->valueint;
            if (raw_pos < 0) raw_pos = 0;
            if (raw_pos > 4095) raw_pos = 4095;
            uint16_t position = (uint16_t)raw_pos;

            /* Optional speed and time — if either is provided, use the
             * atomic 6-byte write (regs 42-47: position+time+speed).
             * If neither is provided, fall back to simple position-only
             * write for backward compatibility and minimal bus traffic. */
            const cJSON *spd_j  = cJSON_GetObjectItem(params, "speed");
            const cJSON *time_j = cJSON_GetObjectItem(params, "time");

            /* Clamp speed to 0-4095 (ST3215 register range, 0 = max speed)
             * Clamp time to 0-30000ms (practical upper bound for safety) */
            int raw_speed = (spd_j && cJSON_IsNumber(spd_j)) ? spd_j->valueint : 0;
            int raw_time  = (time_j && cJSON_IsNumber(time_j)) ? time_j->valueint : 0;
            if (raw_speed < 0) raw_speed = 0;
            if (raw_speed > 4095) raw_speed = 4095;
            if (raw_time < 0) raw_time = 0;
            if (raw_time > 30000) raw_time = 30000;
            uint16_t move_speed = (uint16_t)raw_speed;
            uint16_t move_time  = (uint16_t)raw_time;

            esp_err_t err;
            if (move_speed > 0 || move_time > 0) {
                err = set_position_ext(servo_id, position, move_time, move_speed);
            } else {
                err = set_position(servo_id, position);
            }

            cJSON_AddBoolToObject(result, "ok", err == ESP_OK);
            if (err == ESP_OK) {
                char ev[192];
                snprintf(ev, sizeof(ev),
                         "{\"id\":%d,\"position\":%d,\"time\":%d,\"speed\":%d}",
                         servo_id, position, move_time, move_speed);
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
                uint8_t  ids[MAX_BUS_SERVOS];
                uint16_t positions[MAX_BUS_SERVOS];
                uint16_t times[MAX_BUS_SERVOS];
                uint16_t speeds[MAX_BUS_SERVOS];
                bool valid = true;
                bool has_ext = false; /* Any move has speed or time? */

                for (int i = 0; i < count; i++) {
                    cJSON *m = cJSON_GetArrayItem(moves, i);
                    cJSON *id_item  = m ? cJSON_GetObjectItem(m, "id") : NULL;
                    cJSON *pos_item = m ? cJSON_GetObjectItem(m, "position") : NULL;
                    if (!id_item || !pos_item) { valid = false; break; }
                    ids[i] = (uint8_t)id_item->valueint;

                    /* Clamp position to valid ST3215 range (0-4095) */
                    int raw_p = pos_item->valueint;
                    if (raw_p < 0) raw_p = 0;
                    if (raw_p > 4095) raw_p = 4095;
                    positions[i] = (uint16_t)raw_p;

                    cJSON *time_item  = m ? cJSON_GetObjectItem(m, "time") : NULL;
                    cJSON *speed_item = m ? cJSON_GetObjectItem(m, "speed") : NULL;

                    /* Clamp speed (0-4095) and time (0-30000ms) */
                    int raw_s = (speed_item && cJSON_IsNumber(speed_item))
                                ? speed_item->valueint : 0;
                    int raw_t = (time_item && cJSON_IsNumber(time_item))
                                ? time_item->valueint : 0;
                    if (raw_s < 0) raw_s = 0;
                    if (raw_s > 4095) raw_s = 4095;
                    if (raw_t < 0) raw_t = 0;
                    if (raw_t > 30000) raw_t = 30000;
                    times[i]  = (uint16_t)raw_t;
                    speeds[i] = (uint16_t)raw_s;
                    if (times[i] > 0 || speeds[i] > 0) has_ext = true;
                }
                if (!valid) {
                    cJSON_AddStringToObject(result, "error", "Invalid move entry");
                    bus_unlock();
                    return result;
                }

                /* Use the extended 6-byte sync write if any move has
                 * speed or time, otherwise use the compact 2-byte version. */
                esp_err_t err;
                if (has_ext) {
                    err = sync_write_positions_ext(ids, positions, times,
                                                   speeds, (uint8_t)count);
                } else {
                    err = sync_write_positions(ids, positions, (uint8_t)count);
                }

                cJSON_AddBoolToObject(result, "ok", err == ESP_OK);
                cJSON_AddNumberToObject(result, "count", count);
                cJSON_AddBoolToObject(result, "extended", has_ext);
                char sync_ev[96];
                snprintf(sync_ev, sizeof(sync_ev),
                         "{\"count\":%d,\"extended\":%s}",
                         count, has_ext ? "true" : "false");
                sb_event_publish("servo.sync_move", sync_ev);
            } else {
                cJSON_AddStringToObject(result, "error", "Invalid moves array");
            }
        } else {
            cJSON_AddStringToObject(result, "error", "Missing moves array");
        }

    } else if (strcmp(cmd, "scan") == 0) {
        if (s_ctx.scan_in_progress) {
            cJSON_AddStringToObject(result, "error", "Scan already in progress");
            bus_unlock();
            return result;
        }

        /* Set flags WHILE holding the lock so the check-then-set is atomic.
         * Prevents a TOCTOU race if two scan requests arrive near-simultaneously. */
        s_ctx.abort_scan = false;
        s_ctx.scan_in_progress = true;

        /* Release command-level lock — scan task manages its own per-ping locking */
        bus_unlock();

        /* Start async scan on a separate FreeRTOS task.
         * The httpd thread stays free to process E-stop and other requests.
         * Results are published via event bus ('servo.scan_complete'). */

        BaseType_t created = xTaskCreate(
            scan_bus_task, "scan_bus",
            SCAN_TASK_STACK_SIZE, NULL,
            SCAN_TASK_PRIORITY, NULL
        );

        if (created != pdPASS) {
            s_ctx.scan_in_progress = false;
            ESP_LOGE(TAG, "Failed to create scan task");
            cJSON_AddStringToObject(result, "error", "Failed to start scan task");
            return result;
        }

        cJSON_AddStringToObject(result, "status", "scanning");
        cJSON_AddStringToObject(result, "message",
            "Scan started — results delivered via 'servo.scan_complete' event");
        return result;

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
            bus_unlock();
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
            bus_unlock();
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
            bus_unlock();
            return result;
        }
        uint8_t id = (uint8_t)id_j->valueint;
        uint8_t addr = (uint8_t)addr_j->valueint;
        bool unlock_eeprom = (unlock_j && cJSON_IsTrue(unlock_j));
        int cnt = cJSON_GetArraySize(data_j);
        if (cnt <= 0 || cnt > 50) {
            cJSON_AddStringToObject(result, "error", "data[] must be 1-50 bytes");
            bus_unlock();
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
            bus_unlock();
            return result;
        }
        uint8_t id = (uint8_t)id_j->valueint;
        if (!ping_servo(id)) {
            cJSON_AddStringToObject(result, "error", "Servo not found");
            bus_unlock();
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
            bus_unlock();
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
            bus_unlock();
            return result;
        }
        uint8_t id = (uint8_t)id_j->valueint;
        int arr_size = cJSON_GetArraySize(eeprom_j);
        if (arr_size < 48) {
            cJSON_AddStringToObject(result, "error", "eeprom array too short (need >= 48)");
            bus_unlock();
            return result;
        }
        if (!ping_servo(id)) {
            cJSON_AddStringToObject(result, "error", "Servo not found");
            bus_unlock();
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
            bus_unlock();
            return result;
        }
        uint8_t id = (uint8_t)id_j->valueint;
        if (!ping_servo(id)) {
            cJSON_AddStringToObject(result, "error", "Servo not found");
            bus_unlock();
            return result;
        }

        /* Send native RESET instruction (0x06) — no parameters */
        esp_err_t err = send_instruction(id, STS_INST_RESET, NULL, 0);
        if (err != ESP_OK) {
            cJSON_AddStringToObject(result, "error", "Failed to send RESET instruction");
            bus_unlock();
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

    } else {
        /* Note: emergency_stop is handled in the fast-path above (before bus_lock) */
        cJSON_AddStringToObject(result, "error", "Unknown command");
        cJSON_AddStringToObject(result, "command", cmd);
    }

    bus_unlock();
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

    /* Short timeout — don't block diagnostics waiting on a long scan/restore */
    if (!bus_lock(pdMS_TO_TICKS(BUS_HEALTH_TIMEOUT_MS))) {
        hs.state = SB_HEALTH_DEGRADED;
        snprintf(hs.message, sizeof(hs.message), "Bus busy (lock timeout)");
        return hs;
    }

    /* Try pinging the first servo as a health indicator */
    if (s_ctx.servo_count > 0 && !ping_servo(s_ctx.servo_ids[0])) {
        hs.state = SB_HEALTH_DEGRADED;
        snprintf(hs.message, sizeof(hs.message), "Servo %d not responding",
                 s_ctx.servo_ids[0]);
        bus_unlock();
        return hs;
    }

    hs.state = SB_HEALTH_HEALTHY;
    snprintf(hs.message, sizeof(hs.message), "%d servos on bus", s_ctx.servo_count);
    bus_unlock();
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
