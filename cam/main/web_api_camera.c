/*
 * MiBee Cam v0.1 — Web server (camera API)
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GET/POST /api/camera · GET /api/capture ·
 * GET+POST /api/led — camera state, sensor keys, single-frame JPEG, flash LED.
 *
 * Split from the monolithic web_server.c (issue #19, pure code motion):
 * route table + lifecycle stay in web_server.c; handler bodies live in
 * per-domain modules. Public API: web_server.h; internal seam:
 * web_server_internal.h. Behavior contract: docs/api-contract.md.
 */

#include "web_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "config_manager.h"
#include "camera_driver.h"
#include "esp_camera.h"
#include "flash_led.h"
#include "flash_viewers.h"   /* 板级扩展：拍照期间保持闪光灯判定在场 */
#include "ai_pipeline.h"     /* POST /api/camera 的 AI-VGA 约束检查 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "web_server";

/* ------------------------------------------------------------------ */
/*  POST /led                                                          */
/* ------------------------------------------------------------------ */

esp_err_t api_led_handler(httpd_req_t *req)
{
    char *body = read_body(req, 256);
    if (!body) {
        return json_error(req, "Empty or too large body", HTTPD_400_BAD_REQUEST);
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        return json_error(req, "Invalid JSON", HTTPD_400_BAD_REQUEST);
    }

    cJSON *brightness_item = cJSON_GetObjectItem(json, "brightness");
    if (!brightness_item || !cJSON_IsNumber(brightness_item)) {
        cJSON_Delete(json);
        return json_error(req, "Missing or invalid 'brightness' (0-100)", HTTPD_400_BAD_REQUEST);
    }

    int brightness = brightness_item->valueint;
    if (brightness < 0 || brightness > 100) {
        cJSON_Delete(json);
        return json_error(req, "Brightness must be 0-100", HTTPD_400_BAD_REQUEST);
    }

    cJSON_Delete(json);

    esp_err_t err = flash_led_set_brightness((uint8_t)brightness);
    if (err != ESP_OK) {
        return json_error(req, "Flash LED control failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    cJSON *data = cJSON_CreateObject();
    if (data) {
        cJSON_AddNumberToObject(data, "brightness", brightness);
    }
    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  GET /camera                                                         */
/* ------------------------------------------------------------------ */

/* 当前相机状态 JSON（GET 与 POST 共用；POST 追加 ignored 键集） */
static cJSON *camera_state_json(void)
{
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        return NULL;
    }

    cJSON_AddNumberToObject(data, "cam_framesize",  config_get_cam_framesize());
    cJSON_AddNumberToObject(data, "cam_quality",    config_get_cam_quality());
    /* 契约扩展（2026-09-04）：画质滑杆边界由板端声明，前端据此钳制输入。
     * v1.9（issue #27）：min 随当前档位收紧（板级定标），POST 校验同源 */
    cJSON_AddNumberToObject(data, "quality_min",    camera_quality_min_for(config_get_cam_framesize()));
    cJSON_AddNumberToObject(data, "quality_max",    CAMERA_QUALITY_MAX);
    cJSON_AddNumberToObject(data, "cam_brightness", config_get_cam_brightness());
    cJSON_AddNumberToObject(data, "cam_contrast",   config_get_cam_contrast());
    cJSON_AddNumberToObject(data, "cam_saturation", config_get_cam_saturation());
    cJSON_AddNumberToObject(data, "cam_sharpness",  config_get_cam_sharpness());
    cJSON_AddBoolToObject(data,   "cam_hmirror",    config_get_cam_hmirror());
    cJSON_AddBoolToObject(data,   "cam_vflip",      config_get_cam_vflip());
    cJSON_AddStringToObject(data, "resolution", camera_framesize_name(config_get_cam_framesize()));

    /* 契约 v1.0：分辨率列表动态下发（esp32-camera framesize_t 刻度）。
     * 三层上限（2026-09-04 家族统一）：sensor ∩ board ∩ memory，
     * 上限历史依据（SVGA 稳定/XGA 起冷启动取帧死）见 camera_driver.h。 */
    {
        static const char *res_labels[] = {
            [10] "VGA (640x480)",  [11] "SVGA (800x600)",
            [12] "XGA (1024x768)", [13] "HD (1280x720)",
            [14] "SXGA (1280x1024)", [15] "UXGA (1600x1200)",
        };
        int eff_max = camera_get_effective_max_res();
        cJSON *res_arr = cJSON_CreateArray();
        for (int fs = 10; fs <= eff_max && fs < (int)(sizeof(res_labels) / sizeof(res_labels[0])); fs++) {
            if (!res_labels[fs]) continue;
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "label", res_labels[fs]);
            cJSON_AddNumberToObject(item, "value", fs);
            cJSON_AddItemToArray(res_arr, item);
        }
        cJSON_AddItemToObject(data, "supported_resolutions", res_arr);
        /* 契约扩展（2026-09-04）：上限被哪一层钳制（sensor/board/memory） */
        cJSON_AddStringToObject(data, "res_cap_source", camera_res_cap_source());
    }

    return data;
}

esp_err_t api_camera_get_handler(httpd_req_t *req)
{
    cJSON *data = camera_state_json();
    if (!data) {
        return json_error(req, "Out of memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  POST /camera                                                        */
/* ------------------------------------------------------------------ */

esp_err_t api_camera_post_handler(httpd_req_t *req)
{
    char *body = read_body(req, 2048);
    if (!body) {
        return json_error(req, "Empty or too large body", HTTPD_400_BAD_REQUEST);
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        return json_error(req, "Invalid JSON", HTTPD_400_BAD_REQUEST);
    }

    cJSON *item;
    bool need_sensor_apply = false;
    bool need_reinit = false;
    uint8_t new_framesize = config_get_cam_framesize();
    uint8_t new_quality = config_get_cam_quality();
    int updated = 0;

    /* cam_framesize — 板级上限内自由选择（区间校验，非单值锁定） */
    item = cJSON_GetObjectItem(json, "cam_framesize");
    if (item && cJSON_IsNumber(item)) {
        int val = item->valueint;
            if (val < 0 || val > 15) {
            cJSON_Delete(json);
            return json_error(req, "cam_framesize out of range (0-15)", HTTPD_400_BAD_REQUEST);
        }
        if (val > camera_get_effective_max_res()) {
            cJSON_Delete(json);
            char msg[96];
            snprintf(msg, sizeof(msg),
                "cam_framesize %d exceeds effective max %d (source: %s, PIT-021)",
                val, camera_get_effective_max_res(), camera_res_cap_source());
            return json_error(req, msg, HTTPD_400_BAD_REQUEST);
        }
        /* AI safety check — reject non-VGA if any AI feature is enabled */
        if (!camera_framesize_is_vga((uint8_t)val) &&
            (ai_is_enabled(AI_FEATURE_FACE_DETECT) ||
             ai_is_enabled(AI_FEATURE_MOTION_DETECT) ||
             ai_is_enabled(AI_FEATURE_QR_DECODE))) {
            cJSON_Delete(json);
            return json_error(req, "Disable AI to use non-VGA resolution", HTTPD_400_BAD_REQUEST);
        }
        new_framesize = (uint8_t)val;
        need_reinit = true;
    }

    /* cam_quality — v1.9（issue #27）：下限按目标档位收紧（帧尺寸键
     * 先于本键解析，new_framesize 已含本次切换目标） */
    item = cJSON_GetObjectItem(json, "cam_quality");
    if (item && cJSON_IsNumber(item)) {
        int val = item->valueint;
        uint8_t qmin = camera_quality_min_for(new_framesize);
        if (val < qmin || val > CAMERA_QUALITY_MAX) {
            char msg[80];
            snprintf(msg, sizeof(msg), "cam_quality out of range (%d-%d at this resolution)",
                     qmin, CAMERA_QUALITY_MAX);
            cJSON_Delete(json);
            return json_error(req, msg, HTTPD_400_BAD_REQUEST);
        }
        new_quality = (uint8_t)val;
        need_reinit = true;
    }

    /* Sensor keys: brightness, contrast, saturation, sharpness */
    const char *sensor_int_keys[] = { "cam_brightness", "cam_contrast", "cam_saturation", "cam_sharpness" };
    for (size_t i = 0; i < 4; i++) {
        item = cJSON_GetObjectItem(json, sensor_int_keys[i]);
        if (item && cJSON_IsNumber(item)) {
            int val = item->valueint;
            if (val < -2 || val > 2) {
                cJSON_Delete(json);
                char msg[64];
                snprintf(msg, sizeof(msg), "%s out of range (-2..+2)", sensor_int_keys[i]);
                return json_error(req, msg, HTTPD_400_BAD_REQUEST);
            }
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", val);
            config_set(sensor_int_keys[i], buf);
            need_sensor_apply = true;
            updated++;
        }
    }

    /* Sensor keys: hmirror, vflip */
    const char *sensor_bool_keys[] = { "cam_hmirror", "cam_vflip" };
    for (size_t i = 0; i < 2; i++) {
        item = cJSON_GetObjectItem(json, sensor_bool_keys[i]);
        if (item && cJSON_IsBool(item)) {
            config_set(sensor_bool_keys[i], item->valueint ? "1" : "0");
            need_sensor_apply = true;
            updated++;
        }
    }

    /* Persist config changes */
    if (updated > 0) {
        config_save();
    }

    /* Apply sensor settings live */
    if (need_sensor_apply) {
        camera_apply_sensor_settings();
    }

    /* Coordinated reinit for framesize/quality changes */
    if (need_reinit) {
        esp_err_t err = camera_reinit(new_framesize, new_quality);
        if (err != ESP_OK) {
            cJSON_Delete(json);
            return json_error(req, "Camera reinit failed", HTTPD_500_INTERNAL_SERVER_ERROR);
        }
    }

    /* 未知键回显（契约 v1.9 §5，issue #27）：静默 ok:true 曾让"quality"
     * 这类裸键的拼写错误排障半天——现在点名 WARN + 响应带 ignored 键集 */
    static const char *known_keys[] = {
        "cam_framesize", "cam_quality", "cam_brightness", "cam_contrast",
        "cam_saturation", "cam_sharpness", "cam_hmirror", "cam_vflip",
    };
    cJSON *ignored = cJSON_CreateArray();
    for (cJSON *child = json->child; child; child = child->next) {
        if (!child->string) continue;   /* 数组体：成员无名，跳过（防 strcmp(NULL)） */
        bool known = false;
        for (size_t i = 0; i < sizeof(known_keys) / sizeof(known_keys[0]); i++) {
            if (strcmp(child->string, known_keys[i]) == 0) { known = true; break; }
        }
        if (!known) cJSON_AddItemToArray(ignored, cJSON_CreateString(child->string));
    }
    if (cJSON_GetArraySize(ignored) > 0) {
        char *names = cJSON_PrintUnformatted(ignored);
        ESP_LOGW(TAG, "POST /api/camera ignored unknown keys: %s", names ? names : "?");
        free(names);
    }
    cJSON_Delete(json);

    cJSON *data = camera_state_json();
    if (!data) {
        cJSON_Delete(ignored);
        return json_error(req, "Out of memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    cJSON_AddItemToObject(data, "ignored", ignored);

    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  GET /api/capture — 单帧 JPEG（契约 v1.0 核心端点）                  */
/* ------------------------------------------------------------------ */

esp_err_t api_capture_handler(httpd_req_t *req)
{
    flash_viewers_notify_capture();  /* 板级扩展：拍照期间保持闪光灯判定在场 */
    camera_fb_t *fb = camera_capture();
    if (!fb) {
        return json_error(req, "Capture failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, must-revalidate");
    httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /api/led — 闪光灯亮度读取（契约 v1.0：GET+POST 成对）           */
/* ------------------------------------------------------------------ */

esp_err_t api_led_get_handler(httpd_req_t *req)
{
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "brightness", flash_led_get_brightness());
    return json_ok(req, data);
}
