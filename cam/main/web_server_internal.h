/*
 * MiBee Cam v0.1 — web server internal seam (NOT part of public API)
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shared declarations between web_server.c (route table + lifecycle) and
 * the per-domain handler modules split out of it (issue #19). Public API
 * remains web_server.h. Log TAG stays "web_server" in every module so
 * serial signatures (PITFALLS grep patterns) do not shift.
 */

#pragma once

#include "esp_http_server.h"

/* ---- shared helpers (web_server_util.c) ---- */
void  set_cors_headers(httpd_req_t *req);
float chip_temp_read(void);

/* ---- URI handlers, referenced by s_uris[] in web_server.c ----
 * (definition module in brackets; names match the route table) */
esp_err_t static_file_handler(httpd_req_t *req);
esp_err_t api_status_handler(httpd_req_t *req);
esp_err_t api_config_get_handler(httpd_req_t *req);
esp_err_t api_config_post_handler(httpd_req_t *req);
esp_err_t api_capabilities_handler(httpd_req_t *req);
esp_err_t api_capture_handler(httpd_req_t *req);
esp_err_t api_scan_handler(httpd_req_t *req);
esp_err_t api_reset_handler(httpd_req_t *req);
esp_err_t api_reboot_handler(httpd_req_t *req);
esp_err_t api_csi_calibrate_handler(httpd_req_t *req);
esp_err_t api_time_handler(httpd_req_t *req);
esp_err_t metrics_handler(httpd_req_t *req);
esp_err_t api_led_handler(httpd_req_t *req);
esp_err_t api_led_get_handler(httpd_req_t *req);
esp_err_t api_ai_handler(httpd_req_t *req);
esp_err_t ai_status_get_handler(httpd_req_t *req);
esp_err_t api_camera_get_handler(httpd_req_t *req);
esp_err_t api_camera_post_handler(httpd_req_t *req);
esp_err_t options_handler(httpd_req_t *req);
