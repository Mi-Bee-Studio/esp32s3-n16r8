/*
 * MiBee Cam v0.1 — NVS-backed config manager
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Persists camera and application configuration to NVS namespace "mibee_cfg".
 * Each key is stored individually (not as a blob) for forward/backward
 * compatibility.  Missing keys fall back to compiled-in defaults.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"

/* 家族配置契约 v1.0（docs/config-contract.md §1）：mibee_cfg 逐键 NVS 的
 * 家族 schema 版本键值（u16）。新增键 = 缺键取默认；改名/语义变更须
 * bump 并在契约 §8 登记（含 lazy 迁移步骤）。 */
#define CONFIG_SCHEMA_VERSION 2   /* v2.0：web_password/rtsp_user/rtsp_pass 删除 */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief  Load config from NVS.  Missing keys keep compiled defaults.
 * @return ESP_OK on success (NVS missing is NOT an error — defaults apply)
 * @note   Call AFTER nvs_flash_init() in the main boot sequence.
 */
esp_err_t config_load(void);

/**
 * @brief  Set a single key-value pair in memory.
 * @param  key   One of: wifi_ssid, wifi_pass, wifi_ssid_2, wifi_pass_2,
 *               cam_framesize, cam_quality, cam_fps, timezone, ap_fallback,
 *               xclk_freq_mhz, ai_face_en, ai_motion_en, ai_qr_en,
 *               onvif_enable,
 *               cam_brightness, cam_contrast, cam_saturation, cam_sharpness,
 *               cam_hmirror, cam_vflip
 *               (NVS key names per contract §3; JSON name allow_ap_fallback
 *               maps to key ap_fallback — 17 chars exceeds the NVS 15-char
 *               limit).
 * @param  value String representation.  Numeric keys parse uint8.
 * @return ESP_OK, ESP_ERR_INVALID_ARG for unknown keys or bad values.
 * @note   Does NOT write to NVS — call config_save() to persist.
 */
esp_err_t config_set(const char *key, const char *value);

/**
 * @brief  Max string length (excluding NUL) a key accepts, 0 for non-strings.
 * @note   契约 §4：字符串字段先验长度拒绝（不静默截断凭据）——HTTP 写入
 *         路径用本函数预检，超长直接 400。
 */
size_t config_key_max_len(const char *key);

/**
 * @brief  Persist all in-memory values to NVS.
 * @return ESP_OK, or the first NVS error encountered.
 */
esp_err_t config_save(void);

/**
 * @brief  Reset all values to compiled defaults and persist.
 */
esp_err_t config_reset(void);

/* ------------------------------------------------------------------ */
/*  Typed accessors                                                    */
/* ------------------------------------------------------------------ */

const char *config_get_wifi_ssid(void);
const char *config_get_wifi_pass(void);
const char *config_get_wifi_ssid_2(void);   /* 备用网络（可空） */
const char *config_get_wifi_pass_2(void);
const char *config_get_device_name(void);
const char *config_get_timezone(void);      /* POSIX TZ，空 = UTC（契约 §3.1） */
bool        config_get_allow_ap_fallback(void); /* STA 失败是否兜底 AP（默认开） */
uint8_t     config_get_xclk_freq_mhz(void); /* ∈{10,16,20}，本板默认 16（契约 §5） */
uint8_t     config_get_cam_framesize(void);
uint8_t     config_get_cam_quality(void);
uint8_t     config_get_cam_fps(void);       /* 1-30，default 15（契约 §3.1） */
bool        config_get_ai_face_enable(void);
bool        config_get_ai_motion_enable(void);
bool        config_get_ai_qr_enable(void);
bool        config_get_onvif_enable(void);
bool        config_get_onvif_events(void);   /* 契约 v1.5：MotionAlarm 生成开关 */
/* CSI 调参键族（契约 v1.7；threshold 换算 float 0=auto 或 0.05-1.0） */
bool        config_get_csi_enabled(void);
float       config_get_csi_threshold(void);
uint8_t     config_get_csi_on_hits(void);
uint8_t     config_get_csi_off_hits(void);
uint8_t     config_get_csi_profile(void);
bool        config_get_csi_auto_heal(void);
bool        config_get_flash_viewers(void);   /* 板级扩展：观看者驱动闪光灯 */

int8_t     config_get_cam_brightness(void);
int8_t     config_get_cam_contrast(void);
int8_t     config_get_cam_saturation(void);
int8_t     config_get_cam_sharpness(void);
bool       config_get_cam_hmirror(void);
bool       config_get_cam_vflip(void);

cJSON *config_get_json(void);

#ifdef __cplusplus
}
#endif
