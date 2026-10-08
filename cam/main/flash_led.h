/*
 * MiBee Cam v0.1 — PWM flash LED driver
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Drives a high-brightness flash / torch LED via LEDC PWM.
 * The GPIO is configurable at init time (default GPIO 2).
 *
 * Timer: LEDC_TIMER_2 (TIMER_0 is reserved for camera XCLK).
 * Channel: LEDC_CHANNEL_2.
 * Resolution: 13 bit, 5 kHz.
 */

#pragma once

#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the flash LED PWM driver
 *
 * Configures LEDC timer 2 and channel 2 for the given GPIO.
 *
 * @param gpio  GPIO number for the flash LED (e.g. GPIO_NUM_2)
 * @return ESP_OK on success, or an LEDC error code
 */
esp_err_t flash_led_init(gpio_num_t gpio);

/**
 * @brief Set flash LED brightness
 *
 * @param percent  Brightness level 0–100 (0 = off, 100 = full)
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialised
 */
esp_err_t flash_led_set_brightness(uint8_t percent);

/**
 * @brief Turn the flash LED on at full brightness (shorthand for set 100)
 *
 * @return ESP_OK on success
 */
esp_err_t flash_led_on(void);

/**
 * @brief Turn the flash LED off (shorthand for set 0)
 *
 * @return ESP_OK on success
 */
esp_err_t flash_led_off(void);

/**
 * @brief Read current flash LED brightness (0-100; 0 if not initialised)
 */
uint8_t flash_led_get_brightness(void);

#ifdef __cplusplus
}
#endif
