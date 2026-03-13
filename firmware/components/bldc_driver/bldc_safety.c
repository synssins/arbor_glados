/**
 * @file bldc_safety.c
 * @brief Direction interlock FSM and emergency stop for BLDC motors.
 *
 * State machine per motor:
 *
 *   STOPPED ──(set speed > 0)──→ RUNNING
 *   RUNNING ──(set speed 0)───→ STOPPED
 *   RUNNING ──(brake cmd)─────→ BRAKING ──(stopped)──→ STOPPED
 *   RUNNING ──(dir change)────→ BRAKING ──(stopped)──→ REVERSING
 *           ──(wait ms)───────→ (set dir) → STOPPED
 *   ANY ──────(estop)─────────→ ESTOP
 *   ANY ──────(fault)─────────→ FAULT
 *   ESTOP ────(clear)─────────→ STOPPED (if fault gone)
 *   FAULT ────(clear)─────────→ STOPPED (if fault gone)
 *
 * The REVERSING state enforces a mandatory wait (direction_change_stop_ms)
 * after the motor has stopped before the F/R pin is changed. This protects
 * the BLD-510B driver from back-EMF damage.
 *
 * Task: BLDC Phase A
 */

#include "bldc_safety.h"
#include "bldc_gpio.h"
#include "bldc_modbus.h"
#include "bldc_driver.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bldc-safety";

/* ---- Per-motor FSM context ---- */

typedef struct {
    sb_bldc_state_t       state;
    sb_bldc_dir_t         direction;        /**< Current confirmed direction */
    sb_bldc_dir_t         pending_dir;      /**< Requested direction (during reversal) */
    uint8_t               speed;            /**< Current speed setting */
    uint8_t               faults;           /**< Active fault bitmask */
    bool                  enabled;          /**< EN state */
    bool                  braking;          /**< BK state */
    int64_t               reversal_start_us;/**< When REVERSING state entered */
} motor_fsm_t;

/** Module-level state. */
static struct {
    bool         initialized;
    uint8_t      motor_count;
    sb_bldc_mode_t mode;
    uint16_t     dir_change_stop_ms;
    motor_fsm_t  motors[SB_MAX_BLDC_MOTORS];
} s_safety;

/* ---- Internal helpers ---- */

/** Apply speed to hardware through the correct sub-driver. */
static esp_err_t hw_set_speed(uint8_t idx, uint8_t speed)
{
    if (s_safety.mode == SB_BLDC_MODE_GPIO) {
        return bldc_gpio_set_speed(idx, speed);
    } else {
        return bldc_modbus_set_speed(idx, speed);
    }
}

/** Apply direction to hardware. */
static esp_err_t hw_set_direction(uint8_t idx, sb_bldc_dir_t dir)
{
    if (s_safety.mode == SB_BLDC_MODE_GPIO) {
        return bldc_gpio_set_direction(idx, dir);
    } else {
        return bldc_modbus_set_direction(idx, dir);
    }
}

/** Apply enable to hardware. */
static esp_err_t hw_set_enable(uint8_t idx, bool enable)
{
    if (s_safety.mode == SB_BLDC_MODE_GPIO) {
        return bldc_gpio_set_enable(idx, enable);
    } else {
        return bldc_modbus_set_enable(idx, enable);
    }
}

/** Apply brake to hardware. */
static esp_err_t hw_set_brake(uint8_t idx, bool brake)
{
    if (s_safety.mode == SB_BLDC_MODE_GPIO) {
        return bldc_gpio_set_brake(idx, brake);
    } else {
        return bldc_modbus_set_brake(idx, brake);
    }
}

/** Check if motor is actually stopped (RPM ~ 0). */
static bool motor_is_stopped(uint8_t idx)
{
    if (s_safety.mode == SB_BLDC_MODE_GPIO) {
        return bldc_gpio_get_rpm(idx) < 5;  /* Allow small noise */
    } else {
        uint16_t rpm = 0;
        bldc_modbus_read_rpm(idx, &rpm);
        return rpm < 5;
    }
}

/** Read alarm/fault status from hardware. */
static uint8_t hw_read_faults(uint8_t idx)
{
    if (s_safety.mode == SB_BLDC_MODE_GPIO) {
        return bldc_gpio_read_alarm(idx) ? SB_BLDC_FAULT_LOCKED_ROTOR : 0;
    } else {
        uint8_t faults = 0;
        bldc_modbus_read_faults(idx, &faults);
        return faults;
    }
}

/* ---- Public API ---- */

esp_err_t bldc_safety_init(const sb_bldc_config_t *config)
{
    if (s_safety.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_safety, 0, sizeof(s_safety));
    s_safety.motor_count = config->motor_count;
    s_safety.mode = config->mode;
    s_safety.dir_change_stop_ms = config->direction_change_stop_ms;

    /* Enforce minimum interlock delay */
    if (s_safety.dir_change_stop_ms < 100) {
        s_safety.dir_change_stop_ms = 100;
        ESP_LOGW(TAG, "direction_change_stop_ms clamped to minimum 100ms");
    }

    for (uint8_t i = 0; i < config->motor_count && i < SB_MAX_BLDC_MOTORS; i++) {
        s_safety.motors[i].state     = SB_BLDC_STATE_STOPPED;
        s_safety.motors[i].direction = SB_BLDC_DIR_CW;
        s_safety.motors[i].speed     = 0;
        s_safety.motors[i].faults    = 0;
        s_safety.motors[i].enabled   = false;
        s_safety.motors[i].braking   = true;  /* Start braked */
    }

    s_safety.initialized = true;
    ESP_LOGI(TAG, "Safety FSM initialized: %d motor(s), interlock=%dms",
             config->motor_count, s_safety.dir_change_stop_ms);
    return ESP_OK;
}

esp_err_t bldc_safety_request_speed(uint8_t motor_idx, uint8_t speed)
{
    if (!s_safety.initialized || motor_idx >= s_safety.motor_count) {
        return ESP_ERR_INVALID_STATE;
    }

    motor_fsm_t *m = &s_safety.motors[motor_idx];

    /* Reject if in ESTOP, FAULT, or REVERSING */
    if (m->state == SB_BLDC_STATE_ESTOP ||
        m->state == SB_BLDC_STATE_FAULT ||
        m->state == SB_BLDC_STATE_REVERSING) {
        ESP_LOGW(TAG, "Motor %d: speed rejected in state %d", motor_idx, m->state);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = hw_set_speed(motor_idx, speed);
    if (ret != ESP_OK) return ret;

    m->speed = speed;

    if (speed == 0) {
        m->state = SB_BLDC_STATE_STOPPED;
    } else if (m->state == SB_BLDC_STATE_STOPPED ||
               m->state == SB_BLDC_STATE_BRAKING) {
        /* Release brake when starting */
        hw_set_brake(motor_idx, false);
        m->braking = false;
        m->state = SB_BLDC_STATE_RUNNING;
    }

    ESP_LOGD(TAG, "Motor %d: speed set to %d, state=%d", motor_idx, speed, m->state);
    return ESP_OK;
}

esp_err_t bldc_safety_request_direction(uint8_t motor_idx, sb_bldc_dir_t dir)
{
    if (!s_safety.initialized || motor_idx >= s_safety.motor_count) {
        return ESP_ERR_INVALID_STATE;
    }

    motor_fsm_t *m = &s_safety.motors[motor_idx];

    /* Reject if already in ESTOP, FAULT, or mid-REVERSING */
    if (m->state == SB_BLDC_STATE_ESTOP ||
        m->state == SB_BLDC_STATE_FAULT ||
        m->state == SB_BLDC_STATE_REVERSING) {
        ESP_LOGW(TAG, "Motor %d: direction rejected in state %d", motor_idx, m->state);
        return ESP_ERR_INVALID_STATE;
    }

    /* If already in the requested direction, nothing to do */
    if (m->direction == dir) {
        ESP_LOGD(TAG, "Motor %d: already %s", motor_idx,
                 dir == SB_BLDC_DIR_CW ? "CW" : "CCW");
        return ESP_OK;
    }

    m->pending_dir = dir;

    if (m->state == SB_BLDC_STATE_STOPPED && m->speed == 0) {
        /* Motor is already stopped — can change direction immediately
         * but still observe the minimum wait time */
        m->state = SB_BLDC_STATE_REVERSING;
        m->reversal_start_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Motor %d: direction change %s→%s (motor already stopped, waiting %dms)",
                 motor_idx,
                 m->direction == SB_BLDC_DIR_CW ? "CW" : "CCW",
                 dir == SB_BLDC_DIR_CW ? "CW" : "CCW",
                 s_safety.dir_change_stop_ms);
    } else {
        /* Motor is running — need to stop first */
        ESP_LOGI(TAG, "Motor %d: direction change requested %s→%s — stopping first",
                 motor_idx,
                 m->direction == SB_BLDC_DIR_CW ? "CW" : "CCW",
                 dir == SB_BLDC_DIR_CW ? "CW" : "CCW");

        /* Set speed to 0 and engage brake */
        hw_set_speed(motor_idx, 0);
        hw_set_brake(motor_idx, true);
        m->speed = 0;
        m->braking = true;
        m->state = SB_BLDC_STATE_BRAKING;
        /* The tick function will advance to REVERSING once stopped */
    }

    return ESP_OK;
}

esp_err_t bldc_safety_request_enable(uint8_t motor_idx, bool enable)
{
    if (!s_safety.initialized || motor_idx >= s_safety.motor_count) {
        return ESP_ERR_INVALID_STATE;
    }

    motor_fsm_t *m = &s_safety.motors[motor_idx];

    if (m->state == SB_BLDC_STATE_ESTOP || m->state == SB_BLDC_STATE_FAULT) {
        if (enable) {
            ESP_LOGW(TAG, "Motor %d: enable rejected in state %d", motor_idx, m->state);
            return ESP_ERR_INVALID_STATE;
        }
    }

    esp_err_t ret = hw_set_enable(motor_idx, enable);
    if (ret != ESP_OK) return ret;

    m->enabled = enable;

    if (!enable && m->state == SB_BLDC_STATE_RUNNING) {
        /* Disabling a running motor → stopped */
        hw_set_speed(motor_idx, 0);
        m->speed = 0;
        m->state = SB_BLDC_STATE_STOPPED;
    }

    ESP_LOGD(TAG, "Motor %d: enable → %s", motor_idx, enable ? "ON" : "OFF");
    return ESP_OK;
}

esp_err_t bldc_safety_request_brake(uint8_t motor_idx)
{
    if (!s_safety.initialized || motor_idx >= s_safety.motor_count) {
        return ESP_ERR_INVALID_STATE;
    }

    motor_fsm_t *m = &s_safety.motors[motor_idx];

    /* Brake always accepted regardless of state (safety operation) */
    hw_set_speed(motor_idx, 0);
    hw_set_brake(motor_idx, true);

    m->speed = 0;
    m->braking = true;

    if (m->state == SB_BLDC_STATE_RUNNING) {
        m->state = SB_BLDC_STATE_BRAKING;
    }

    ESP_LOGI(TAG, "Motor %d: brake engaged", motor_idx);
    return ESP_OK;
}

esp_err_t bldc_safety_emergency_stop(void)
{
    if (!s_safety.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "EMERGENCY STOP — all motors");

    /* Hit hardware e-stop first (speed, timing critical) */
    esp_err_t ret;
    if (s_safety.mode == SB_BLDC_MODE_GPIO) {
        ret = bldc_gpio_emergency_stop();
    } else {
        ret = bldc_modbus_emergency_stop();
    }

    /* Update all FSM states */
    for (uint8_t i = 0; i < s_safety.motor_count; i++) {
        s_safety.motors[i].state   = SB_BLDC_STATE_ESTOP;
        s_safety.motors[i].speed   = 0;
        s_safety.motors[i].enabled = false;
        s_safety.motors[i].braking = true;
    }

    return ret;
}

esp_err_t bldc_safety_clear_fault(uint8_t motor_idx)
{
    if (!s_safety.initialized || motor_idx >= s_safety.motor_count) {
        return ESP_ERR_INVALID_STATE;
    }

    motor_fsm_t *m = &s_safety.motors[motor_idx];

    if (m->state != SB_BLDC_STATE_ESTOP && m->state != SB_BLDC_STATE_FAULT) {
        return ESP_OK;  /* Nothing to clear */
    }

    /* Check if hardware fault is still present */
    uint8_t hw_faults = hw_read_faults(motor_idx);
    if (hw_faults && m->state == SB_BLDC_STATE_FAULT) {
        ESP_LOGW(TAG, "Motor %d: cannot clear — fault still present: 0x%02x",
                 motor_idx, hw_faults);
        return ESP_ERR_INVALID_STATE;
    }

    m->state  = SB_BLDC_STATE_STOPPED;
    m->faults = 0;
    m->speed  = 0;

    ESP_LOGI(TAG, "Motor %d: fault/estop cleared → STOPPED", motor_idx);
    return ESP_OK;
}

void bldc_safety_report_fault(uint8_t motor_idx, uint8_t faults)
{
    if (!s_safety.initialized || motor_idx >= s_safety.motor_count) {
        return;
    }

    motor_fsm_t *m = &s_safety.motors[motor_idx];

    ESP_LOGW(TAG, "Motor %d: FAULT reported: 0x%02x", motor_idx, faults);

    /* Immediately disable motor */
    hw_set_speed(motor_idx, 0);
    hw_set_brake(motor_idx, true);
    hw_set_enable(motor_idx, false);

    m->state   = SB_BLDC_STATE_FAULT;
    m->faults  = faults;
    m->speed   = 0;
    m->enabled = false;
    m->braking = true;
}

sb_bldc_state_t bldc_safety_get_state(uint8_t motor_idx)
{
    if (!s_safety.initialized || motor_idx >= s_safety.motor_count) {
        return SB_BLDC_STATE_FAULT;
    }
    return s_safety.motors[motor_idx].state;
}

esp_err_t bldc_safety_get_motor_state(uint8_t motor_idx,
                                       sb_bldc_motor_state_t *state_out)
{
    if (!s_safety.initialized || motor_idx >= s_safety.motor_count || !state_out) {
        return ESP_ERR_INVALID_ARG;
    }

    motor_fsm_t *m = &s_safety.motors[motor_idx];

    state_out->state     = m->state;
    state_out->direction = m->direction;
    state_out->speed     = m->speed;
    state_out->enabled   = m->enabled;
    state_out->braking   = m->braking;
    state_out->faults    = m->faults;

    /* Get live RPM from hardware */
    if (s_safety.mode == SB_BLDC_MODE_GPIO) {
        state_out->rpm = bldc_gpio_get_rpm(motor_idx);
    } else {
        uint16_t rpm = 0;
        bldc_modbus_read_rpm(motor_idx, &rpm);
        state_out->rpm = rpm;
    }

    return ESP_OK;
}

void bldc_safety_tick(void)
{
    if (!s_safety.initialized) return;

    for (uint8_t i = 0; i < s_safety.motor_count; i++) {
        motor_fsm_t *m = &s_safety.motors[i];

        /* Skip motors in terminal states (ESTOP requires explicit clear) */
        if (m->state == SB_BLDC_STATE_ESTOP) {
            continue;
        }

        /* Check for hardware faults */
        if (m->state != SB_BLDC_STATE_FAULT) {
            uint8_t faults = hw_read_faults(i);
            if (faults) {
                bldc_safety_report_fault(i, faults);
                continue;
            }
        }

        /* Advance BRAKING → REVERSING once motor is stopped */
        if (m->state == SB_BLDC_STATE_BRAKING) {
            if (motor_is_stopped(i)) {
                /* Check if this braking was for a direction change */
                if (m->pending_dir != m->direction) {
                    m->state = SB_BLDC_STATE_REVERSING;
                    m->reversal_start_us = esp_timer_get_time();
                    ESP_LOGD(TAG, "Motor %d: stopped, entering REVERSING wait", i);
                } else {
                    m->state = SB_BLDC_STATE_STOPPED;
                    m->braking = false;
                    hw_set_brake(i, false);
                    ESP_LOGD(TAG, "Motor %d: braking complete → STOPPED", i);
                }
            }
        }

        /* Advance REVERSING → STOPPED after the mandatory wait */
        if (m->state == SB_BLDC_STATE_REVERSING) {
            int64_t elapsed_us = esp_timer_get_time() - m->reversal_start_us;
            int64_t required_us = (int64_t)s_safety.dir_change_stop_ms * 1000;

            if (elapsed_us >= required_us && motor_is_stopped(i)) {
                /* Safe to change direction now */
                hw_set_direction(i, m->pending_dir);
                m->direction = m->pending_dir;
                m->state = SB_BLDC_STATE_STOPPED;
                m->braking = false;
                hw_set_brake(i, false);

                ESP_LOGI(TAG, "Motor %d: reversal complete → %s, STOPPED",
                         i, m->direction == SB_BLDC_DIR_CW ? "CW" : "CCW");
            }
        }
    }
}
