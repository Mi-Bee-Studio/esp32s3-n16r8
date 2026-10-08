/*
 * MiBee Cam v0.1 — OV3660 camera driver
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Wraps the espressif/esp32-camera component with:
 *   - Pin configuration from Kconfig (CONFIG_CAMERA_PIN_*)
 *   - Sensor ID verification  (expects PID 0x77 for OV3660)
 *   - Single-frame JPEG capture helper
 */

#include "camera_driver.h"
#include "config_manager.h"
#include "frame_broadcaster.h"
#include "ai_pipeline.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
static const char *TAG = "camera_driver";

/* ------------------------------------------------------------------ */
/*  Camera pin mapping — sourced from sdkconfig.defaults via Kconfig   */
/* ------------------------------------------------------------------ */

static const camera_config_t s_camera_cfg = {
    .pin_pwdn     = CONFIG_CAMERA_PIN_PWDN,
    .pin_reset    = CONFIG_CAMERA_PIN_RESET,
    .pin_xclk     = CONFIG_CAMERA_PIN_XCLK,
    .pin_sccb_sda = CONFIG_CAMERA_PIN_SIOD,
    .pin_sccb_scl = CONFIG_CAMERA_PIN_SIOC,

    .pin_d7       = CONFIG_CAMERA_PIN_D7,
    .pin_d6       = CONFIG_CAMERA_PIN_D6,
    .pin_d5       = CONFIG_CAMERA_PIN_D5,
    .pin_d4       = CONFIG_CAMERA_PIN_D4,
    .pin_d3       = CONFIG_CAMERA_PIN_D3,
    .pin_d2       = CONFIG_CAMERA_PIN_D2,
    .pin_d1       = CONFIG_CAMERA_PIN_D1,
    .pin_d0       = CONFIG_CAMERA_PIN_D0,

    .pin_vsync    = CONFIG_CAMERA_PIN_VSYNC,
    .pin_href     = CONFIG_CAMERA_PIN_HREF,
    .pin_pclk     = CONFIG_CAMERA_PIN_PCLK,

    .xclk_freq_hz = 16000000,   /* 编译期基准；运行期由 config xclk_freq_mhz
                                   覆盖（契约 §3.1/§5，本板默认 16——
                                   2026-09-04 定稿：20M 下 XGA+ 帧损坏
                                   （NO-SOI/OVF），16M 实测 SXGA 稳） */
    .ledc_timer   = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG,
    .frame_size   = FRAMESIZE_VGA,       /* overridden by config at init */
    .jpeg_quality = 12,                   /* overridden by config at init */
    .fb_count     = 2,

    .fb_location  = CAMERA_FB_IN_PSRAM,
    .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
};

/* ------------------------------------------------------------------ */
/*  三层分辨率上限（sensor ∩ board ∩ memory，PIT-021 附录）            */
/* ------------------------------------------------------------------ */

/* memory 层：esp32-camera 的 JPEG fb 按 宽*高/5 分配（cam_hal 同式），
 * 预算 = fb_size * fb_count，分完后须仍留 floor 给 lwIP 大缓冲等消费者。
 * 只能收紧上限（防御 PSRAM 退化态），不会把 effective 放宽超过实测常数。 */
#define CAMERA_FB_COUNT      2     /* 与 s_camera_cfg 一致 */
#define CAMERA_RES_MEM_FLOOR (512 * 1024)

static bool s_camera_inited = false;
static const char *s_cap_source = "board";

/* framesize → 尺寸表，下标 0 对应 FRAMESIZE_VGA（钉死 esp32-camera 2.1.x
 * 枚举序；组件枚举漂移时由 _Static_assert 在构建期暴露） */
static const struct { uint16_t w, h; } s_fs_dims[] = {
    { 640,  480},  { 800,  600},  {1024,  768},  {1280,  720},  {1280, 1024},
    {1600, 1200},  {1920, 1080},  { 720, 1280},  { 864, 1536},  {2048, 1536},
    {2560, 1440},  {2560, 1600},  {1080, 1920},  {2560, 1920},  {2592, 1944},
};
_Static_assert(FRAMESIZE_VGA == 10, "s_fs_dims pinned to esp32-camera 2.1.x enum");

static size_t fb_bytes_for_fs(int fs)
{
    int idx = fs - (int)FRAMESIZE_VGA;
    if (idx < 0 || (size_t)idx >= sizeof(s_fs_dims) / sizeof(s_fs_dims[0])) {
        return 0;
    }
    return (size_t)s_fs_dims[idx].w * s_fs_dims[idx].h / 5;
}

static bool fb_budget_ok(int fs)
{
    size_t need = fb_bytes_for_fs(fs) * CAMERA_FB_COUNT;
    size_t cur_fb = s_camera_inited
        ? fb_bytes_for_fs((int)config_get_cam_framesize()) * CAMERA_FB_COUNT : 0;
    size_t avail = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) + cur_fb;
    return need != 0 && avail >= need + CAMERA_RES_MEM_FLOOR;
}

/** sensor 层：查 esp32-camera 组件自带能力表（单一事实源，勿手抄 PID 表）。
 *  未初始化/未知 PID → 回退板级常数（不放宽）。 */
static int sensor_max_framesize(void)
{
    sensor_t *s = esp_camera_sensor_get();
    camera_sensor_info_t *info = s ? esp_camera_sensor_get_info(&s->id) : NULL;
    if (info && (int)info->max_size >= (int)FRAMESIZE_VGA) {
        return (int)info->max_size;
    }
    if (s) {
        ESP_LOGW(TAG, "Unknown sensor PID 0x%04X — sensor layer falls back to board max",
                 s->id.PID);
    }
    return CAMERA_RES_BOARD_MAX;
}

uint8_t camera_quality_min_for(uint8_t framesize)
{
    if (framesize >= 14) return 14;              /* SXGA+（本板上限） */
    if (framesize >= 12) return 12;              /* XGA / HD */
    return CAMERA_QUALITY_MIN;                   /* ≤SVGA 家族底 */
}

int camera_get_effective_max_res(void)
{
    int sensor_cap = sensor_max_framesize();
    int board_cap  = CAMERA_RES_BOARD_MAX;
    int cap = (sensor_cap < board_cap) ? sensor_cap : board_cap;
    while (cap > (int)FRAMESIZE_VGA && !fb_budget_ok(cap)) {
        cap--;
    }
    if (cap == sensor_cap) {
        s_cap_source = "sensor";
    } else if (cap == board_cap) {
        s_cap_source = "board";
    } else {
        s_cap_source = "memory";
    }
    return cap;
}

const char *camera_res_cap_source(void)
{
    return s_cap_source;
}

/* ------------------------------------------------------------------ */
/*  camera_init()                                                      */
/* ------------------------------------------------------------------ */

esp_err_t camera_init(void)
{
    /* Read config values */
    uint8_t framesize = config_get_cam_framesize();
    if (framesize > camera_get_effective_max_res()) {
        ESP_LOGW(TAG, "Config framesize=%u exceeds effective max %d (source: %s) — clamping (PIT-021)",
                 framesize, camera_get_effective_max_res(), camera_res_cap_source());
        framesize = (uint8_t)camera_get_effective_max_res();
    }
    uint8_t quality = config_get_cam_quality();
    if (quality < CAMERA_QUALITY_MIN) quality = CAMERA_QUALITY_MIN;
    if (quality > CAMERA_QUALITY_MAX) quality = CAMERA_QUALITY_MAX;

    /* Force VGA when AI is enabled (pipeline hardcodes 640x480) */
    if (!camera_framesize_is_vga(framesize) &&
        (config_get_ai_face_enable() || config_get_ai_motion_enable() || config_get_ai_qr_enable())) {
        ESP_LOGW(TAG, "AI enabled but framesize is not VGA (cur=%d) — forcing VGA", framesize);
        framesize = camera_framesize_from_int(10); /* FRAMESIZE_VGA */
    }

    ESP_LOGI(TAG, "Initializing OV3660 camera (framesize=%d quality=%d, PSRAM fb)", framesize, quality);

    /* Build dynamic config from static pins + config values */
    camera_config_t cfg = s_camera_cfg;
    cfg.frame_size   = (framesize_t)framesize;
    cfg.jpeg_quality = quality;
    cfg.xclk_freq_hz = (uint32_t)config_get_xclk_freq_mhz() * 1000000U;

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init() failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "esp_camera_init() OK — probing sensor ID");

    /* ---- 传感器身份：查组件能力表（单一事实源）-------------------
     * 2026-09-04 纠偏：本模组实测 PID=0x3660（组件表 OV3660 项），历史
     * 注释/文档里的"PID=0x77"是错的（0x77 是 OV7725）。能力上限由
     * camera_get_effective_max_res() 的 sensor 层给出——换接其他传感器
     * （如 OV2640）不再硬拒，候选表自动收缩。 */
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == NULL) {
        ESP_LOGE(TAG, "esp_camera_sensor_get() returned NULL");
        return ESP_ERR_CAMERA_NOT_DETECTED;
    }

    uint16_t pid = sensor->id.PID;
    ESP_LOGI(TAG, "Sensor PID: 0x%04X  MID: 0x%02X:%02X", pid, sensor->id.MIDH, sensor->id.MIDL);

    camera_sensor_info_t *info = esp_camera_sensor_get_info(&sensor->id);
    if (info) {
        ESP_LOGI(TAG, "Sensor confirmed: %s (PID 0x%04X, sensor max framesize=%d)",
                 info->name, info->pid, (int)info->max_size);
    } else {
        ESP_LOGW(TAG, "Unknown sensor PID=0x%04X — continuing, sensor layer falls back to board max",
                 pid);
    }

    /* Apply sensor settings from config */
    camera_apply_sensor_settings();

    s_camera_inited = true;

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  camera_capture()                                                   */
/* ------------------------------------------------------------------ */

camera_fb_t *camera_capture(void)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGE(TAG, "Frame capture failed (esp_camera_fb_get returned NULL)");
        return NULL;
    }

    ESP_LOGI(TAG, "Captured frame: %ux%u  format=%u  len=%u",
             fb->width, fb->height,
             (unsigned)fb->format,
             (unsigned)fb->len);

    return fb;
}

/* ------------------------------------------------------------------ */
/*  camera_apply_sensor_settings()                                     */
/* ------------------------------------------------------------------ */

void camera_apply_sensor_settings(void)
{
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) {
        ESP_LOGW(TAG, "Cannot apply sensor settings — sensor is NULL");
        return;
    }

    int8_t brightness = config_get_cam_brightness();
    int8_t contrast   = config_get_cam_contrast();
    int8_t saturation = config_get_cam_saturation();
    int8_t sharpness  = config_get_cam_sharpness();
    int     hmirror   = config_get_cam_hmirror() ? 1 : 0;
    int     vflip     = config_get_cam_vflip()   ? 1 : 0;

    if (sensor->set_brightness) sensor->set_brightness(sensor, brightness);
    if (sensor->set_contrast)   sensor->set_contrast(sensor, contrast);
    if (sensor->set_saturation) sensor->set_saturation(sensor, saturation);
    if (sensor->set_sharpness)  sensor->set_sharpness(sensor, sharpness);
    if (sensor->set_hmirror)    sensor->set_hmirror(sensor, hmirror);
    if (sensor->set_vflip)      sensor->set_vflip(sensor, vflip);

    ESP_LOGI(TAG, "Sensor settings applied: bright=%d contrast=%d sat=%d sharp=%d mirror=%d flip=%d",
             brightness, contrast, saturation, sharpness, hmirror, vflip);
}

/* ------------------------------------------------------------------ */
/*  camera_reinit()                                                    */
/* ------------------------------------------------------------------ */

esp_err_t camera_reinit(uint8_t framesize, uint8_t quality)
{
    if (framesize > camera_get_effective_max_res()) {
        ESP_LOGW(TAG, "reinit framesize=%u exceeds effective max %d (source: %s) — clamping",
                 framesize, camera_get_effective_max_res(), camera_res_cap_source());
        framesize = (uint8_t)camera_get_effective_max_res();
    }
    if (quality < CAMERA_QUALITY_MIN) quality = CAMERA_QUALITY_MIN;
    if (quality > CAMERA_QUALITY_MAX) quality = CAMERA_QUALITY_MAX;
    ESP_LOGI(TAG, "Camera reinit requested: framesize=%d quality=%d", framesize, quality);

    /* Record AI running state before stopping */
    bool ai_was_running = ai_is_enabled(AI_FEATURE_FACE_DETECT) ||
                          ai_is_enabled(AI_FEATURE_MOTION_DETECT) ||
                          ai_is_enabled(AI_FEATURE_QR_DECODE);

    /* 1. Coordinated stop: AI first (it reads frames), then broadcaster */
    if (ai_was_running) {
        ai_stop_task();
    }
    frame_broadcaster_stop();

    /* 2. Deinit camera */
    s_camera_inited = false;
    esp_err_t deinit_err = esp_camera_deinit();
    if (deinit_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_deinit() failed: %s", esp_err_to_name(deinit_err));
    }

    /* 3. Update config in memory and persist（先留旧值：失败路径按旧值回滚。
     * 2026-09-04 XGA 复测实锤的坑：此前失败路径从 config 读"刚保存的新值"
     * 去"恢复"，等于用失败档位反复 init，板子留在 fb_get NULL 死循环直到
     * 人工救） */
    uint8_t prev_framesize = config_get_cam_framesize();
    uint8_t prev_quality   = config_get_cam_quality();
    char framesize_str[4];
    char quality_str[4];
    snprintf(framesize_str, sizeof(framesize_str), "%u", (unsigned)framesize);
    snprintf(quality_str, sizeof(quality_str), "%u", (unsigned)quality);
    config_set("cam_framesize", framesize_str);
    config_set("cam_quality", quality_str);
    config_save();

    /* 4. Re-init with new settings */
    camera_config_t cfg = s_camera_cfg;
    cfg.frame_size   = (framesize_t)framesize;
    cfg.jpeg_quality = quality;
    cfg.xclk_freq_hz = (uint32_t)config_get_xclk_freq_mhz() * 1000000U;

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera reinit failed: %s — rolling back to framesize=%u",
                 esp_err_to_name(err), prev_framesize);
        /* 回滚配置到旧值并按旧档位重建（勿用 config 现值——那是失败的新档） */
        char prev_fs_str[4], prev_q_str[4];
        snprintf(prev_fs_str, sizeof(prev_fs_str), "%u", (unsigned)prev_framesize);
        snprintf(prev_q_str, sizeof(prev_q_str), "%u", (unsigned)prev_quality);
        config_set("cam_framesize", prev_fs_str);
        config_set("cam_quality", prev_q_str);
        config_save();
        cfg.frame_size   = (framesize_t)prev_framesize;
        cfg.jpeg_quality = prev_quality;
        esp_err_t rb = esp_camera_init(&cfg);
        if (rb != ESP_OK) {
            /* 回滚也失败说明驱动被失败 init 楔死（2026-09-04 UXGA 试验实录），
             * 只有重启能救。boot 路径不走本函数，不会形成重启环。 */
            ESP_LOGE(TAG, "Rollback init also failed: %s — rebooting to recover",
                     esp_err_to_name(rb));
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
        camera_apply_sensor_settings();
        frame_broadcaster_start();
        if (ai_was_running) ai_start_task();
        return err;
    }

    /* 5. Apply sensor settings */
    camera_apply_sensor_settings();
    s_camera_inited = true;

    /* 6. Restart broadcaster */
    frame_broadcaster_start();

    /* 7. Restart AI only if it was running AND framesize is VGA */
    if (ai_was_running && camera_framesize_is_vga(framesize)) {
        ai_start_task();
    } else if (ai_was_running && !camera_framesize_is_vga(framesize)) {
        ESP_LOGW(TAG, "AI was running but new framesize=%d is not VGA — AI stays stopped", framesize);
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Helper functions                                                   */
/* ------------------------------------------------------------------ */

uint8_t camera_framesize_from_int(uint8_t val)
{
    /* The esp32-camera framesize_t enum has 25 valid values (0..24).
     * FRAMESIZE_INVALID = 25. If out of range, default to VGA (10). */
    if (val > 24) return 10;
    return val;
}

const char *camera_framesize_name(uint8_t framesize)
{
    /* MUST match the framesize_t enum from esp32-camera driver/include/sensor.h */
    switch (framesize) {
        case 0:  return "96X96";
        case 1:  return "QQVGA";
        case 2:  return "128X128";
        case 3:  return "QCIF";
        case 4:  return "HQVGA";
        case 5:  return "240X240";
        case 6:  return "QVGA";
        case 7:  return "320X320";
        case 8:  return "CIF";
        case 9:  return "HVGA";
        case 10: return "VGA";
        case 11: return "SVGA";
        case 12: return "XGA";
        case 13: return "HD";
        case 14: return "SXGA";
        case 15: return "UXGA";
        case 16: return "FHD";
        case 17: return "P_HD";
        case 18: return "P_3MP";
        case 19: return "QXGA";
        case 20: return "QHD";
        case 21: return "WQXGA";
        case 22: return "P_FHD";
        case 23: return "QSXGA";
        case 24: return "5MP";
        default: return "unknown";
    }
}

/** @brief 返回检测到的传感器型号字符串（契约 v1.0: status.camera 字段）
 *  2026-09-04：改查组件能力表取名——旧手抄映射把 OV3660 标成 PID 0x77
 *  （实际 0x3660，0x77 是 OV7725），导致实戴传感器上报 "unknown"。 */
const char *camera_sensor_name(void)
{
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) {
        return "unknown";
    }
    camera_sensor_info_t *info = esp_camera_sensor_get_info(&sensor->id);
    return info ? info->name : "unknown";
}

bool camera_framesize_is_vga(uint8_t framesize)
{
    /* FRAMESIZE_VGA = 10 in esp32-camera framesize_t enum */
    return framesize == 10;
}
