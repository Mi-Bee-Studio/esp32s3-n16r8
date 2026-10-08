/*
 * MiBee Cam v0.1 — WS2812 status LED driver
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Drives a single WS2812 addressable RGB LED on GPIO 48 via RMT.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WS2812 status LED on GPIO 48
 *
 * Configures an RMT channel and initializes a 1-LED strip.
 * Must be called before any other status_led_* function.
 *
 * @return ESP_OK on success, or an error code from led_strip
 */
esp_err_t status_led_init(void);

/**
 * @brief Set status LED to an RGB color
 *
 * @param r  Red channel (0–255)
 * @param g  Green channel (0–255)
 * @param b  Blue channel (0–255)
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t status_led_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Turn off the status LED (set colour to black)
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t status_led_off(void);

/* ------------------------------------------------------------------ */
/*  Named colour constants (use as positional args)                    */
/* ------------------------------------------------------------------ */
/** Red                          */  /* {255,   0,   0} */
#define STATUS_LED_RED     255, 0, 0
/** Green                        */  /* {  0, 255,   0} */
#define STATUS_LED_GREEN   0, 255, 0
/** Blue                         */  /* {  0,   0, 255} */
#define STATUS_LED_BLUE    0, 0, 255
/** Yellow (red + green)         */  /* {255, 255,   0} */
#define STATUS_LED_YELLOW  255, 255, 0

#ifdef __cplusplus
}
#endif
