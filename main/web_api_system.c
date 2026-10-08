/*
 * MiBee Cam v0.1 — Web server (system API)
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GET /api/status · /api/capabilities · /api/scan ·
 * /metrics, POST /api/reset · /api/reboot · /api/csi/calibrate · /api/time.
 *
 * Split from the monolithic web_server.c (issue #19, pure code motion):
 * route table + lifecycle stay in web_server.c; handler bodies live in
 * per-domain modules. Public API: web_server.h; internal seam:
 * web_server_internal.h. Behavior contract: docs/api-contract.md.
 */

#include "web_server.h"
#include "web_server_internal.h"
#include "watchdog.h"
#include "mjpeg_streamer.h"
#include "ota_updater.h"     /* FW_VERSION（#ifndef 兜底定义，单一来源） */
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "ai_pipeline.h"
#include "camera_driver.h"
#include "flash_viewers.h"   /* 板级扩展：观看者驱动闪光灯 */
#include "csi_motion.h"      /* 契约 v1.6：/api/status 的 csi 快照字段（编译关闭时恒缺省） */
#include "wifi_channel_health.h"  /* 契约 v1.7 ①b：信道健康快照 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "web_server";

/* ------------------------------------------------------------------ */
/*  GET /status                                                        */
/* ------------------------------------------------------------------ */

esp_err_t api_status_handler(httpd_req_t *req)
{
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        return json_error(req, "Out of memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    /* Device + WiFi (契约 v1.0 字段) */
    const char *device_name = config_get_device_name();
    cJSON_AddStringToObject(data, "device_name",
        (device_name && device_name[0]) ? device_name : "MiBeeCam");
    cJSON_AddStringToObject(data, "wifi_ssid", config_get_wifi_ssid());
    cJSON_AddStringToObject(data, "wifi_state",
        wifi_manager_is_connected() ? "connected" : "disconnected");
    cJSON_AddStringToObject(data, "wifi_net", wifi_manager_active_net());
    cJSON_AddStringToObject(data, "current_ssid", wifi_manager_current_ssid());
    cJSON_AddStringToObject(data, "ip", wifi_manager_get_ip());
    /* 2026-09-04 API 对齐：SPA 信号芯片与 WiFi 页当前连接行依赖
     * wifi_rssi/wifi_channel（三姐妹板均已下发，本板此前缺失 → 无信号显示） */
    cJSON_AddNumberToObject(data, "wifi_rssi", wifi_manager_get_rssi());
    cJSON_AddNumberToObject(data, "wifi_channel", wifi_manager_get_channel());
    /* 2026-10-08 增补（加性字段，消费者可忽略）：连接兼容性阶梯档位 */
    cJSON_AddStringToObject(data, "wifi_compat_profile",
        wifi_manager_compat_profile());

    /* Camera — 传感器型号 + 当前分辨率（细节在 /api/camera） */
    cJSON_AddStringToObject(data, "camera", camera_sensor_name());
    cJSON_AddStringToObject(data, "resolution",
        camera_framesize_name(config_get_cam_framesize()));

    /* AI status */
    cJSON *ai = cJSON_CreateObject();
    if (ai) {
        cJSON_AddBoolToObject(ai, "face",   ai_is_enabled(AI_FEATURE_FACE_DETECT));
        cJSON_AddBoolToObject(ai, "motion", ai_is_enabled(AI_FEATURE_MOTION_DETECT));
        cJSON_AddBoolToObject(ai, "qr",     ai_is_enabled(AI_FEATURE_QR_DECODE));
        cJSON_AddItemToObject(data, "ai_status", ai);
    }

    /* System */
    cJSON_AddStringToObject(data, "firmware_version", FW_VERSION);
    watchdog_attach_status(data);
    cJSON_AddNumberToObject(data, "free_heap",
        (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(data, "min_heap",
        (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(data, "free_psram",
        (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    /* S3 片内温度（对齐 seeed/luatos；经典 ESP32 无此传感器故不提供） */
    cJSON_AddNumberToObject(data, "chip_temp", (double)chip_temp_read());
    cJSON_AddNumberToObject(data, "stream_clients",
        mjpeg_stream_client_count());
    cJSON_AddNumberToObject(data, "stream_clients_max", 2);

    /* Viewer-driven flash LED watcher (板级扩展 flash_viewers) */
    {
        cJSON *fv = cJSON_CreateObject();
        if (fv) {
            cJSON_AddNumberToObject(fv, "enabled", (double)config_get_flash_viewers());
            cJSON_AddNumberToObject(fv, "active", (double)flash_viewers_active());
            cJSON_AddItemToObject(data, "flash_viewers", fv);
        }
    }
    cJSON_AddNumberToObject(data, "uptime",
        (double)(esp_timer_get_time() / 1000000));

    /* CSI 实时快照（契约 v1.6，与 /ws csi_status 心跳同形同值；本板无 WS
     * 服务，SPA 胶囊/统计片由此字段驱动。CSI 编译关闭时恒缺省） */
    csi_motion_status_t csi;
    if (csi_motion_get_status(&csi)) {
        cJSON *csi_obj = cJSON_CreateObject();
        if (csi_obj) {
            cJSON_AddStringToObject(csi_obj, "state", csi.state);
            cJSON_AddNumberToObject(csi_obj, "score", (double)csi.score);
            cJSON_AddNumberToObject(csi_obj, "thr", (double)csi.thr);
            /* 契约 v1.7 增补：调参/自愈可观测面 */
            cJSON_AddNumberToObject(csi_obj, "profile", (double)csi.profile);
            cJSON_AddBoolToObject(csi_obj, "thr_locked", csi.thr_locked);
            cJSON_AddBoolToObject(csi_obj, "calibrating", csi.calibrating);
            cJSON_AddNumberToObject(csi_obj, "flip_rate", (double)csi.flip_rate);
            cJSON_AddNumberToObject(csi_obj, "tx_pps", (double)csi.tx_pps);
            cJSON_AddNumberToObject(csi_obj, "cb_pps", (double)csi.cb_pps);
            cJSON_AddNumberToObject(csi_obj, "adm_pps", (double)csi.adm_pps);
            cJSON_AddItemToObject(data, "csi", csi_obj);
        }
    }
    /* 契约 v1.7 ①b：Wi-Fi 信道健康快照（CSI 无关，全家族字段一致） */
    wifi_chan_health_t ch;
    if (wifi_channel_health_get(&ch)) {
        cJSON *ch_obj = cJSON_CreateObject();
        if (ch_obj) {
            cJSON_AddNumberToObject(ch_obj, "rssi_avg", (double)ch.rssi_avg);
            cJSON_AddNumberToObject(ch_obj, "rssi_min", (double)ch.rssi_min);
            cJSON_AddNumberToObject(ch_obj, "channel", (double)ch.channel);
            cJSON_AddNumberToObject(ch_obj, "disconnects_1h", (double)ch.disconnects_1h);
            cJSON_AddNumberToObject(ch_obj, "scan_ts", (double)ch.scan_ts);
            cJSON_AddNumberToObject(ch_obj, "bss_on_chan", (double)ch.bss_on_chan);
            cJSON_AddNumberToObject(ch_obj, "bss_total", (double)ch.bss_total);
            cJSON_AddNumberToObject(ch_obj, "busy_score", (double)ch.busy_score);
            cJSON_AddNumberToObject(ch_obj, "csi_adm_pps", (double)ch.csi_adm_pps);
            cJSON_AddNumberToObject(ch_obj, "csi_cb_ratio", (double)ch.csi_cb_ratio);
            cJSON_AddItemToObject(data, "chan_health", ch_obj);
        }
    }

    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  GET /api/capabilities                                              */
/* ------------------------------------------------------------------ */

esp_err_t api_capabilities_handler(httpd_req_t *req)
{
    cJSON *data = cJSON_CreateObject();
    if (!data) {
        return json_error(req, "Out of memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    
    /* 契约 v1.0：12 个布尔能力位 + api_version/wifi_scan（见 docs/api-contract.md） */
    cJSON_AddStringToObject(data, "api_version", "1.10");
    cJSON_AddBoolToObject(data, "wifi_scan", true);
    cJSON_AddBoolToObject(data, "ai",        true);   /* Has AI pipeline */
    cJSON_AddBoolToObject(data, "sd",        false);  /* No SD card */
    cJSON_AddBoolToObject(data, "audio",     false);  /* No audio */
    cJSON_AddBoolToObject(data, "ota",       true);   /* OTA web endpoints (v1.1 移植 seeed ota_updater) */
    cJSON_AddBoolToObject(data, "mic",       false);  /* No mic */
    cJSON_AddBoolToObject(data, "flash_led", true);   /* Has flash LED */
    cJSON_AddBoolToObject(data, "recording", false);  /* No recording */
    cJSON_AddBoolToObject(data, "timelapse", false);  /* No timelapse */
    cJSON_AddBoolToObject(data, "onvif",     true);   /* Has ONVIF */
    cJSON_AddBoolToObject(data, "rtsp",      true);   /* Has RTSP */
    cJSON_AddBoolToObject(data, "websocket", false);  /* No WebSocket */
    cJSON_AddBoolToObject(data, "mdns",      true);   /* Has mDNS */
    

#if CONFIG_MIBEE_CSI_MOTION
    /* 契约 v1.4：WiFi CSI 运动感知（编译期门控，恒定；true ⇒ /ws csi_status 心跳） */
    cJSON_AddBoolToObject(data, "csi_motion", true);
    cJSON_AddBoolToObject(data, "onvif_events", true);   /* 契约 v1.5：ONVIF MotionAlarm 事件服务 */
#endif
    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  GET /api/scan — WiFi 扫描（阻塞式，按 RSSI 降序）                  */
/* ------------------------------------------------------------------ */

esp_err_t api_scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        return json_error(req, "Scan failed (STA not ready?)", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;

    wifi_ap_record_t *recs = malloc(sizeof(wifi_ap_record_t) * (n ? n : 1));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return json_error(req, "Out of memory", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    esp_wifi_scan_get_ap_records(&n, recs);

    /* 按 RSSI 降序（简单插入排序，n≤20） */
    for (int i = 1; i < (int)n; i++) {
        wifi_ap_record_t key = recs[i];
        int j = i - 1;
        while (j >= 0 && recs[j].rssi < key.rssi) {
            recs[j + 1] = recs[j];
            j--;
        }
        recs[j + 1] = key;
    }

    cJSON *data = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < (int)n; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", (const char *)recs[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", recs[i].rssi);
        cJSON_AddNumberToObject(ap, "auth", recs[i].authmode);
        cJSON_AddItemToArray(arr, ap);
    }
    free(recs);
    cJSON_AddItemToObject(data, "networks", arr);
    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  POST /api/reset · POST /api/reboot                                  */
/* ------------------------------------------------------------------ */

esp_err_t api_reset_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Factory reset requested via web API");
    config_reset();

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "message", "Rebooting...");
    json_ok(req, data);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  /* not reached */
}

esp_err_t api_reboot_handler(httpd_req_t *req)
{
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "message", "Rebooting...");
    json_ok(req, data);
    ESP_LOGW(TAG, "Reboot requested via web API");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  /* not reached */
}

/** @brief 触发 CSI 立即重校准（契约 v1.7；write auth。CSI-off 板 404，
 *  运行时未就绪 503），背景执行，进度经 csi.calibrating / 串口日志。 */
esp_err_t api_csi_calibrate_handler(httpd_req_t *req)
{
    esp_err_t cr = csi_motion_recalibrate();
    if (cr == ESP_ERR_NOT_SUPPORTED) {
        return json_error(req, "CSI sensing not built (csi_motion capability absent)",
                          HTTPD_404_NOT_FOUND);
    }
    if (cr != ESP_OK) {
        return json_error(req, "CSI runtime not ready (no Wi-Fi link?)", 503);
    }
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "message", "CSI recalibration started");
    return json_ok(req, data);
}

/* ------------------------------------------------------------------ */
/*  POST /api/time — 手动设置系统时间                                   */
/* ------------------------------------------------------------------ */

esp_err_t api_time_handler(httpd_req_t *req)
{
    char *body = read_body(req, 256);
    if (!body) return json_error(req, "Empty body", HTTPD_400_BAD_REQUEST);
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) return json_error(req, "Invalid JSON", HTTPD_400_BAD_REQUEST);

    cJSON *jy = cJSON_GetObjectItem(json, "year");
    cJSON *jmo = cJSON_GetObjectItem(json, "month");
    cJSON *jd = cJSON_GetObjectItem(json, "day");
    cJSON *jh = cJSON_GetObjectItem(json, "hour");
    cJSON *jmi = cJSON_GetObjectItem(json, "min");
    cJSON *js = cJSON_GetObjectItem(json, "sec");

    if (!cJSON_IsNumber(jy) || !cJSON_IsNumber(jmo) || !cJSON_IsNumber(jd) ||
        !cJSON_IsNumber(jh) || !cJSON_IsNumber(jmi) || !cJSON_IsNumber(js)) {
        cJSON_Delete(json);
        return json_error(req, "Missing time fields", HTTPD_400_BAD_REQUEST);
    }

    struct tm tm_now = {
        .tm_year = jy->valueint - 1900,
        .tm_mon = jmo->valueint - 1,
        .tm_mday = jd->valueint,
        .tm_hour = jh->valueint,
        .tm_min = jmi->valueint,
        .tm_sec = js->valueint,
    };
    time_t epoch = mktime(&tm_now);
    cJSON_Delete(json);

    if (epoch < (time_t)1577836800) {  /* < 2020-01-01: 非法日期 */
        return json_error(req, "Invalid date", HTTPD_400_BAD_REQUEST);
    }
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    return json_ok(req, NULL);
}

/* ------------------------------------------------------------------ */
/*  GET /metrics — Prometheus 最小指标                                  */
/* ------------------------------------------------------------------ */

esp_err_t metrics_handler(httpd_req_t *req)
{
    char buf[1024];
    int rssi = 0;
    wifi_ap_record_t ap;
    if (wifi_manager_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }
    int len = snprintf(buf, sizeof(buf),
        "# HELP esp_free_heap_bytes Free heap memory\n"
        "# TYPE esp_free_heap_bytes gauge\n"
        "esp_free_heap_bytes %lu\n"
        "# HELP esp_free_psram_bytes Free PSRAM memory\n"
        "# TYPE esp_free_psram_bytes gauge\n"
        "esp_free_psram_bytes %lu\n"
        "# HELP esp_min_free_heap_bytes Minimum free heap bytes since boot\n"
        "# TYPE esp_min_free_heap_bytes gauge\n"
        "esp_min_free_heap_bytes %lu\n"
        "# HELP esp_uptime_seconds System uptime in seconds\n"
        "# TYPE esp_uptime_seconds gauge\n"
        "esp_uptime_seconds %lu\n"
        "# HELP esp_wifi_rssi_dbm WiFi RSSI\n"
        "# TYPE esp_wifi_rssi_dbm gauge\n"
        "esp_wifi_rssi_dbm %d\n"
        "# HELP esp_mjpeg_clients MJPEG stream clients\n"
        "# TYPE esp_mjpeg_clients gauge\n"
        "esp_mjpeg_clients %d\n",
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned long)esp_get_minimum_free_heap_size(),
        (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000),
        rssi,
        mjpeg_stream_client_count());

    set_cors_headers(req);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}
