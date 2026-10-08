/*
 * MiBee Cam v0.1 — MJPEG streamer over independent TCP server
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Serves MJPEG frames via a separate TCP server on port 81.
 * Each client gets its own FreeRTOS task.
 * Maximum concurrent clients: 2.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize MJPEG streamer internals (mutex, client counter).
 * @return ESP_OK or ESP_ERR_NO_MEM
 */
esp_err_t mjpeg_stream_init(void);

/**
 * @brief Start the MJPEG TCP streaming server on the specified port.
 * @param port TCP port to listen on (typically 81)
 * @return ESP_OK or ESP_FAIL
 *
 * Creates a listening socket and spawns a task that accepts connections.
 * Each connection spawns a client task that streams MJPEG via multipart/x-mixed-replace.
 */
esp_err_t mjpeg_stream_server_start(uint16_t port);

/**
 * @brief Return current number of active MJPEG stream clients.
 */
int mjpeg_stream_client_count(void);

#ifdef __cplusplus
}
#endif
