/*
 * MiBee Cam v0.1 — ONVIF 板级适配层（onvif-c 组件接缝）
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 唯一的板级差异面：把固件的 wifi/config/rtsp/device_id 符号接进
 * onvif_c_config_t 回调（组件本身零板级 include）。启动步骤编号不变
 * （main.c Step 7 调 onvif_port_start）。
 */

#include "onvif_port.h"
#include "onvif_c.h"
#include "wifi_manager.h"
#include "config_manager.h"
#include "rtsp_server.h"
#include "device_id.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "web_server.h"
#include "ota_updater.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "time_sync.h"

static const char *TAG = "onvif_port";

static const char *port_serial(void)
{
    return device_get_serial();
}

static const char *port_uuid(void)
{
    return device_get_uuid();
}

static const char *port_ip(void)
{
    return wifi_manager_get_ip();
}

static const char *port_stream_uri(void)
{
    const char *url = rtsp_get_url();
    return url ? url : "rtsp://0.0.0.0:554/stream";
}

static uint8_t port_frame_rate(void)
{
    return config_get_cam_fps();   /* 契约 §3.1 cam_fps 消费者 */
}

static bool port_events_enabled(void)
{
    return config_get_onvif_events();   /* 契约 v1.5：MotionAlarm 生成开关 */
}

/* ---- 时间配置接缝（issue #43：apply-or-fault，绝不静默） ---- */

static bool port_time_ntp_active(void)
{
    return time_is_synced();   /* 如实：SNTP 真同步过才报 NTP */
}

static const char *port_time_tz(void)
{
    return config_get_timezone();   /* 空 = 组件报 UTC */
}

static esp_err_t port_time_apply(const onvif_c_time_req_t *req)
{
    if (req->has_tz) {
        esp_err_t err = config_set("timezone", req->tz);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "timezone persist failed: %s", esp_err_to_name(err));
            return err;
        }
        setenv("TZ", req->tz, 1);
        tzset();
    }
    if (req->ntp_mode) {
        /* 模式切回 NTP：按当前配置源重启 SNTP，时间由同步落地 */
        return time_sync_restart();
    }
    if (req->has_utc) {
        struct timeval tv = { .tv_sec = (time_t)req->utc_epoch, .tv_usec = 0 };
        if (settimeofday(&tv, NULL) != 0) {
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "manual time set to %lld", (long long)req->utc_epoch);
    }
    return ESP_OK;
}

static esp_err_t port_ntp_set(const char *const *servers, size_t n)
{
    esp_err_t e1 = config_set("ntp_server1", n > 0 ? servers[0] : "");
    esp_err_t e2 = config_set("ntp_server2", n > 1 ? servers[1] : "");
    if (e1 != ESP_OK) return e1;
    if (e2 != ESP_OK) return e2;
    ESP_LOGI(TAG, "NTP servers set (%u) — restarting SNTP", (unsigned)n);
    return time_sync_restart();
}

esp_err_t onvif_port_start(void)
{
    /* 运行时开关（原 onvif_start 的 config 门） */
    if (!config_get_onvif_enable()) {
        ESP_LOGI(TAG, "ONVIF disabled by config");
        return ESP_OK;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    static char hostname[32];   /* cfg 浅拷贝持有指针，须与 cfg 同寿 */
    snprintf(hostname, sizeof(hostname), "mibeecam-%02x%02x",
             mac[4], mac[5]);

    const onvif_c_config_t cfg = {
        .manufacturer     = "MiBee",
        .model            = "MiBeeCam",
        .hardware_id      = "ESP32-S3-N16R8",
        .firmware_version = FW_VERSION,   /* 唯一版本源：ota_updater.h */
        .serial           = port_serial,
        .uuid             = port_uuid,
        .ip               = port_ip,
        .stream_uri       = port_stream_uri,
        .frame_rate       = port_frame_rate,
        .events_enabled   = port_events_enabled,
        .time_ntp_active  = port_time_ntp_active,
        .time_tz          = port_time_tz,
        .time_apply       = port_time_apply,
        .ntp_set          = port_ntp_set,
        .http_port        = 80,
        .wdt_watch_discovery = true,
        .mdns_hostname    = hostname,
        .mdns_instance    = "MiBee Cam",
    };

    httpd_handle_t server = web_server_get_handle();
    if (!server) {
        ESP_LOGW(TAG, "Web server not available, ONVIF skipped");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = onvif_c_start(server, &cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ONVIF services started (onvif-c component)");
    }
    return err;
}
