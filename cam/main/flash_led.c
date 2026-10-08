/*
 * MiBee Cam v0.1 — PWM flash LED driver
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Hardware:
 *   - LED: High-current flash / torch LED (typically with driver transistor)
 *   - GPIO: configurable (default 2)
 *   - Timer: LEDC_TIMER_2  (TIMER_0 is reserved for camera XCLK)
 *   - Channel: LEDC_CHANNEL_2
 *   - Resolution: 13 bit, 5 kHz
 */

#include "flash_led.h"

#include "esp_err.h"
#include "esp_log.h"
#include "driver/ledc.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

static const char *TAG = "flash_led";

/** PWM frequency (5 kHz — smooth, no audible whine). */
#define FLASH_LED_FREQ_HZ       (5000)

/** Duty resolution — 13 bit gives 0–8191 range. */
#define FLASH_LED_DUTY_RES      LEDC_TIMER_13_BIT

/** Maximum duty value for the chosen resolution (2^13 - 1). */
#define FLASH_LED_DUTY_MAX      ((1 << 13) - 1)

/** LEDC timer number — NOT timer 0 (camera XCLK uses it). */
#define FLASH_LED_TIMER         LEDC_TIMER_2

/** LEDC channel number. */
#define FLASH_LED_CHANNEL       LEDC_CHANNEL_2

/** LEDC speed mode — low speed is the typical choice. */
#define FLASH_LED_SPEED_MODE    LEDC_LOW_SPEED_MODE

/* ------------------------------------------------------------------ */
/*  Module state                                                       */
/* ------------------------------------------------------------------ */

static bool s_initialized = false;

/* Pre-populated channel config — gpio_num is set at init time. */
static ledc_channel_config_t s_channel = {
    .gpio_num   = 0,
    .speed_mode = FLASH_LED_SPEED_MODE,
    .channel    = FLASH_LED_CHANNEL,
    .intr_type  = LEDC_INTR_DISABLE,
    .timer_sel  = FLASH_LED_TIMER,
    .duty       = 0,
    .hpoint     = 0,
};

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t flash_led_init(gpio_num_t gpio)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Flash LED already initialised");
        return ESP_OK;
    }

    /* ---- Timer config ------------------------------------------------ */
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = FLASH_LED_SPEED_MODE,
        .duty_resolution = FLASH_LED_DUTY_RES,
        .timer_num       = FLASH_LED_TIMER,
        .freq_hz         = FLASH_LED_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };

    esp_err_t ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ---- Channel config ---------------------------------------------- */
    s_channel.gpio_num = gpio;
    ret = ledc_channel_config(&s_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Flash LED initialized on GPIO %d (PWM %d Hz, %d-bit)",
             gpio, FLASH_LED_FREQ_HZ, 13);
    return ESP_OK;
}

esp_err_t flash_led_set_brightness(uint8_t percent)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (percent > 100) {
        percent = 100;
    }

    uint32_t duty = (uint32_t)percent * FLASH_LED_DUTY_MAX / 100;

    esp_err_t ret = ledc_set_duty(s_channel.speed_mode,
                                  s_channel.channel, duty);
    if (ret != ESP_OK) {
        return ret;
    }
    return ledc_update_duty(s_channel.speed_mode, s_channel.channel);
}

esp_err_t flash_led_on(void)
{
    return flash_led_set_brightness(100);
}

esp_err_t flash_led_off(void)
{
    return flash_led_set_brightness(0);
}

/** @brief 读取当前闪光灯亮度百分比（未初始化返回 0） */
uint8_t flash_led_get_brightness(void)
{
    if (!s_initialized) {
        return 0;
    }
    return (uint8_t)((ledc_get_duty(s_channel.speed_mode, s_channel.channel) * 100)
                     / FLASH_LED_DUTY_MAX);
}
