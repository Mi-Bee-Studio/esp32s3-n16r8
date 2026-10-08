/*
 * MiBee Cam v0.1 — Web server (shared helpers)
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * CORS headers, JSON envelope helpers, request-body
 * reader, S3 chip temperature sensor, CORS preflight handler.
 *
 * Split from the monolithic web_server.c (issue #19, pure code motion):
 * route table + lifecycle stay in web_server.c; handler bodies live in
 * per-domain modules. Public API: web_server.h; internal seam:
 * web_server_internal.h. Behavior contract: docs/api-contract.md.
 */

#include "web_server.h"
#include "web_server_internal.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "driver/temperature_sensor.h"   /* chip_temp：S3 温度传感器（对齐 seeed/luatos，2026-09-04） */
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "web_server";

/* ── chip_temp：S3 温度传感器（2026-09-04 API 对齐，方案同 seeed/luatos）──
 * S3 温度传感器只有固定测量档（[-10,80]/[20,100]/[50,125]/[-30,50]），请求
 * 区间必须完整落入某一档，跨档被驱动拒绝——按高温档优先依次回退。
 * 惰性安装（首次 /api/status 请求时），此后驻留复用。 */
static temperature_sensor_handle_t s_tsens = NULL;
static float s_chip_temp = 0.0f;

static void chip_temp_ensure(void)
{
    if (s_tsens) {
        return;
    }
    static const temperature_sensor_config_t cand[] = {
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(50, 125),
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100),
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80),
    };
    for (size_t i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
        if (temperature_sensor_install(&cand[i], &s_tsens) == ESP_OK &&
            temperature_sensor_enable(s_tsens) == ESP_OK) {
            ESP_LOGI(TAG, "Temp sensor ready (range %d~%d°C)",
                     (int)cand[i].range_min, (int)cand[i].range_max);
            return;
        }
        s_tsens = NULL;
    }
    ESP_LOGW(TAG, "Temp sensor init failed — chip_temp stays 0");
}

float chip_temp_read(void)
{
    chip_temp_ensure();
    if (s_tsens) {
        temperature_sensor_get_celsius(s_tsens, &s_chip_temp);
    }
    return s_chip_temp;
}

/* ------------------------------------------------------------------ */
/*  JSON helpers                                                        */
/* ------------------------------------------------------------------ */

esp_err_t json_ok(httpd_req_t *req, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "JSON alloc failed", 17);
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    if (data) {
        cJSON_AddItemToObject(root, "data", data);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "JSON print failed", 17);
        return ESP_FAIL;
    }
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

esp_err_t json_error(httpd_req_t *req, const char *msg, int status)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "JSON alloc failed", 17);
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", msg);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "JSON print failed", 17);
        return ESP_FAIL;
    }
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_err(req, status, json);
    free(json);
    return ESP_FAIL;
}

char *read_body(httpd_req_t *req, size_t max_len)
{
    size_t len = req->content_len;
    if (len == 0 || len > max_len) {
        return NULL;
    }
    char *buf = malloc(len + 1);
    if (!buf) {
        return NULL;
    }
    int ret = httpd_req_recv(req, buf, len);
    if (ret <= 0) {
        free(buf);
        return NULL;
    }
    buf[ret] = '\0';
    return buf;
}

esp_err_t json_error_status(httpd_req_t *req, const char *msg, const char *status_line)
{
    char json[256];
    int len = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", msg);
    if (len >= (int)sizeof(json)) len = (int)sizeof(json) - 1;
    httpd_resp_set_status(req, status_line);
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);
    return httpd_resp_send(req, json, len);
}

/* ------------------------------------------------------------------ */
/*  CORS helpers                                                       */
/* ------------------------------------------------------------------ */

void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
}

/* ------------------------------------------------------------------ */
/*  OPTIONS — CORS preflight                                           */
/* ------------------------------------------------------------------ */

esp_err_t options_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}
