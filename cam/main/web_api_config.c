/*
 * MiBee Cam v0.1 — Web server (config API)
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GET/POST /api/config — whitelist key application with
 * contract §4 validation, NVS persistence, WiFi/timezone/CSI side effects.
 *
 * Split from the monolithic web_server.c (issue #19, pure code motion):
 * route table + lifecycle stay in web_server.c; handler bodies live in
 * per-domain modules. Public API: web_server.h; internal seam:
 * web_server_internal.h. Behavior contract: docs/api-contract.md.
 */

#include "web_server.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "config_manager.h"
#include "camera_driver.h"   /* CAMERA_QUALITY_MAX / camera_quality_min_for（契约 §4 校验） */
#include "csi_motion.h"      /* csi 键族热应用（契约 v1.7） */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "web_server";

/* ------------------------------------------------------------------ */
/*  GET /config                                                        */
/* ------------------------------------------------------------------ */

esp_err_t api_config_get_handler(httpd_req_t *req)
{
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        return json_error(req, "Out of memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    /* 家族 schema 版本（契约 §1；字段名与 ai-thinker 一致） */
    cJSON_AddNumberToObject(data, "schema_version",  CONFIG_SCHEMA_VERSION);

    cJSON_AddStringToObject(data, "wifi_ssid",        config_get_wifi_ssid());
    /* Mask password */
    if (config_get_wifi_pass() && config_get_wifi_pass()[0]) {
        cJSON_AddStringToObject(data, "wifi_pass", "****");
    } else {
        cJSON_AddStringToObject(data, "wifi_pass", "");
    }
    cJSON_AddStringToObject(data, "wifi_ssid_2",      config_get_wifi_ssid_2());
    if (config_get_wifi_pass_2() && config_get_wifi_pass_2()[0]) {
        cJSON_AddStringToObject(data, "wifi_pass_2", "****");
    } else {
        cJSON_AddStringToObject(data, "wifi_pass_2", "");
    }
    cJSON_AddStringToObject(data, "device_name",      config_get_device_name());
    cJSON_AddStringToObject(data, "timezone",         config_get_timezone());
    cJSON_AddBoolToObject(data,   "allow_ap_fallback", config_get_allow_ap_fallback());
    cJSON_AddNumberToObject(data, "cam_framesize",    config_get_cam_framesize());
    cJSON_AddNumberToObject(data, "cam_fps",          config_get_cam_fps());
    cJSON_AddNumberToObject(data, "cam_quality",      config_get_cam_quality());
    cJSON_AddNumberToObject(data, "xclk_freq_mhz",    config_get_xclk_freq_mhz());
    /* AI JSON 名随契约 §3.2 收敛为 ai_*_en */
    cJSON_AddBoolToObject(data,   "ai_face_en",       config_get_ai_face_enable());
    cJSON_AddBoolToObject(data,   "ai_motion_en",     config_get_ai_motion_enable());
    cJSON_AddBoolToObject(data,   "ai_qr_en",         config_get_ai_qr_enable());
    cJSON_AddBoolToObject(data,   "onvif_enable",     config_get_onvif_enable());
    cJSON_AddBoolToObject(data,   "onvif_events",     config_get_onvif_events());   /* 契约 v1.5 */
    /* CSI 调参键族（契约 v1.7；CSI-off 亦有默认值） */
    cJSON_AddBoolToObject(data,   "csi_enabled",      config_get_csi_enabled());
    cJSON_AddNumberToObject(data, "csi_threshold",    (double)config_get_csi_threshold());
    cJSON_AddNumberToObject(data, "csi_on_hits",      (double)config_get_csi_on_hits());
    cJSON_AddNumberToObject(data, "csi_off_hits",     (double)config_get_csi_off_hits());
    cJSON_AddNumberToObject(data, "csi_profile",      (double)config_get_csi_profile());
    cJSON_AddBoolToObject(data,   "csi_auto_heal",    config_get_csi_auto_heal());
    /* 板级扩展：观看者驱动闪光灯（有此键=板支持，SPA 据此显示开关） */
    cJSON_AddBoolToObject(data,   "flash_viewers",    config_get_flash_viewers());
    cJSON_AddNumberToObject(data, "cam_brightness", config_get_cam_brightness());
    cJSON_AddNumberToObject(data, "cam_contrast",   config_get_cam_contrast());
    cJSON_AddNumberToObject(data, "cam_saturation", config_get_cam_saturation());
    cJSON_AddNumberToObject(data, "cam_sharpness",  config_get_cam_sharpness());
    cJSON_AddBoolToObject(data,   "cam_hmirror",    config_get_cam_hmirror());
    cJSON_AddBoolToObject(data,   "cam_vflip",      config_get_cam_vflip());

    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  POST /config                                                        */
/* ------------------------------------------------------------------ */

esp_err_t api_config_post_handler(httpd_req_t *req)
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
    
    /* Iterate over known config keys and apply via config_set()
     * 白名单 = 契约 §3 JSON 字段名；校验矩阵 = 契约 §4（越界一律 400）。 */
    cJSON *item;
    int updated = 0;
    bool wifi_changed = false;
    bool tz_changed = false;
    bool csi_changed = false;
    const float prev_csi_thr = config_get_csi_threshold();
    const char *known_keys[] = {
        "wifi_ssid", "wifi_pass", "wifi_ssid_2", "wifi_pass_2",
        "device_name", "timezone", "allow_ap_fallback",
        "cam_framesize", "cam_fps", "cam_quality", "xclk_freq_mhz",
        "ai_face_en", "ai_motion_en", "ai_qr_en",
        "onvif_enable", "onvif_events",
        "cam_brightness", "cam_contrast", "cam_saturation", "cam_sharpness",
        "cam_hmirror", "cam_vflip",
        "csi_enabled", "csi_threshold", "csi_on_hits", "csi_off_hits",
        "csi_profile", "csi_auto_heal",
        "flash_viewers",
        NULL
    };

    for (int i = 0; known_keys[i]; i++) {
        const char *key = known_keys[i];
        item = cJSON_GetObjectItem(json, key);
        if (!item) continue;
        if (strncmp(key, "csi_", 4) == 0) csi_changed = true;
        if (strcmp(key, "wifi_ssid") == 0 ||
            strcmp(key, "wifi_pass") == 0 ||
            strcmp(key, "wifi_ssid_2") == 0 ||
            strcmp(key, "wifi_pass_2") == 0) {
            wifi_changed = true;
        }

        /* JSON 名 → NVS 键映射：allow_ap_fallback(17) 超 NVS 15 字符限，
         * 契约 §3.1 键名 ap_fallback */
        const char *cfg_key =
            (strcmp(key, "allow_ap_fallback") == 0) ? "ap_fallback" : key;

        /* —— 字符串字段：先验长度拒绝（契约 §4：不静默截断凭据） —— */
        if (cJSON_IsString(item)) {
            const char *val = item->valuestring;
            if (strcmp(val, "****") == 0) {
                continue;   /* masked value echoed back — no change */
            }
            size_t maxlen = config_key_max_len(cfg_key);
            if (maxlen == 0) {
                continue;   /* numeric key sent as string — ignore */
            }
            size_t vlen = strlen(val);
            if (strcmp(key, "timezone") == 0) {
                /* POSIX TZ，契约 §3.1 str≤47（§4 的 1-64 以缓冲域为准收紧） */
                if (vlen == 0 || vlen > maxlen - 1) {
                    char msg[80];
                    snprintf(msg, sizeof(msg), "timezone must be 1-%u characters",
                             (unsigned)(maxlen - 1));
                    cJSON_Delete(json);
                    return json_error(req, msg, HTTPD_400_BAD_REQUEST);
                }
                tz_changed = true;
            } else if (vlen > maxlen - 1) {
                char msg[96];
                snprintf(msg, sizeof(msg), "%s too long (max %u characters)",
                         key, (unsigned)(maxlen - 1));
                cJSON_Delete(json);
                return json_error(req, msg, HTTPD_400_BAD_REQUEST);
            }
            if (config_set(cfg_key, val) == ESP_OK) {
                updated++;
            }
            continue;
        }

        if (!cJSON_IsBool(item) && !cJSON_IsNumber(item)) {
            continue;
        }
        int val = item->valueint;
        /* csi_threshold：JSON float 0-1 → NVS 百分刻度 u8（契约 v1.7） */
        if (strcmp(key, "csi_threshold") == 0) {
            double t = item->valuedouble;
            if (t != 0.0 && (t < 0.05 || t > 1.0)) {
                cJSON_Delete(json);
                return json_error(req, "csi_threshold must be 0 (auto) or 0.05-1.0",
                                  HTTPD_400_BAD_REQUEST);
            }
            val = (int)(t * 100.0 + 0.5);
        }

        /* —— 契约 §4 数值域校验（越界 400）—— */
        if (strcmp(key, "cam_quality") == 0) {
            /* 画质边界（2026-09-04，驱动不变量）：q<10 撞 esp32-camera 的
             * w*h/5 JPEG fb 预算产生截断帧，PIT-021。v1.9（issue #27）：
             * 下限随目标档位收紧（同 body 带新档位则按新档位算） */
            cJSON *fs_item = cJSON_GetObjectItem(json, "cam_framesize");
            uint8_t eff_fs = (fs_item && cJSON_IsNumber(fs_item))
                ? (uint8_t)fs_item->valueint : config_get_cam_framesize();
            uint8_t qmin = camera_quality_min_for(eff_fs);
            if (val < qmin || val > CAMERA_QUALITY_MAX) {
                char msg[80];
                snprintf(msg, sizeof(msg), "cam_quality out of range (%d-%d at this resolution)",
                         qmin, CAMERA_QUALITY_MAX);
                cJSON_Delete(json);
                return json_error(req, msg, HTTPD_400_BAD_REQUEST);
            }
        } else if (strcmp(key, "cam_fps") == 0) {
            if (val < 1 || val > 30) {
                cJSON_Delete(json);
                return json_error(req, "cam_fps out of range (1-30)", HTTPD_400_BAD_REQUEST);
            }
        } else if (strcmp(key, "cam_framesize") == 0) {
            /* 合法域 = 板 supported_resolutions（契约 §2：三层上限交集，
             * 与 GET /api/camera 动态生成的 10..effective_max 同源） */
            int eff_max = camera_get_effective_max_res();
            if (val < 10 || val > eff_max) {
                char msg[96];
                snprintf(msg, sizeof(msg),
                         "cam_framesize %d unsupported (max %d, cap source: %s)",
                         val, eff_max, camera_res_cap_source());
                cJSON_Delete(json);
                return json_error(req, msg, HTTPD_400_BAD_REQUEST);
            }
        } else if (strcmp(key, "xclk_freq_mhz") == 0) {
            if (val != 10 && val != 16 && val != 20) {
                cJSON_Delete(json);
                return json_error(req, "xclk_freq_mhz must be 10, 16 or 20", HTTPD_400_BAD_REQUEST);
            }
        } else if (strcmp(key, "allow_ap_fallback") == 0) {
            if (val != 0 && val != 1) {
                cJSON_Delete(json);
                return json_error(req, "allow_ap_fallback must be 0 or 1", HTTPD_400_BAD_REQUEST);
            }
        } else if (strcmp(key, "cam_brightness") == 0 ||
                   strcmp(key, "cam_contrast") == 0 ||
                   strcmp(key, "cam_saturation") == 0 ||
                   strcmp(key, "cam_sharpness") == 0) {
            if (val < -2 || val > 2) {
                char msg[64];
                snprintf(msg, sizeof(msg), "%s out of range (-2..+2)", key);
                cJSON_Delete(json);
                return json_error(req, msg, HTTPD_400_BAD_REQUEST);
            }
        } else if (strcmp(key, "csi_on_hits") == 0 || strcmp(key, "csi_off_hits") == 0) {
            if (val < 1 || val > 20) {
                cJSON_Delete(json);
                return json_error(req, "csi hits must be 1-20", HTTPD_400_BAD_REQUEST);
            }
        } else if (strcmp(key, "csi_profile") == 0) {
            if (val < 0 || val > 1) {
                cJSON_Delete(json);
                return json_error(req, "csi_profile must be 0 or 1", HTTPD_400_BAD_REQUEST);
            }
        } else if (strcmp(key, "csi_enabled") == 0 || strcmp(key, "csi_auto_heal") == 0
                   || strcmp(key, "flash_viewers") == 0) {
            if (val != 0 && val != 1) {
                cJSON_Delete(json);
                return json_error(req, "csi bool keys must be 0 or 1", HTTPD_400_BAD_REQUEST);
            }
        }

        char value_str[16];
        snprintf(value_str, sizeof(value_str), "%d", val);
        if (config_set(cfg_key, value_str) == ESP_OK) {
            updated++;
        }
    }

    cJSON_Delete(json);

    if (updated > 0) {
        config_save();
    }

    /* CSI 键族热应用（契约 v1.7；threshold 显式改 0=恢复自动，带重校准） */
    if (csi_changed) {
        if (config_get_csi_threshold() == 0.0f && prev_csi_thr > 0.0f) {
            csi_motion_set_threshold(0.0f);
        }
        csi_motion_apply_config();
    }

    /* timezone 立即生效（契约 §3.1，同 seeed/ai-thinker）：本板无 NTP，
     * 供 /api/time 手动设时后的 localtime() 换算使用 */
    if (tz_changed) {
        setenv("TZ", config_get_timezone(), 1);
        tzset();
    }

    if (wifi_changed) {
        ESP_LOGW(TAG, "WiFi config changed -- rebooting in 1s");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    cJSON *resp_data = cJSON_CreateObject();
    if (resp_data) {
        cJSON_AddNumberToObject(resp_data, "updated", updated);
    }
    return json_ok(req, resp_data);
}
