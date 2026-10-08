/*
 * MiBee Cam v0.1 — Web server (AI API)
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * POST /api/ai · GET /api/ai/status — feature toggles and
 * live detection results (face/motion/QR).
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
#include "ai_pipeline.h"
#include <stdlib.h>

static const char *TAG = "web_server";

/* ------------------------------------------------------------------ */
/*  POST /ai                                                           */
/* ------------------------------------------------------------------ */

esp_err_t api_ai_handler(httpd_req_t *req)
{
    char *body = read_body(req, 512);
    if (!body) {
        return json_error(req, "Empty or too large body", HTTPD_400_BAD_REQUEST);
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        return json_error(req, "Invalid JSON", HTTPD_400_BAD_REQUEST);
    }

    /* Toggle AI features — persist config AND apply live via ai_enable(). */
    cJSON *item;
    int updated = 0;

    item = cJSON_GetObjectItem(json, "face");
    if (item && cJSON_IsBool(item)) {
        config_set("ai_face_en", item->valueint ? "1" : "0");   /* 契约 §3.2 键名（≤15 字符，PIT-022） */
        ai_enable(AI_FEATURE_FACE_DETECT, item->valueint ? true : false);
        ESP_LOGI(TAG, "AI face detection %s", item->valueint ? "enabled" : "disabled");
        updated++;
    }

    item = cJSON_GetObjectItem(json, "motion");
    if (item && cJSON_IsBool(item)) {
        config_set("ai_motion_en", item->valueint ? "1" : "0");   /* NVS 键 ≤15 字符（PIT-022） */
        ai_enable(AI_FEATURE_MOTION_DETECT, item->valueint ? true : false);
        ESP_LOGI(TAG, "AI motion detection %s", item->valueint ? "enabled" : "disabled");
        updated++;
    }

    item = cJSON_GetObjectItem(json, "qr");
    if (item && cJSON_IsBool(item)) {
        config_set("ai_qr_en", item->valueint ? "1" : "0");   /* 契约 §3.2 键名 */
        ai_enable(AI_FEATURE_QR_DECODE, item->valueint ? true : false);
        ESP_LOGI(TAG, "AI QR detection %s", item->valueint ? "enabled" : "disabled");
        updated++;
    }

    cJSON_Delete(json);

    if (updated > 0) {
        config_save();
    }

    cJSON *data = cJSON_CreateObject();
    if (data) {
        cJSON_AddNumberToObject(data, "updated", updated);
        cJSON_AddBoolToObject(data, "face",   config_get_ai_face_enable());
        cJSON_AddBoolToObject(data, "motion", config_get_ai_motion_enable());
        cJSON_AddBoolToObject(data, "qr",     config_get_ai_qr_enable());
    }
    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  GET /ai/status                                                     */
/* ------------------------------------------------------------------ */

esp_err_t ai_status_get_handler(httpd_req_t *req)
{
    ai_result_t result;
    if (!ai_get_result(&result)) {
        return json_error(req, "AI pipeline not running", HTTPD_404_NOT_FOUND);
    }

    cJSON *data = cJSON_CreateObject();
    if (!data) {
        return json_error(req, "Out of memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    /* Face */
    cJSON *face = cJSON_CreateObject();
    cJSON_AddNumberToObject(face, "count", result.face.count);
    cJSON *boxes = cJSON_CreateArray();
    for (int i = 0; i < result.face.count && i < AI_MAX_FACES; i++) {
        cJSON *box = cJSON_CreateObject();
        cJSON_AddNumberToObject(box, "x", result.face.faces[i].x);
        cJSON_AddNumberToObject(box, "y", result.face.faces[i].y);
        cJSON_AddNumberToObject(box, "w", result.face.faces[i].w);
        cJSON_AddNumberToObject(box, "h", result.face.faces[i].h);
        cJSON_AddNumberToObject(box, "confidence", result.face.faces[i].confidence);
        cJSON_AddItemToArray(boxes, box);
    }
    cJSON_AddItemToObject(face, "boxes", boxes);
    cJSON_AddItemToObject(data, "face", face);

    /* Motion */
    cJSON *motion = cJSON_CreateObject();
    cJSON_AddNumberToObject(motion, "score", result.motion.score);
    cJSON_AddItemToObject(data, "motion", motion);

    /* QR */
    cJSON *qr = cJSON_CreateObject();
    cJSON_AddNumberToObject(qr, "count", result.qr.count);
    cJSON *codes = cJSON_CreateArray();
    for (int i = 0; i < result.qr.count && i < AI_MAX_QR_CODES; i++) {
        cJSON_AddItemToArray(codes, cJSON_CreateString(result.qr.strings[i]));
    }
    cJSON_AddItemToObject(qr, "codes", codes);
    cJSON_AddItemToObject(data, "qr", qr);

    /* Sequence */
    cJSON_AddNumberToObject(data, "seq", result.frame_seq);

    return json_ok(req, data);
}
