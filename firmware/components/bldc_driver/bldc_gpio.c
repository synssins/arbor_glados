/**
 * @file bldc_gpio.c
 * @brief GPIO + LEDC PWM sub-driver for BLDC motor control.
 *
 * Controls external BLDC drivers (BLD-510B) via digital GPIO signals
 * and LEDC PWM for speed control.
 *
 * Pin signals:
 *   EN  → digital output (polarity configurable)
 *   F/R → digital output (CW/CCW)
 *   BK  → digital output (brake)
 *   SV  → LEDC PWM output (speed, 1-2 KHz)
 *   PG  → pulse input (RPM feedback, via PCNT or ISR)
 *   ALM → digital input (alarm, active low)
 *
 * Task: BLDC Phase A
 */

#include "bldc_gpio.h"
#include "bldc_driver.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "bldc-gpio";

/* ---- Per-motor GPIO runtime context ---- */

typedef struct {
    /* Config snapshot */
    int8_t   pin_en;
    int8_t   pin_fr;
    int8_t   pin_bk;
    int8_t   pin_sv;
    int8_t   pin_pg;
    int8_t   pin_alm;
    bool     en_active_high;

    /* LEDC channel assigned to this motor's SV pin */
    ledc_channel_t ledc_channel;

    /* RPM measurement */
    volatile uint32_t pg_pulse_count;   /**< Pulses since last RPM calc */
    uint16_t          last_rpm;         /**< Last calculated RPM */
    int64_t           last_rpm_time_us; /**< Timestamp of last RPM calc */
    uint8_t           pole_pairs;       /**< For RPM conversion */
} bldc_gpio_motor_t;

/** Module-level state. */
static struct {
    bool              initialized;
    uint8_t           motor_count;
    bldc_gpio_motor_t motors[SB_MAX_BLDC_MOTORS];
} s_gpio;

/* ---- PG pulse ISR ---- */

static void IRAM_ATTR pg_isr_handler(void *arg)
{
    uint8_t idx = (uint8_t)(uintptr_t)arg;
    if (idx < SB_MAX_BLDC_MOTORS) {
        s_gpio.motors[idx].pg_pulse_count++;
    }
}

/* ---- Helpers ---- */

static esp_err_t setup_digital_output(int8_t pin, const char *label,
                                       uint8_t motor_idx, uint32_t initial_level)
{
    if (pin < 0) {
        return ESP_OK;  /* Not assigned — skip */
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Motor %d: failed to configure %s (GPIO %d): %s",
                 motor_idx, label, pin, esp_err_to_name(ret));
        return ret;
    }
    gpio_set_level(pin, initial_level);
    ESP_LOGD(TAG, "Motor %d: %s → GPIO %d (initial=%lu)",
             motor_idx, label, pin, initial_level);
    return ESP_OK;
}

static esp_err_t setup_alarm_input(int8_t pin, uint8_t motor_idx)
{
    if (pin < 0) {
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* ALM is active-low, pull up */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,     /* Polled, not interrupt-driven */
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Motor %d: failed to configure ALM (GPIO %d): %s",
                 motor_idx, pin, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t setup_pg_input(int8_t pin, uint8_t motor_idx)
{
    if (pin < 0) {
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_POSEDGE,   /* Count rising edges */
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Motor %d: failed to configure PG (GPIO %d): %s",
                 motor_idx, pin, esp_err_to_name(ret));
        return ret;
    }

    /* Install ISR service if not already installed */
    static bool isr_service_installed = false;
    if (!isr_service_installed) {
        ret = gpio_install_isr_service(0);
        if (ret == ESP_ERR_INVALID_STATE) {
            /* Already installed (e.g. by another component) */
            ret = ESP_OK;
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s",
                     esp_err_to_name(ret));
            return ret;
        }
        isr_service_installed = true;
    }

    ret = gpio_isr_handler_add(pin, pg_isr_handler,
                                (void *)(uintptr_t)motor_idx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Motor %d: failed to add PG ISR: %s",
                 motor_idx, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t setup_pwm(int8_t pin, uint8_t motor_idx,
                             ledc_channel_t channel, uint32_t freq_hz)
{
    if (pin < 0) {
        return ESP_OK;
    }

    /* Timer config — use timer 0 for all BLDC PWM channels */
    static bool timer_configured = false;
    if (!timer_configured) {
        ledc_timer_config_t timer_conf = {
            .speed_mode      = LEDC_LOW_SPEED_MODE,
            .timer_num       = LEDC_TIMER_1,  /* Timer 1 — timer 0 may be used by servo_pwm */
            .duty_resolution = LEDC_TIMER_8_BIT,  /* 0-255 range matches BLD-510B */
            .freq_hz         = freq_hz,
            .clk_cfg         = LEDC_AUTO_CLK,
        };
        esp_err_t ret = ledc_timer_config(&timer_conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(ret));
            return ret;
        }
        timer_configured = true;
        ESP_LOGI(TAG, "LEDC timer configured: %lu Hz, 8-bit", freq_hz);
    }

    ledc_channel_config_t ch_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = channel,
        .timer_sel  = LEDC_TIMER_1,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = pin,
        .duty       = 0,  /* Start stopped */
        .hpoint     = 0,
    };
    esp_err_t ret = ledc_channel_config(&ch_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Motor %d: LEDC channel config failed (GPIO %d): %s",
                 motor_idx, pin, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "Motor %d: SV PWM → GPIO %d, channel %d", motor_idx, pin, channel);
    return ESP_OK;
}

/* ---- Public API ---- */

esp_err_t bldc_gpio_init(const sb_bldc_config_t *config)
{
    if (s_gpio.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!config || config->motor_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_gpio, 0, sizeof(s_gpio));
    s_gpio.motor_count = config->motor_count;

    uint32_t pwm_freq = config->pwm_freq_hz;
    if (pwm_freq == 0) {
        pwm_freq = 1000;  /* Default 1 KHz per BLD-510B spec */
    }

    for (uint8_t i = 0; i < config->motor_count && i < SB_MAX_BLDC_MOTORS; i++) {
        const sb_bldc_motor_config_t *mc = &config->motors[i];
        bldc_gpio_motor_t *gm = &s_gpio.motors[i];

        /* Store pin config */
        gm->pin_en  = mc->pin_en;
        gm->pin_fr  = mc->pin_fr;
        gm->pin_bk  = mc->pin_bk;
        gm->pin_sv  = mc->pin_sv;
        gm->pin_pg  = mc->pin_pg;
        gm->pin_alm = mc->pin_alm;
        gm->en_active_high = mc->en_active_high;
        gm->pole_pairs     = mc->pole_pairs > 0 ? mc->pole_pairs : 4;
        gm->ledc_channel   = (ledc_channel_t)i;  /* Channel 0, 1, 2, 3 */
        gm->last_rpm_time_us = esp_timer_get_time();

        /* EN: start disabled (safe).
         * active_high → low = disabled.  active_low → high = disabled. */
        uint32_t en_safe = mc->en_active_high ? 0 : 1;
        esp_err_t ret;

        ret = setup_digital_output(mc->pin_en, "EN", i, en_safe);
        if (ret != ESP_OK) return ret;

        /* F/R: start CW (arbitrary safe default) */
        ret = setup_digital_output(mc->pin_fr, "F/R", i, 0);
        if (ret != ESP_OK) return ret;

        /* BK: start with brake engaged (safe) — BK connected = stop */
        ret = setup_digital_output(mc->pin_bk, "BK", i, 1);
        if (ret != ESP_OK) return ret;

        /* SV: PWM at 0% duty (stopped) */
        ret = setup_pwm(mc->pin_sv, i, gm->ledc_channel, pwm_freq);
        if (ret != ESP_OK) return ret;

        /* ALM: input with pull-up */
        ret = setup_alarm_input(mc->pin_alm, i);
        if (ret != ESP_OK) return ret;

        /* PG: pulse input with ISR */
        ret = setup_pg_input(mc->pin_pg, i);
        if (ret != ESP_OK) return ret;

        ESP_LOGI(TAG, "Motor %d GPIO init OK (EN=%d F/R=%d BK=%d SV=%d PG=%d ALM=%d)",
                 i, mc->pin_en, mc->pin_fr, mc->pin_bk,
                 mc->pin_sv, mc->pin_pg, mc->pin_alm);
    }

    s_gpio.initialized = true;
    ESP_LOGI(TAG, "GPIO sub-driver initialized: %d motor(s)", config->motor_count);
    return ESP_OK;
}

void bldc_gpio_deinit(void)
{
    if (!s_gpio.initialized) return;

    /* Stop all motors first */
    bldc_gpio_emergency_stop();

    for (uint8_t i = 0; i < s_gpio.motor_count; i++) {
        bldc_gpio_motor_t *gm = &s_gpio.motors[i];

        /* Remove PG ISR */
        if (gm->pin_pg >= 0) {
            gpio_isr_handler_remove(gm->pin_pg);
        }

        /* Stop LEDC channel */
        if (gm->pin_sv >= 0) {
            ledc_stop(LEDC_LOW_SPEED_MODE, gm->ledc_channel, 0);
        }

        /* Reset all output pins to input mode (high-Z) */
        int8_t pins[] = { gm->pin_en, gm->pin_fr, gm->pin_bk };
        for (int p = 0; p < 3; p++) {
            if (pins[p] >= 0) {
                gpio_reset_pin(pins[p]);
            }
        }
    }

    s_gpio.initialized = false;
    ESP_LOGI(TAG, "GPIO sub-driver de-initialized");
}

esp_err_t bldc_gpio_set_speed(uint8_t motor_idx, uint8_t speed)
{
    if (!s_gpio.initialized || motor_idx >= s_gpio.motor_count) {
        return ESP_ERR_INVALID_STATE;
    }

    bldc_gpio_motor_t *gm = &s_gpio.motors[motor_idx];
    if (gm->pin_sv < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t ret = ledc_set_duty(LEDC_LOW_SPEED_MODE, gm->ledc_channel, speed);
    if (ret != ESP_OK) return ret;

    ret = ledc_update_duty(LEDC_LOW_SPEED_MODE, gm->ledc_channel);
    if (ret != ESP_OK) return ret;

    ESP_LOGD(TAG, "Motor %d: speed → %d", motor_idx, speed);
    return ESP_OK;
}

esp_err_t bldc_gpio_set_direction(uint8_t motor_idx, sb_bldc_dir_t dir)
{
    if (!s_gpio.initialized || motor_idx >= s_gpio.motor_count) {
        return ESP_ERR_INVALID_STATE;
    }

    bldc_gpio_motor_t *gm = &s_gpio.motors[motor_idx];
    if (gm->pin_fr < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* F/R pin: 0 = CW, 1 = CCW */
    gpio_set_level(gm->pin_fr, (dir == SB_BLDC_DIR_CCW) ? 1 : 0);
    ESP_LOGD(TAG, "Motor %d: direction → %s", motor_idx,
             dir == SB_BLDC_DIR_CW ? "CW" : "CCW");
    return ESP_OK;
}

esp_err_t bldc_gpio_set_enable(uint8_t motor_idx, bool enable)
{
    if (!s_gpio.initialized || motor_idx >= s_gpio.motor_count) {
        return ESP_ERR_INVALID_STATE;
    }

    bldc_gpio_motor_t *gm = &s_gpio.motors[motor_idx];
    if (gm->pin_en < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Respect EN polarity configuration */
    uint32_t level;
    if (gm->en_active_high) {
        level = enable ? 1 : 0;
    } else {
        level = enable ? 0 : 1;
    }

    gpio_set_level(gm->pin_en, level);
    ESP_LOGD(TAG, "Motor %d: enable → %s (GPIO %d = %lu)",
             motor_idx, enable ? "ON" : "OFF", gm->pin_en, level);
    return ESP_OK;
}

esp_err_t bldc_gpio_set_brake(uint8_t motor_idx, bool brake)
{
    if (!s_gpio.initialized || motor_idx >= s_gpio.motor_count) {
        return ESP_ERR_INVALID_STATE;
    }

    bldc_gpio_motor_t *gm = &s_gpio.motors[motor_idx];
    if (gm->pin_bk < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* BK: 1 = brake engaged (connected = stop) */
    gpio_set_level(gm->pin_bk, brake ? 1 : 0);
    ESP_LOGD(TAG, "Motor %d: brake → %s", motor_idx, brake ? "ON" : "OFF");
    return ESP_OK;
}

bool bldc_gpio_read_alarm(uint8_t motor_idx)
{
    if (!s_gpio.initialized || motor_idx >= s_gpio.motor_count) {
        return false;
    }

    bldc_gpio_motor_t *gm = &s_gpio.motors[motor_idx];
    if (gm->pin_alm < 0) {
        return false;  /* No alarm pin configured */
    }

    /* ALM is active-low: pin low = alarm active */
    return gpio_get_level(gm->pin_alm) == 0;
}

uint16_t bldc_gpio_get_rpm(uint8_t motor_idx)
{
    if (!s_gpio.initialized || motor_idx >= s_gpio.motor_count) {
        return 0;
    }

    bldc_gpio_motor_t *gm = &s_gpio.motors[motor_idx];
    if (gm->pin_pg < 0) {
        return 0;
    }

    /* Calculate RPM from pulse count since last call.
     * PG output: pole_pairs pulses per revolution.
     * RPM = (pulses / pole_pairs) / (elapsed_seconds) * 60 */
    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_us = now_us - gm->last_rpm_time_us;

    if (elapsed_us < 100000) {
        /* Less than 100ms since last calc — return cached value */
        return gm->last_rpm;
    }

    uint32_t pulses = gm->pg_pulse_count;
    gm->pg_pulse_count = 0;
    gm->last_rpm_time_us = now_us;

    if (elapsed_us == 0 || gm->pole_pairs == 0) {
        gm->last_rpm = 0;
        return 0;
    }

    double elapsed_sec = (double)elapsed_us / 1000000.0;
    double revolutions = (double)pulses / (double)gm->pole_pairs;
    uint16_t rpm = (uint16_t)(revolutions / elapsed_sec * 60.0);

    gm->last_rpm = rpm;
    return rpm;
}

esp_err_t bldc_gpio_emergency_stop(void)
{
    if (!s_gpio.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "EMERGENCY STOP — all motors");

    for (uint8_t i = 0; i < s_gpio.motor_count; i++) {
        bldc_gpio_motor_t *gm = &s_gpio.motors[i];

        /* 1. Speed to 0 immediately */
        if (gm->pin_sv >= 0) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, gm->ledc_channel, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, gm->ledc_channel);
        }

        /* 2. Brake ON */
        if (gm->pin_bk >= 0) {
            gpio_set_level(gm->pin_bk, 1);
        }

        /* 3. Enable OFF */
        if (gm->pin_en >= 0) {
            uint32_t en_off = gm->en_active_high ? 0 : 1;
            gpio_set_level(gm->pin_en, en_off);
        }
    }

    ESP_LOGW(TAG, "All motors stopped via GPIO e-stop");
    return ESP_OK;
}
