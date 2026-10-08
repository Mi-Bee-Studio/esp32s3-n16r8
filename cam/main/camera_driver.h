/*
 * MiBee Cam v0.1 — Camera driver for OV3660
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Public API:
 *   - camera_init()       Initialize OV3660 via esp_camera, verify sensor ID
 *   - camera_capture()    Grab one JPEG frame from the camera
 */

#pragma once

#include "esp_camera.h"

/* JPEG quality bounds (lower = better quality / larger frames) — driver
 * invariant, applies to every board using esp32-camera: JPEG frame buffers
 * are sized w*h/5 (max 1:5 compression), q<10 overflows that budget on
 * complex scenes and produces truncated frames (family finding 2026-09-04,
 * PITFALLS PIT-021; validated on ai-thinker/seeed/luatos hardware).
 * NOTE: resolution cap for THIS board is not yet measured — do not copy
 * sister-board caps; measure on hardware before restricting 0-15. */
#define CAMERA_QUALITY_MIN 10
#define CAMERA_QUALITY_MAX 63

/* 契约 v1.9 §5（issue #27）：画质下限随档位收紧（只紧不松）——fb 按
 * w×h/5 分配，高档位配低 q 的码流超限截帧（.119 实测 SXGA@q10 NO-SOI
 * 风暴，PIT-021 家族边界在高分辨率档内移）。板级定标（n16r8 实测，
 * .119 OV3660/.113 OV5640）：≤SVGA=10、XGA/HD=12、SXGA=14。 */
uint8_t camera_quality_min_for(uint8_t framesize);

/* ── 分辨率三层上限（2026-09-04 家族统一，PIT-021 附录）──────────────
 * effective = min(传感器上限, 板级实测上限, 运行时 fb 预算)，刻度是
 * esp32-camera framesize_t 原始枚举（VGA=10 … 5MP=24）：
 *  1. sensor  — 组件自动检测（camera_sensor_info_t.max_size，本模组
 *               OV3660→QXGA=19），换传感器候选表自适应收缩；
 *  2. board   — 本板实测常数（唯一手工数字，禁止沿用姐妹板数值）：
 *               2026-09-04 两轮上板复测 + 双网复核，**SVGA(11) 稳定，
 *               XGA(12) 起致命**（XGA 冷启动即 fb_get NULL 楔死整板，
 *               GT/MiBeeAP2 双网一致，采集侧判定；PSRAM 8MB 充裕，是
 *               模组/传感器/DVP 组合极限而非内存问题）；
 *  3. memory  — 运行时 fb 预算校验（宽*高/5*fb_count + floor ≤ 可用
 *               PSRAM），只能收紧，防御 PSRAM 退化态。
 * 历史教训：首轮“仅 VGA”结论被两条污染链毁掉（NVS 键名 16 字符令
 * config_save 整体失败 → “关 AI”存不住 → AI 强制 VGA 钳制每晨复活；
 * 加上校验写成 `!=max` 单值锁）。详见 PIT-021/022。
 * /api/camera 下发 res_cap_source 报告被哪一层钳制（诊断用）。 */
#define CAMERA_RES_BOARD_MAX 14   /* FRAMESIZE_SXGA：2026-09-04 定稿（XCLK16M+PSRAM80M+WiFi迁移实测，UXGA init 失败） */
int camera_get_effective_max_res(void);

/** @brief 上限被哪一层钳制（sensor / board / memory），静态字符串 */
const char *camera_res_cap_source(void);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the OV3660 camera sensor.
 *
 * Pins are drawn from Kconfig (CONFIG_CAMERA_PIN_*).
 * Frame buffer is allocated in PSRAM, format JPEG, size VGA.
 *
 * @return ESP_OK on success, or an esp_err_t code on failure.
 */
esp_err_t camera_init(void);

/**
 * @brief Capture a single frame from the camera.
 *
 * Returns a pointer to the frame buffer.  The caller MUST call
 * esp_camera_fb_return(fb) after processing the frame.
 *
 * @return Pointer to camera_fb_t, or NULL if capture failed.
 */
camera_fb_t *camera_capture(void);

/**
 * @brief Apply sensor settings (brightness, contrast, saturation, sharpness, mirror, flip) from config.
 */
void camera_apply_sensor_settings(void);

/**
 * @brief Coordinated camera re-initialization.
 *
 * Stops AI + broadcaster, deinits camera, reinits with new framesize/quality,
 * restarts broadcaster + AI (if it was running and framesize is VGA).
 * The new values are also persisted to config.
 *
 * @param framesize New framesize (framesize_t enum value, 0..24).
 * @param quality   New JPEG quality (0-63).
 * @return ESP_OK on success, or esp_err_t on init failure.
 */
esp_err_t camera_reinit(uint8_t framesize, uint8_t quality);

/**
 * @brief Validate and return a framesize enum value.
 *        Clamps out-of-range values to VGA (10).
 */
uint8_t camera_framesize_from_int(uint8_t val);

/**
 * @brief Get human-readable name for a framesize enum value.
 */
const char *camera_framesize_name(uint8_t framesize);

/**
 * @brief Check if a framesize is VGA (640x480).
 *        FRAMESIZE_VGA = 10 in esp32-camera framesize_t enum.
 */
bool camera_framesize_is_vga(uint8_t framesize);

/**
 * @brief Detected sensor model string ("OV2640"/"OV3660"/"OV5640"/"unknown").
 */
const char *camera_sensor_name(void);

#ifdef __cplusplus
}
#endif
