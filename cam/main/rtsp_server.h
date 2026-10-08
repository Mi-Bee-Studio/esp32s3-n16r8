/*
 * MiBee Cam v0.1 — RTSP server (MJPEG-only, digest auth)
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Single MJPEG video track over RTP/UDP or TCP-interleaved (with patches).
 * Digest authentication via config_manager (rtsp_user / rtsp_pass).
 *
 * Architecture:
 *   rtsp_start()
 *     └─ create espp::RtspServer with MJPEG track
 *     └─ start server
 *     └─ spawn video_feed_task (Core 1)
 *          └─ frame_broadcaster_subscribe(FRAMESUB_RTSP)
 *          └─ loop: get_frame() → send_frame(0)
 *   rtsp_stop()
 *     └─ kill video task → stop server → destroy
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Create and start the RTSP server.
 *
 *         Initialises espp::RtspServer with MJPEG-only video track,
 *         configures digest auth from config_manager, binds port 554,
 *         and spawns a video feed task on Core 1.
 *
 * @return ESP_OK on success, ESP_FAIL on error.
 * @note   Call after frame_broadcaster_start().
 */
esp_err_t rtsp_start(void);

/**
 * @brief  Stop the RTSP server and all feed tasks.
 *
 *         Signals the video feed task to exit, stops the RtspServer,
 *         and releases all resources.
 *
 * @return ESP_OK on success.
 */
esp_err_t rtsp_stop(void);

/**
 * @brief  Get the RTSP stream URL.
 *
 *         Format: rtsp://<user>:<pass>@<ip>:554/stream
 *
 * @return Pointer to a static string buffer. Valid until the next call.
 */
const char *rtsp_get_url(void);

#ifdef __cplusplus
}
#endif
