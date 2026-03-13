/**
 * @file bldc_driver.h
 * @brief Brushless DC motor driver plugin — public types and entry point.
 *
 * Implements the sb_plugin_t interface for external BLDC motor drivers
 * (BLD-510B as first target). Two control modes:
 *   - GPIO+PWM: direct digital signals (F/R, EN, BK) + LEDC PWM speed
 *   - RS485 Modbus RTU: register-based control via half-duplex UART
 *
 * Pin assignments come from NVS config — no hardcoded GPIOs.
 *
 * Task: BLDC Phase A
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of BLDC motors per node. */
#define SB_MAX_BLDC_MOTORS 4

/** Maximum length for motor name. */
#define SB_BLDC_NAME_MAX 32

/* ---- Enumerations ---- */

/** Control mode selection. */
typedef enum {
    SB_BLDC_MODE_GPIO   = 0,  /**< GPIO + LEDC PWM control */
    SB_BLDC_MODE_MODBUS = 1,  /**< RS485 Modbus RTU control */
} sb_bldc_mode_t;

/** Motor rotation direction. */
typedef enum {
    SB_BLDC_DIR_CW  = 0,  /**< Clockwise */
    SB_BLDC_DIR_CCW = 1,  /**< Counter-clockwise */
} sb_bldc_dir_t;

/** Safety FSM states. */
typedef enum {
    SB_BLDC_STATE_STOPPED   = 0,  /**< Motor stopped, ready */
    SB_BLDC_STATE_RUNNING   = 1,  /**< Motor running */
    SB_BLDC_STATE_BRAKING   = 2,  /**< Brake engaged, decelerating */
    SB_BLDC_STATE_REVERSING = 3,  /**< Direction change in progress */
    SB_BLDC_STATE_ESTOP     = 4,  /**< Emergency stop — requires clear */
    SB_BLDC_STATE_FAULT     = 5,  /**< Driver alarm — requires clear */
} sb_bldc_state_t;

/** Fault/alarm bitmask (from BLD-510B ALM output / Modbus $801BH). */
typedef enum {
    SB_BLDC_FAULT_NONE         = 0x00,
    SB_BLDC_FAULT_LOCKED_ROTOR = 0x01,  /**< Rotor locked / stall */
    SB_BLDC_FAULT_OVERCURRENT  = 0x02,  /**< Overcurrent detected */
    SB_BLDC_FAULT_HALL_ERROR   = 0x04,  /**< Hall sensor error */
    SB_BLDC_FAULT_OVERVOLTAGE  = 0x08,  /**< Supply voltage too high */
    SB_BLDC_FAULT_UNDERVOLTAGE = 0x10,  /**< Supply voltage too low */
    SB_BLDC_FAULT_COMM_ERROR   = 0x20,  /**< Modbus communication lost */
} sb_bldc_fault_t;

/* ---- Configuration ---- */

/** Per-motor configuration (stored in NVS). */
typedef struct {
    uint8_t  id;                         /**< Motor index (0-based) */
    char     name[SB_BLDC_NAME_MAX];     /**< Human-readable name */
    uint8_t  modbus_addr;                /**< RS485 slave address (Modbus mode) */
    uint8_t  pole_pairs;                 /**< Motor pole pairs (for RPM calc) */
    uint16_t max_speed;                  /**< Maximum speed value (0-255 or RPM) */
    uint16_t accel_time_100ms;           /**< Acceleration ramp (units of 0.1s) */
    uint16_t decel_time_100ms;           /**< Deceleration ramp (units of 0.1s) */

    /* GPIO mode pin assignments (-1 = not assigned) */
    int8_t   pin_en;                     /**< Enable output */
    int8_t   pin_fr;                     /**< Forward/Reverse output */
    int8_t   pin_bk;                     /**< Brake output */
    int8_t   pin_sv;                     /**< Speed PWM output (LEDC) */
    int8_t   pin_pg;                     /**< Pulse Generator feedback input */
    int8_t   pin_alm;                    /**< Alarm input (active low from driver) */
    bool     en_active_high;             /**< EN polarity: true=V2.4, false=V2.0 */
} sb_bldc_motor_config_t;

/** BLDC module configuration (stored in NVS). */
typedef struct {
    sb_bldc_mode_t mode;                          /**< Control mode */
    uint8_t        motor_count;                   /**< Number of motors (1-4) */

    /* RS485 pins (Modbus mode only, -1 = not assigned) */
    int8_t         rs485_tx;                      /**< UART TX for RS485 */
    int8_t         rs485_rx;                      /**< UART RX for RS485 */
    int8_t         rs485_de;                      /**< RS485 DE/RE direction pin */
    uint32_t       rs485_baud;                    /**< Modbus baud rate */

    /* Timing */
    uint32_t       pwm_freq_hz;                   /**< LEDC PWM frequency (1000-2000 Hz) */
    uint16_t       direction_change_stop_ms;      /**< Min stop time before reversal (ms) */
    uint16_t       alarm_poll_ms;                 /**< ALM / fault poll interval (ms) */

    sb_bldc_motor_config_t motors[SB_MAX_BLDC_MOTORS];
} sb_bldc_config_t;

/* ---- Runtime state ---- */

/** Runtime state for a single motor (read from hardware or tracked). */
typedef struct {
    sb_bldc_state_t state;       /**< Current FSM state */
    sb_bldc_dir_t   direction;   /**< Current direction */
    uint8_t         speed;       /**< Current speed setting (0-255) */
    uint16_t        rpm;         /**< Measured RPM (from PG or Modbus) */
    bool            enabled;     /**< EN signal active */
    bool            braking;     /**< BK signal active */
    uint8_t         faults;      /**< Bitmask of sb_bldc_fault_t */
} sb_bldc_motor_state_t;

/* ---- Plugin entry point ---- */

/**
 * Get the bldc-driver plugin instance.
 *
 * Call this to register the plugin with the plugin manager.
 * The returned pointer is static — do not free.
 */
struct sb_plugin *sb_bldc_driver_plugin(void);

#ifdef __cplusplus
}
#endif
