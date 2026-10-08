/*
 * MiBee Cam v0.1 — WS2812 status LED driver
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Hardware:
 *   - LED: WS2812 addressable RGB
 *   - GPIO: 48
 *   - Backend: RMT (via espressif/led_strip component)
 */

#include "status_led.h"

#include "esp_err.h"
#include "esp_log.h"
#include "led_strip.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

static const char *TAG = "status_led";

/** Status LED is hard-wired to GPIO 48 on this board. */
#define STATUS_LED_GPIO     GPIO_NUM_48

/** Number of addressable LEDs on the strip (1 for the status LED). */
#define STATUS_LED_COUNT    1

/** RMT tick rate — 10 MHz gives 100 ns resolution, adequate for WS2812. */
#define RMT_RESOLUTION_HZ   (10 * 1000 * 1000)

/* ------------------------------------------------------------------ */
/*  Module state                                                       */
/* ------------------------------------------------------------------ */

static led_strip_handle_t s_led_strip = NULL;

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t status_led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = STATUS_LED_GPIO,
        .max_leds       = STATUS_LED_COUNT,
    };

    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz  = RMT_RESOLUTION_HZ,
        .flags.with_dma = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Start with LED off */
    ret = led_strip_clear(s_led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_clear failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WS2812 status LED initialized on GPIO %d",
             STATUS_LED_GPIO);
    return ESP_OK;
}

esp_err_t status_led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_led_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = led_strip_set_pixel(s_led_strip, 0, r, g, b);
    if (ret != ESP_OK) {
        return ret;
    }
    return led_strip_refresh(s_led_strip);
}

esp_err_t status_led_off(void)
{
    if (s_led_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return led_strip_clear(s_led_strip);
}
