/*
 * MiBee Cam v0.1 — NVS-backed config manager（家族配置契约 v1.0）
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * NVS namespace: "mibee_cfg"（契约 §1：四仓统一逐键格式）
 *
 * Design:
 *   - Individual key-value pairs (NOT a blob) so future firmware versions
 *     can add/remove keys without migration code.
 *   - config_load() reads every known key; NVS_ERR_NOT_FOUND keeps the
 *     compiled default for that key（契约 §1 单键自治：缺键 = 编译默认值）.
 *   - config_save() writes every key then calls nvs_commit(). A single
 *     key write failure only WARNs and the batch continues（契约 §1，
 *     PIT-022 另一半教训：不得因单键失败中止整批写入）.
 *   - schema_ver (u16) is written on every save; missing on load → seeded
 *     with the compiled family schema version（契约 §1 版本键）.
 *   - Renamed keys carry a lazy migration: on load, if the new key is
 *     missing, the legacy key name is read instead (logged once); the new
 *     key is written on the next save（契约 §6 n16r8 行：键名对齐）.
 *   - config_set() accepts string values and parses numeric keys as uint8.
 *   - A FreeRTOS mutex protects write paths (config_set, config_save,
 *     config_reset) for safe concurrent access from web server and
 *     AT command tasks.
 */

#include "config_manager.h"
#include "sdkconfig.h"
#include "camera_driver.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define NVS_NAMESPACE  "mibee_cfg"
#define TAG            "config"
#define KEY_SCHEMA_VER "schema_ver"   /* 契约 §1 家族版本键（u16） */

/* NVS 键名 ≤15 字符（NVS 硬限制）——所有键字面量构建期断言（契约 §1，
 * PIT-022：超长键曾使整个 config_save() 失败） */
#define KEY_ASSERT(k) _Static_assert(sizeof(k) <= 16, "NVS key >15 chars: " k)
KEY_ASSERT("schema_ver");
KEY_ASSERT("wifi_ssid");
KEY_ASSERT("wifi_pass");
KEY_ASSERT("wifi_ssid_2");
KEY_ASSERT("wifi_pass_2");
KEY_ASSERT("device_name");
KEY_ASSERT("timezone");
KEY_ASSERT("ap_fallback");
KEY_ASSERT("cam_framesize");
KEY_ASSERT("cam_fps");
KEY_ASSERT("cam_quality");
KEY_ASSERT("cam_vflip");
KEY_ASSERT("cam_hmirror");
KEY_ASSERT("onvif_enable");
KEY_ASSERT("onvif_events");
KEY_ASSERT("csi_enabled");
KEY_ASSERT("csi_threshold");
KEY_ASSERT("csi_on_hits");
KEY_ASSERT("csi_off_hits");
KEY_ASSERT("csi_profile");
KEY_ASSERT("csi_auto_heal");
KEY_ASSERT("flash_viewers");
KEY_ASSERT("ai_face_en");
KEY_ASSERT("ai_motion_en");
KEY_ASSERT("ai_qr_en");
KEY_ASSERT("cam_brightness");
KEY_ASSERT("cam_contrast");
KEY_ASSERT("cam_saturation");
KEY_ASSERT("cam_sharpness");
KEY_ASSERT("xclk_freq_mhz");
/* 契约对齐前的存量键名（lazy 迁移读取用，见 migrate_lazy_legacy_keys） */
KEY_ASSERT("ai_face_enable");
KEY_ASSERT("ai_qr_enable");

/* ------------------------------------------------------------------ */
/*  Internal state                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char    wifi_ssid[33];
    char    wifi_pass[65];
    char    wifi_ssid_2[33];   /* 备用网络（2026-09-04：此前本板单 WiFi，
                                  .119 板位主网弱态失联时无路可退） */
    char    wifi_pass_2[65];
    uint8_t cam_framesize;
    uint8_t cam_fps;           /* 契约 §3.1：1-30，default 15（本板暂无消费者，
                                  persist + expose；ONVIF FrameRateLimit 已接） */
    uint8_t cam_quality;
    bool    ai_face_enable;
    bool    ai_motion_enable;
    bool    ai_qr_enable;
    bool    onvif_enable;
    bool    onvif_events;   /* 契约 v1.5：ONVIF MotionAlarm 生成开关（默认关） */
    /* CSI 感知调参键族（契约 v1.7 §3.2）。threshold 以百分刻度 u8 持久化
     * （0=auto；5-100 ↔ 0.05-1.00），JSON 面由 web 层换算 float。 */
    bool    csi_enabled;
    uint8_t csi_threshold_pct;
    uint8_t csi_on_hits;
    uint8_t csi_off_hits;
    uint8_t csi_profile;
    bool    csi_auto_heal;
    int8_t  cam_brightness;   /* OV3660 brightness: -2..+2 */
    int8_t  cam_contrast;     /* OV3660 contrast: -2..+2 */
    int8_t  cam_saturation;   /* OV3660 saturation: -2..+2 */
    int8_t  cam_sharpness;    /* OV3660 sharpness range TBD from sensor_t */
    bool    cam_hmirror;      /* horizontal mirror */
    bool    cam_vflip;        /* vertical flip */
    bool    flash_viewers;    /* 板级扩展：有观看者（流/拍照）自动开闪光灯，默认 false */
    char    device_name[33];  /* 契约 v1.0: 设备名称 */
    char    timezone[48];     /* 契约 §3.1：POSIX TZ str≤47，空 = UTC */
    bool    allow_ap_fallback;/* 契约 §3.1：STA 失败兜底 AP（NVS 键 ap_fallback，
                                 JSON 名 17 字符超 NVS 限，故键名缩短） */
    uint8_t xclk_freq_mhz;    /* 契约 §3.1/§5：∈{10,16,20}，本板默认 16
                                 （2026-09-05 实测 20M 下 XGA+ 帧损坏） */
} config_t;

static config_t s_config;

static SemaphoreHandle_t s_config_mutex = NULL;

static const config_t s_defaults = {
    .wifi_ssid       = "",
    .wifi_pass       = "",
    .wifi_ssid_2     = "",
    .wifi_pass_2     = "",
    .cam_framesize   = 10,              /* FRAMESIZE_VGA — safe for OV3660 smoke test */
    .cam_fps         = 15,              /* 契约 §3.1 家族默认 */
    .cam_quality     = 12,
    .ai_face_enable  = true,
    .ai_motion_enable = true,
    .ai_qr_enable    = true,
    .onvif_enable    = true,
    .onvif_events    = false,
    .csi_enabled     = true,
    .csi_threshold_pct = 0,   /* 0 = auto（校准+settle） */
    .csi_on_hits     = 4,
    .csi_off_hits    = 3,
    .csi_profile     = 0,     /* Lightweight */
    .csi_auto_heal   = true,  /* 自愈环默认开（PIT-041） */
    .cam_brightness  = 0,
    .cam_contrast    = 0,
    .cam_saturation  = 0,
    .cam_sharpness   = 0,
    .cam_hmirror     = false,
    .flash_viewers   = false,  /* 板级扩展：观看者驱动闪光灯，默认关 */
    .cam_vflip       = false,
    .device_name     = "MiBeeCam",
    .timezone        = "",               /* 空 = UTC（契约 §3.1） */
    .allow_ap_fallback = true,           /* 契约 §3.1 家族默认 1 = 保留现行为 */
    .xclk_freq_mhz   = 16,               /* 契约 §5 板级覆盖：n16r8 = 16 MHz */
};

/* ------------------------------------------------------------------ */
/*  Key table — maps string key names to their metadata                */
/* ------------------------------------------------------------------ */

typedef enum {
    TYPE_STRING,
    TYPE_U8,
    TYPE_I8,
} key_type_t;

typedef struct {
    const char *name;
    key_type_t  type;
    size_t      offset;         /* byte offset within config_t */
    size_t      max_len;        /* for strings: buffer size    */
} key_entry_t;

/* Offsets for string members */
#define OFF_STR(field)  offsetof(config_t, field), sizeof(((config_t *)0)->field)
#define OFF_U8(field)   offsetof(config_t, field), 1u
#define OFF_I8(field)   offsetof(config_t, field), 1u

static const key_entry_t s_keys[] = {
    /* 键名 = 契约 §3 NVS 键（≤15 字符，KEY_ASSERT 全量断言）。
     * config_set()/config_key_max_len() 按本表匹配；JSON 字段名与键名
     * 不同者（allow_ap_fallback）由 web 层映射。 */
    { "wifi_ssid",       TYPE_STRING, OFF_STR(wifi_ssid)       },
    { "wifi_pass",       TYPE_STRING, OFF_STR(wifi_pass)       },
    { "wifi_ssid_2",     TYPE_STRING, OFF_STR(wifi_ssid_2)     },
    { "wifi_pass_2",     TYPE_STRING, OFF_STR(wifi_pass_2)     },
    { "cam_framesize",   TYPE_U8,     OFF_U8(cam_framesize)    },
    { "cam_fps",         TYPE_U8,     OFF_U8(cam_fps)          },
    { "cam_quality",     TYPE_U8,     OFF_U8(cam_quality)      },
    /* AI 键名收敛到契约 §3.2（ai_face_en/ai_motion_en/ai_qr_en，JSON 名同步）。
     * PIT-022：ai_motion_enable(16) 曾使 config_save() 整体失败 → 改
     * ai_motion_en（旧非法键从未写入过，无需迁移）；ai_face_enable(13)/
     * ai_qr_enable(12) 合法且有存量数据 → lazy 迁移见
     * migrate_lazy_legacy_keys()。 */
    { "ai_face_en",      TYPE_U8,     OFF_U8(ai_face_enable)   },
    { "ai_motion_en",    TYPE_U8,     OFF_U8(ai_motion_enable) },
    { "ai_qr_en",        TYPE_U8,     OFF_U8(ai_qr_enable)     },
    { "onvif_enable",    TYPE_U8,     OFF_U8(onvif_enable)     },
    { "onvif_events",    TYPE_U8,     OFF_U8(onvif_events)     },
    { "csi_enabled",     TYPE_U8,     OFF_U8(csi_enabled)      },
    { "csi_threshold",   TYPE_U8,     OFF_U8(csi_threshold_pct)},
    { "csi_on_hits",     TYPE_U8,     OFF_U8(csi_on_hits)      },
    { "csi_off_hits",    TYPE_U8,     OFF_U8(csi_off_hits)     },
    { "csi_profile",     TYPE_U8,     OFF_U8(csi_profile)      },
    { "csi_auto_heal",   TYPE_U8,     OFF_U8(csi_auto_heal)    },
    { "flash_viewers",   TYPE_U8,     OFF_U8(flash_viewers)    },   /* 板级扩展 */
    { "cam_brightness",  TYPE_I8,     OFF_I8(cam_brightness)   },
    { "cam_contrast",    TYPE_I8,     OFF_I8(cam_contrast)     },
    { "cam_saturation",  TYPE_I8,     OFF_I8(cam_saturation)   },
    { "cam_sharpness",   TYPE_I8,     OFF_I8(cam_sharpness)    },
    { "cam_hmirror",     TYPE_U8,     OFF_U8(cam_hmirror)      },
    { "cam_vflip",       TYPE_U8,     OFF_U8(cam_vflip)        },
    { "device_name",     TYPE_STRING, OFF_STR(device_name)     },
    { "timezone",        TYPE_STRING, OFF_STR(timezone)        },
    { "ap_fallback",     TYPE_U8,     OFF_U8(allow_ap_fallback) },
    { "xclk_freq_mhz",   TYPE_U8,     OFF_U8(xclk_freq_mhz)    },
};

#define NUM_KEYS (sizeof(s_keys) / sizeof(s_keys[0]))

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static const key_entry_t *find_key(const char *name)
{
    for (size_t i = 0; i < NUM_KEYS; i++) {
        if (strcmp(s_keys[i].name, name) == 0) {
            return &s_keys[i];
        }
    }
    return NULL;
}

static void *field_ptr(const key_entry_t *k)
{
    return (uint8_t *)&s_config + k->offset;
}


/* Read a string key from NVS; if missing, keep the default.
 * 契约 §1 单键自治：读失败只影响该键（WARN），缺键 = 编译默认值。 */
static esp_err_t read_str_nvs(nvs_handle_t h, const key_entry_t *k)
{
    char *buf = (char *)field_ptr(k);

    size_t len = 0;
    esp_err_t err = nvs_get_str(h, k->name, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;   /* keep default */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read key '%s' failed: %s — keeping default",
                 k->name, esp_err_to_name(err));
        return err;
    }
    if (len > k->max_len) {
        ESP_LOGW(TAG, "key '%s' value %u bytes > buffer %u — keeping default",
                 k->name, (unsigned)len, (unsigned)k->max_len);
        return ESP_ERR_INVALID_SIZE;
    }
    return nvs_get_str(h, k->name, buf, &len);
}

/* Read a uint8 key from NVS; if missing, keep the default. */
static esp_err_t read_u8_nvs(nvs_handle_t h, const key_entry_t *k)
{
    uint8_t *val = (uint8_t *)field_ptr(k);
    esp_err_t err = nvs_get_u8(h, k->name, val);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;   /* keep default */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read key '%s' failed: %s — keeping default",
                 k->name, esp_err_to_name(err));
    }
    return err;
}

/* Read an int8 key from NVS; if missing, keep the default. */
static esp_err_t read_i8_nvs(nvs_handle_t h, const key_entry_t *k)
{
    int8_t *val = (int8_t *)field_ptr(k);
    esp_err_t err = nvs_get_i8(h, k->name, val);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;   /* keep default */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read key '%s' failed: %s — keeping default",
                 k->name, esp_err_to_name(err));
    }
    return err;
}

/* Lazy key migration（契约 §6 n16r8 行：键名对齐不丢存量数据）。
 * 新键缺失时回落读旧键名（log once），下次 save 写入新键；幂等——
 * 新键一旦存在即跳过。ai_motion_enable(16) 从未成功写入 NVS
 * （PIT-022），不在迁移表内。 */
static void migrate_lazy_legacy_keys(nvs_handle_t h)
{
    static const struct {
        const char *legacy;
        const char *current;
    } renames[] = {
        { "ai_face_enable", "ai_face_en" },
        { "ai_qr_enable",   "ai_qr_en"   },
    };

    for (size_t i = 0; i < sizeof(renames) / sizeof(renames[0]); i++) {
        const key_entry_t *k = find_key(renames[i].current);
        uint8_t probe = 0;
        if (!k || k->type != TYPE_U8) {
            continue;
        }
        if (nvs_get_u8(h, renames[i].current, &probe) == ESP_OK) {
            continue;   /* new key already present — nothing to do */
        }
        if (nvs_get_u8(h, renames[i].legacy, &probe) == ESP_OK) {
            *(uint8_t *)field_ptr(k) = probe;
            ESP_LOGW(TAG, "lazy NVS migration: '%s' <- legacy key '%s' (rewritten on next save)",
                     renames[i].current, renames[i].legacy);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t config_load(void)
{
    /* Prevent double-init: re-entry would leak the config mutex */
    static bool s_config_loaded = false;
    if (s_config_loaded) {
        return ESP_OK;
    }

    /* Start with defaults */
    memcpy(&s_config, &s_defaults, sizeof(s_config));

    /* Create mutex for thread-safe write access */
    s_config_mutex = xSemaphoreCreateMutex();
    if (!s_config_mutex) {
        ESP_LOGE(TAG, "Failed to create config mutex");
        return ESP_ERR_NO_MEM;
    }

    bool have_schema_ver = false;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No stored config, using defaults");
    } else {
        for (size_t i = 0; i < NUM_KEYS; i++) {
            const key_entry_t *k = &s_keys[i];
            if (k->type == TYPE_STRING) {
                read_str_nvs(h, k);
            } else if (k->type == TYPE_U8) {
                read_u8_nvs(h, k);
            } else if (k->type == TYPE_I8) {
                read_i8_nvs(h, k);
            }
        }
        /* 改名键 lazy 迁移（契约 §6）：放在表驱动读取之后，仅补新键缺失者 */
        migrate_lazy_legacy_keys(h);

        uint16_t schema_ver = 0;
        have_schema_ver = (nvs_get_u16(h, KEY_SCHEMA_VER, &schema_ver) == ESP_OK);
        nvs_close(h);
    }

    /* schema_ver 缺失（契约 §1/§6：本仓逐键本就无版本键）→ 补种 = 1，
     * 其余缺键保持编译默认。设备数据不动。 */
    if (!have_schema_ver) {
        nvs_handle_t hw;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &hw) == ESP_OK) {
            esp_err_t werr = nvs_set_u16(hw, KEY_SCHEMA_VER, CONFIG_SCHEMA_VERSION);
            if (werr == ESP_OK) {
                werr = nvs_commit(hw);
            }
            if (werr != ESP_OK) {
                ESP_LOGW(TAG, "seed %s failed: %s (retry on next save)",
                         KEY_SCHEMA_VER, esp_err_to_name(werr));
            } else {
                ESP_LOGI(TAG, "Seeded %s=%d (family config schema)",
                         KEY_SCHEMA_VER, CONFIG_SCHEMA_VERSION);
            }
            nvs_close(hw);
        }
    }

    /* 板级边界（契约 §4 加载钳制）：分辨率上限 / fps 域 / xclk 白名单。
     * 旧配置可能存过越界值（当时配置与实际输出脱节）——加载钳回域内 */
    if (s_config.cam_framesize > CAMERA_RES_BOARD_MAX) {
        ESP_LOGW(TAG, "Legacy cam_framesize=%u clamped to board max %d on load",
                 s_config.cam_framesize, CAMERA_RES_BOARD_MAX);
        s_config.cam_framesize = CAMERA_RES_BOARD_MAX;
    }
    if (s_config.cam_fps == 0 || s_config.cam_fps > 30) {
        ESP_LOGW(TAG, "cam_fps=%u out of range, defaulting to 15", s_config.cam_fps);
        s_config.cam_fps = 15;
    }
    if (s_config.xclk_freq_mhz != 10 && s_config.xclk_freq_mhz != 16 &&
        s_config.xclk_freq_mhz != 20) {
        ESP_LOGW(TAG, "xclk_freq_mhz=%u not in {10,16,20}, defaulting to 16",
                 s_config.xclk_freq_mhz);
        s_config.xclk_freq_mhz = 16;
    }

    ESP_LOGI(TAG, "Config loaded from NVS (schema v%d)", CONFIG_SCHEMA_VERSION);
    s_config_loaded = true;
    return ESP_OK;
}

esp_err_t config_set(const char *key, const char *value)
{
    const key_entry_t *k = find_key(key);
    if (!k || !value) {
        /* PIT-022：未知键曾是“静默失败”（调用方不查返回值），点名告警 */
        ESP_LOGW(TAG, "config_set: unknown key '%s' — value DROPPED", key ? key : "(null)");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (k->type == TYPE_STRING) {
        char *buf = (char *)field_ptr(k);
        strncpy(buf, value, k->max_len - 1);
        buf[k->max_len - 1] = '\0';
    } else if (k->type == TYPE_U8) {
        unsigned long uv = strtoul(value, NULL, 10);
        if (uv > 255) {
            xSemaphoreGive(s_config_mutex);
            return ESP_ERR_INVALID_ARG;
        }
        *(uint8_t *)field_ptr(k) = (uint8_t)uv;
    } else if (k->type == TYPE_I8) {
        long sv = strtol(value, NULL, 10);
        if (sv < -128 || sv > 127) {
            xSemaphoreGive(s_config_mutex);
            return ESP_ERR_INVALID_ARG;
        }
        *(int8_t *)field_ptr(k) = (int8_t)sv;
    }

    xSemaphoreGive(s_config_mutex);

    return ESP_OK;
}

size_t config_key_max_len(const char *key)
{
    const key_entry_t *k = find_key(key);
    if (!k || k->type != TYPE_STRING) {
        return 0;   /* non-string key (or unknown) — no length domain */
    }
    return k->max_len;   /* buffer size incl. NUL */
}

esp_err_t config_save(void)
{
    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write: %s", esp_err_to_name(err));
        xSemaphoreGive(s_config_mutex);
        return err;
    }

    /* 契约 §1 单键自治（PIT-022 教训）：单键写失败只 WARN、批次继续，
     * 最后统一 commit。旧实现遇错即返回，其后所有键永不落盘。 */
    bool all_ok = true;
    for (size_t i = 0; i < NUM_KEYS; i++) {
        const key_entry_t *k = &s_keys[i];
        esp_err_t werr;
        if (k->type == TYPE_STRING) {
            werr = nvs_set_str(h, k->name, (const char *)field_ptr(k));
        } else if (k->type == TYPE_U8) {
            werr = nvs_set_u8(h, k->name, *(const uint8_t *)field_ptr(k));
        } else {
            werr = nvs_set_i8(h, k->name, *(const int8_t *)field_ptr(k));
        }
        if (werr != ESP_OK) {
            ESP_LOGW(TAG, "write NVS key '%s' failed: %s — key skipped, batch continues",
                     k->name, esp_err_to_name(werr));
            all_ok = false;
        }
    }

    /* schema_ver 随每次保存落盘（契约 §1） */
    esp_err_t werr = nvs_set_u16(h, KEY_SCHEMA_VER, CONFIG_SCHEMA_VERSION);
    if (werr != ESP_OK) {
        ESP_LOGW(TAG, "write NVS key '%s' failed: %s — key skipped",
                 KEY_SCHEMA_VER, esp_err_to_name(werr));
        all_ok = false;
    }

    err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Config saved to NVS%s", all_ok ? "" : " (partial — see key warnings above)");
    }

    xSemaphoreGive(s_config_mutex);
    return err;
}

esp_err_t config_reset(void)
{

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    memcpy(&s_config, &s_defaults, sizeof(s_config));
    xSemaphoreGive(s_config_mutex);
    ESP_LOGI(TAG, "Config reset to factory defaults");
    return config_save();
}

/* ------------------------------------------------------------------ */
/*  Typed accessors                                                    */
/* ------------------------------------------------------------------ */

const char *config_get_wifi_ssid(void)      { return s_config.wifi_ssid; }
const char *config_get_wifi_pass(void)      { return s_config.wifi_pass; }
const char *config_get_wifi_ssid_2(void)    { return s_config.wifi_ssid_2; }
const char *config_get_wifi_pass_2(void)    { return s_config.wifi_pass_2; }
uint8_t     config_get_cam_framesize(void)  { return s_config.cam_framesize; }
uint8_t     config_get_cam_fps(void)        { return s_config.cam_fps; }
uint8_t     config_get_cam_quality(void)    { return s_config.cam_quality; }
bool        config_get_ai_face_enable(void) { return s_config.ai_face_enable; }
bool        config_get_ai_motion_enable(void) { return s_config.ai_motion_enable; }
bool        config_get_ai_qr_enable(void)   { return s_config.ai_qr_enable; }
bool        config_get_onvif_enable(void)   { return s_config.onvif_enable; }
bool        config_get_onvif_events(void)   { return s_config.onvif_events; }
bool        config_get_csi_enabled(void)    { return s_config.csi_enabled; }
float       config_get_csi_threshold(void)  { return s_config.csi_threshold_pct / 100.0f; }
uint8_t     config_get_csi_on_hits(void)    { return s_config.csi_on_hits; }
uint8_t     config_get_csi_off_hits(void)   { return s_config.csi_off_hits; }
uint8_t     config_get_csi_profile(void)    { return s_config.csi_profile; }
bool        config_get_csi_auto_heal(void)  { return s_config.csi_auto_heal; }
bool        config_get_flash_viewers(void)  { return s_config.flash_viewers; }   /* 板级扩展 */

int8_t config_get_cam_brightness(void) { return s_config.cam_brightness; }
int8_t config_get_cam_contrast(void)   { return s_config.cam_contrast; }
int8_t config_get_cam_saturation(void) { return s_config.cam_saturation; }
int8_t config_get_cam_sharpness(void)  { return s_config.cam_sharpness; }
bool   config_get_cam_hmirror(void)    { return s_config.cam_hmirror; }
bool   config_get_cam_vflip(void)      { return s_config.cam_vflip; }
const char *config_get_device_name(void)  { return s_config.device_name; }
const char *config_get_timezone(void)     { return s_config.timezone; }
bool   config_get_allow_ap_fallback(void) { return s_config.allow_ap_fallback; }
uint8_t config_get_xclk_freq_mhz(void)    { return s_config.xclk_freq_mhz; }

cJSON *config_get_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    /* 家族 schema 版本（契约 §1） */
    cJSON_AddNumberToObject(root, "schema_version", CONFIG_SCHEMA_VERSION);

    /* Camera settings */
    cJSON_AddNumberToObject(root, "cam_framesize", s_config.cam_framesize);
    cJSON_AddNumberToObject(root, "cam_fps", s_config.cam_fps);
    cJSON_AddNumberToObject(root, "cam_quality", s_config.cam_quality);
    cJSON_AddNumberToObject(root, "cam_brightness", s_config.cam_brightness);
    cJSON_AddNumberToObject(root, "cam_contrast", s_config.cam_contrast);
    cJSON_AddNumberToObject(root, "cam_saturation", s_config.cam_saturation);
    cJSON_AddNumberToObject(root, "cam_sharpness", s_config.cam_sharpness);
    cJSON_AddBoolToObject(root, "cam_hmirror", s_config.cam_hmirror);
    cJSON_AddBoolToObject(root, "cam_vflip", s_config.cam_vflip);
    cJSON_AddNumberToObject(root, "xclk_freq_mhz", s_config.xclk_freq_mhz);

    /* WiFi */
    cJSON_AddStringToObject(root, "wifi_ssid", s_config.wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_pass",
                            s_config.wifi_pass[0] ? "****" : "");
    cJSON_AddStringToObject(root, "wifi_ssid_2", s_config.wifi_ssid_2);
    cJSON_AddStringToObject(root, "wifi_pass_2",
                            s_config.wifi_pass_2[0] ? "****" : "");

    /* Device / locale（契约 §3.1） */
    cJSON_AddStringToObject(root, "device_name", s_config.device_name);
    cJSON_AddStringToObject(root, "timezone", s_config.timezone);
    cJSON_AddBoolToObject(root, "allow_ap_fallback", s_config.allow_ap_fallback);

    /* AI（JSON 名随契约 §3.2 收敛为 ai_*_en） */
    cJSON_AddBoolToObject(root, "ai_face_en", s_config.ai_face_enable);
    cJSON_AddBoolToObject(root, "ai_motion_en", s_config.ai_motion_enable);
    cJSON_AddBoolToObject(root, "ai_qr_en", s_config.ai_qr_enable);

    /* ONVIF（契约 v2.0：RTSP 凭据键已随密码体系移除） */
    cJSON_AddBoolToObject(root, "onvif_enable", s_config.onvif_enable);

    return root;
}
