/*
 * MiBee Cam v0.1 — RTSP server (MJPEG-only, digest auth)
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Single MJPEG video track over RTP/UDP (or TCP-interleaved with espp/rtsp
 * patches from patches/espp__rtsp/).
 *
 * No authentication (contract v1.9): device-level passwords removed
 * family-wide; the RTSP stream is open on the trusted LAN.
 *
 * espp/rtsp v1.1.3 native API:
 *   espp::RtspServer, espp::MjpegPacketizer, add_track
 *
 * NO audio track — the board has no microphone.
 */

#include "rtsp_server.h"

#include "esp_log.h"
#include "frame_broadcaster.h"
#include "config_manager.h"
#include "wifi_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* espp/rtsp C++ headers */
#include "rtsp_server.hpp"
#include "mjpeg_packetizer.hpp"

#include <memory>
#include <string>
#include <cstdio>

static const char *TAG = "rtsp";

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define RTSP_PORT           554
#define RTSP_PATH           "/stream"
#define VIDEO_TASK_PRIORITY 2
#define VIDEO_TASK_STACK    4096

/* ------------------------------------------------------------------ */
/*  Static state                                                       */
/* ------------------------------------------------------------------ */

static std::unique_ptr<espp::RtspServer> s_server;
static TaskHandle_t s_video_task = NULL;
static volatile bool s_running = false;

/* ------------------------------------------------------------------ */
/*  Video feed task — pulls JPEG frames from frame_broadcaster         */
/* ------------------------------------------------------------------ */

extern "C" void rtsp_video_feed_task(void *arg)
{
    (void)arg;

    frame_sub_t *vsub = frame_broadcaster_subscribe(FRAMESUB_RTSP);
    if (!vsub) {
        ESP_LOGE(TAG, "Failed to subscribe to frame broadcaster");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "RTSP video feed task started");

    /* Try to send the most recent frame immediately */
    frame_msg_t msg;
    uint32_t last_seq = 0;
    if (frame_broadcaster_get_frame(vsub, &msg)) {
        last_seq = msg.seq;
        s_server->send_frame(0, std::span<const uint8_t>(msg.data, msg.len));
        frame_broadcaster_release(&msg);
    }

    while (s_running) {
        frame_msg_t msg;
        if (frame_broadcaster_get_frame(vsub, &msg)) {
            /* 只发新帧（seq 前进才发）：get_frame 无条件返回缓存帧，而背压
             * 窗口内 send_frame 快速返回（session 侧丢弃+冷却），成功路径
             * 若无节流会退化成 p2 热自旋，饿死同核低优先级任务（TWDT 首捕：
             * chan_health 停喂 10s，2026-10-03 二号机；PIT-057 同族）。
             * 正常流上限仍 ≥ 帧率（帧周期 61ms >> 10ms 让步）。 */
            bool is_new = (msg.seq != last_seq);
            last_seq = msg.seq;
            if (is_new) {
                s_server->send_frame(0, std::span<const uint8_t>(msg.data, msg.len));
            }
            frame_broadcaster_release(&msg);
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            /* No frame available yet — yield briefly before retry */
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    frame_broadcaster_unsubscribe(vsub);
    ESP_LOGI(TAG, "RTSP video feed task exiting");
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Public C API                                                       */
/* ------------------------------------------------------------------ */

esp_err_t rtsp_start(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "RTSP server already running");
        return ESP_OK;
    }

    const char *ip = wifi_manager_get_ip();

    ESP_LOGI(TAG, "Creating RTSP server on %s:%d", ip, RTSP_PORT);

    /* 契约 v1.9：不配置 auth_username/auth_password——espp 会话层空用户名
     * 即免认证（check_auth 直接放行），digest 质询不再发生 */
    s_server = std::make_unique<espp::RtspServer>(espp::RtspServer::Config{
        .server_address = ip,
        .port = RTSP_PORT,
        .path = RTSP_PATH,
        .max_data_size = 1400,
        .log_level = espp::Logger::Verbosity::WARN,
        .accept_task_stack_size_bytes = 4096,
        .session_task_stack_size_bytes = 8192,
        .control_task_stack_size_bytes = 8192,
        .auth_username = "",   /* 空 = 会话层免认证（espp check_auth 直接放行） */
        .auth_password = "",
        .auth_realm = "",
    });

    /* Track 0 — MJPEG video (payload type 26, RFC 2435) */
    auto mjpeg_packer = std::make_shared<espp::MjpegPacketizer>(
        espp::MjpegPacketizer::Config{
            .max_payload_size = 1400,
        });
    s_server->add_track(espp::RtspServer::TrackConfig{
        .track_id = 0,
        .packetizer = mjpeg_packer,
    });

    /* Single MJPEG track only — NO audio track (board has no mic) */

    /* Refresh IP string in case WiFi connected recently */
    ip = wifi_manager_get_ip();
    ESP_LOGI(TAG, "RTSP server initialized: rtsp://%s:%d%s", ip, RTSP_PORT, RTSP_PATH);

    /* Start the RTSP server */
    if (!s_server->start()) {
        ESP_LOGE(TAG, "Failed to start RTSP server");
        s_server.reset();
        return ESP_FAIL;
    }

    s_running = true;

    /* Spawn video feed task on Core 1 */
    BaseType_t created = xTaskCreatePinnedToCore(
        rtsp_video_feed_task,
        "rtsp_video",
        VIDEO_TASK_STACK,
        NULL,
        VIDEO_TASK_PRIORITY,
        &s_video_task,
        1);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create video feed task");
        s_server->stop();
        s_server.reset();
        s_running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "RTSP server started on rtsp://%s:%d%s", ip, RTSP_PORT, RTSP_PATH);
    return ESP_OK;
}

esp_err_t rtsp_stop(void)
{
    s_running = false;

    /* Kill video feed task */
    if (s_video_task) {
        vTaskDelete(s_video_task);
        s_video_task = NULL;
    }

    if (s_server) {
        s_server->stop();
        s_server.reset();
    }

    ESP_LOGI(TAG, "RTSP server stopped");
    return ESP_OK;
}

const char *rtsp_get_url(void)
{
    /* 契约 v1.9：URL 不再内嵌凭据（原实现把明文 user:pass 交给 ONVIF
     * GetStreamUri 的未认证 SOAP 客户端并打进启动日志——随密码移除消灭） */
    static char buf[64];
    snprintf(buf, sizeof(buf), "rtsp://%s:%d%s",
             wifi_manager_get_ip(), RTSP_PORT, RTSP_PATH);
    return buf;
}
