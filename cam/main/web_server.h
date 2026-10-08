/*
 * MiBee Cam v0.1 — Web server (ESP HTTP Server)
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Registers REST API endpoints and static file serving from SPIFFS.
 *
 * Endpoints:
 *   GET  /         — static files from SPIFFS (index.html, CSS, JS)
 *   GET  /stream   — MJPEG live stream (handled by mjpeg_streamer)
 *   GET  /status   — device status as JSON
 *   GET  /config   — configuration as JSON
 *   POST /config   — update configuration
 *   POST /led      — flash LED brightness control
 *   POST /ai       — AI feature toggle (face, motion, qr)
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the HTTP web server and register all URI handlers.
 * @param port  TCP port to listen on (typically 80)
 * @return ESP_OK on success, or an esp_err_t code
 */
esp_err_t web_server_start(uint16_t port);

/**
 * @brief Stop the HTTP web server and release resources.
 */
void web_server_stop(void);

/**
 * @brief Get the httpd server handle (useful for other modules).
 */
httpd_handle_t web_server_get_handle(void);

/* ---- Shared JSON helpers (usable by other modules) ---------------- */

/**
 * @brief Send a JSON success response: {"ok":true,"data":<data>}
 * @param req   HTTP request
 * @param data  JSON data item to include (may be NULL)
 */
esp_err_t json_ok(httpd_req_t *req, cJSON *data);

/**
 * @brief Send a JSON error response: {"ok":false,"error":<msg>}
 * @param req     HTTP request
 * @param msg     Error message string
 * @param status  HTTP status code (e.g. HTTPD_400_BAD_REQUEST)
 */
esp_err_t json_error(httpd_req_t *req, const char *msg, int status);

/**
 * @brief Read the full HTTP request body into a heap-allocated buffer.
 * Caller MUST free() the returned buffer.
 * @param req     HTTP request
 * @param max_len Maximum allowed content length
 * @return NULL on empty/oversized body, or a null-terminated string
 */
char *read_body(httpd_req_t *req, size_t max_len);

/**
 * @brief Send JSON error with a custom status line (e.g. "429 Too Many Requests").
 */
esp_err_t json_error_status(httpd_req_t *req, const char *msg, const char *status_line);

#ifdef __cplusplus
}
#endif
