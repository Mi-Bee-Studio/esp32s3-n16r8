/*
 * MiBee Cam v0.1 — Main application entry point
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Boot sequence:
 *   1. NVS flash
 *   2. SPIFFS (Web UI assets)
 *   3. WiFi (AP or STA)
 *   4. Camera (OV3660)
 *   5. Frame broadcaster + Web server + MJPEG streamer
 *   6. RTSP server (MJPEG-only, digest auth)
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#if CONFIG_HEAP_TASK_TRACKING
/* 内部堆地板诊断（issue #11 遗留专项）：生产构建（sdkconfig.defaults）不开
 * 此选项，仅诊断构建在本地 gitignored sdkconfig 里开启，代码零开销保留。 */
#include "esp_heap_task_info.h"
#endif
#include "driver/gpio.h"
#include "camera_driver.h"
#include "config_manager.h"
#include "watchdog.h"
#include "wifi_manager.h"
#include "time_sync.h"
#include "csi_motion.h"
#include "wifi_channel_health.h"
#include "web_server.h"
#include "mjpeg_streamer.h"
#include "flash_viewers.h"
#include "rtsp_server.h"
#include "frame_broadcaster.h"
#include "ai_pipeline.h"
#include "onvif_port.h"
#include "at_command.h"
#include "ota_updater.h"
#include "flash_led.h"
#include "status_led.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "main";

/* httpd :80 self-heal probe — sends a real HTTP request to localhost:80.
 * TCP connect alone is insufficient: LWIP accepts connections even when
 * httpd has no free worker. Only a real request proves the event loop is alive.
 *
 * 2026-09-04 自愈误杀修复（PIT-002 家族教训，方案同 seeed/ai-thinker）：
 * NVR 多路订阅 + 弱链路时 lwIP 池被 TIME_WAIT 挤占，探针自己 socket()/
 * connect() 拿不到资源（EMFILE/ENOBUFS，串口同时可见 httpd accept(23)），
 * 旧实现把它计为"httpd 死"→ 2/2 → esp_restart —— 资源紧张被翻译成重启，
 * 实测 2-7 分钟循环（rst:0xc，无 panic）。修复：资源类失败一律不计数
 * （池子 15s MSL 后自行恢复）；只有"TCP 连上但应用层无响应"才判疑似卡死。 */
/* 探针失败时的占用者转储（ai#31 家族推广，2026-10-03）：socket 表被静默
 * 存量会话卡满时（PIT-052 型），探针只报"不响应"——谁占着不可见，只能等
 * 自愈重启。逐 fd 打对端 + mjpeg 客户端数/RSSI/内部堆，一次定位卡表占用
 * 者。getpeername 双栈陷阱（IPv4 对端以 v4-mapped 返回）同 web_server_static.c
 * 的 PIT-056 注释：sockaddr_storage 判族。 */
static void log_socket_holders(void)
{
    httpd_handle_t h = web_server_get_handle();
    if (!h) {
        ESP_LOGW(TAG, "diag: httpd handle NULL");
        return;
    }
    int fds[16];
    size_t count = sizeof(fds) / sizeof(fds[0]);
    if (httpd_get_client_list(h, &count, fds) != ESP_OK) {
        ESP_LOGW(TAG, "diag: httpd_get_client_list failed");
        return;
    }
    ESP_LOGW(TAG, "diag: httpd sessions=%u mjpeg_clients=%d rssi=%d int_free=%u",
             (unsigned)count, mjpeg_stream_client_count(),
             wifi_manager_get_rssi(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    for (size_t i = 0; i < count; i++) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
        char peer[INET6_ADDRSTRLEN] = "?";
        int port = 0;
        if (lwip_getpeername(fds[i], (struct sockaddr *)&ss, &sl) == 0) {
            const void *addr = NULL;
            int fam = ss.ss_family;
            if (fam == AF_INET) {
                addr = &((struct sockaddr_in *)&ss)->sin_addr;
                port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
            } else if (fam == AF_INET6) {
                addr = &((struct sockaddr_in6 *)&ss)->sin6_addr;
                port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
            }
            char abuf[INET6_ADDRSTRLEN];
            if (addr && inet_ntop(fam, addr, abuf, sizeof(abuf)) != NULL) {
                const char *p = abuf;
                if (strncasecmp(p, "::ffff:", 7) == 0) {
                    p += 7;   /* IPv4-mapped → 报 IPv4 文本 */
                }
                strlcpy(peer, p, sizeof(peer));
            }
        }
        ESP_LOGW(TAG, "diag: fd=%d peer=%s:%d", fds[i], peer, port);
    }
}

static bool probe_httpd_port80(void)
{    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ESP_LOGW(TAG, "httpd probe: no socket available (errno=%d) — not counted", errno);
        return true;
    }

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(80),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };

    bool ok = false;
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) == 0) {
        static const char req[] =
            "GET /api/status HTTP/1.0\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: close\r\n\r\n";
        if (send(sock, req, sizeof(req) - 1, 0) > 0) {
            char buf[32];
            int n = recv(sock, buf, sizeof(buf), 0);
            ok = (n > 0);
        }
    } else {
        /* 本机回环 connect 失败：EMFILE/ENOBUFS 属资源紧张，不计数 */
        ESP_LOGW(TAG, "httpd probe: connect failed (errno=%d) — not counted", errno);
        ok = true;
    }
    close(sock);
    return ok;
}


/* ------------------------------------------------------------------ */
/*  SPIFFS mount for Web UI static assets                              */
/* ------------------------------------------------------------------ */

static void init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = NULL,
        .max_files              = 8,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SPIFFS mounted at /spiffs");
    }
}

/* ------------------------------------------------------------------ */
/*  app_main                                                          */
/* ------------------------------------------------------------------ */

#if CONFIG_HEAP_TASK_TRACKING
/* 内部堆地板诊断（issue #11 遗留）：打区域总量 + 每任务堆用量全景。
 * 依赖 CONFIG_HEAP_TRACK_DELETED_TASKS 才能看到已删除任务的遗留分配。 */
static void heap_diag_dump(int seq)
{
    ESP_LOGI(TAG, "=== HEAP DIAG #%d internal=%lu psram=%lu min_free_int=%lu ===",
             seq,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    heap_caps_print_all_task_stat_overview(NULL);
    fflush(stdout);
}
#else
static void heap_diag_dump(int seq) { (void)seq; }
#endif

void app_main(void)
{
    watchdog_init();
    ESP_LOGI(TAG, "MiBee Cam v0.1 starting...");
    ESP_LOGI(TAG, "Free heap: %lu  Free PSRAM: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* ---- 1. NVS flash ------------------------------------------------ */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition corrupt, erasing...");
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS initialized");
    } else {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
    }
    config_load();

    /* 时区（契约 §3.1）：非空即在启动时应用——SNTP（issue #7）与
     * /api/time 手动设时共用此 TZ（POST /api/config 改 timezone 时
     * 立即重设，同 seeed/ai-thinker；time_sync_init 内亦会重放） */
    {
        const char *tz = config_get_timezone();
        if (tz && tz[0]) {
            setenv("TZ", tz, 1);
            tzset();
            ESP_LOGI(TAG, "Timezone applied: %s", tz);
        }
    }

    /* ---- 2. SPIFFS for Web UI ---------------------------------------- */
    init_spiffs();

    /* ---- 3. WiFi (AP or STA) ------------------------------------------- */
    {
        esp_err_t wf_err = wifi_manager_init();
        if (wf_err == ESP_OK) {
            ESP_LOGI(TAG, "WiFi manager started");
        } else {
            ESP_LOGE(TAG, "WiFi manager init failed: %s", esp_err_to_name(wf_err));
        }
    }

    /* ---- 3a. ESPectre CSI motion sensing (optional, after WiFi) ------- */
    csi_motion_init();
    wifi_channel_health_init();   /* 契约 v1.7 ①b：信道健康感知（CSI 无关，四仓共享） */

    /* ---- 3b. SNTP time sync (issue #7：ONVIF 事件时戳 1970 纪元修复) -- */
    /* WiFi 已连（重启到已保存网络时会很快）即同步；未连由主循环每 60s
     * 重试。ONVIF 事件契约 §13 容忍未同步（照发，时戳为纪元）。 */
    if (wifi_manager_is_connected()) {
        time_sync_init();
    }

    /* ---- 4. Camera init + LED 子系统初始化 ------------------------- */
    {
        esp_err_t cam_err = camera_init();
        if (cam_err == ESP_OK) {
            /* Snapshot one frame to prove the pipeline works */
            camera_fb_t *fb = camera_capture();
            if (fb) {
                ESP_LOGI(TAG, "First frame OK: %ux%u  %u bytes",
                         fb->width, fb->height, (unsigned)fb->len);
                esp_camera_fb_return(fb);
            }
        } else {
            ESP_LOGE(TAG, "Camera init failed, skipping frame capture");
        }

        /* 2026-09-11（issue #11 §四）：flash_led/status_led 自基线起就没有
         * init 调用——AT+LED / POST /api/led 恒 INVALID_STATE、状态灯从未
         * 点亮。此处补上初始化。引脚说明：flash LED 取 GPIO2（社区板型资料
         * 主流说法，也是原开机探测候选之一；不同批次板 LED 位置不同，点亮
         * 与否以实板为准，运行时配置键方案见 issue 待办）；status LED 维持
         * 模块内默认（WS2812@48，devkit 系参考设计）。原 GPIO 2/3/46 探测
         * 舞蹈移除——每次开机白耗 6s，且裸 gpio_set_level 与 LEDC/RMT 驱动
         * 冲突。 */
        if (flash_led_init(GPIO_NUM_2) != ESP_OK) {
            ESP_LOGW(TAG, "Flash LED init failed — AT+LED/api/led will error");
        }
        if (status_led_init() != ESP_OK) {
            ESP_LOGW(TAG, "Status LED init failed — wifi status colors unavailable");
        }
    }

    /* ---- 4a. SELFTEST 自检行（工作区 AGENTS.md"固件自检行"规范）------
     * 一行可 grep 的开机体检证据：serialtap 按前缀聚合做台架异常发现
     * （板间横向对比：PSRAM 没起来、传感器换了、flash 缩水一眼可见）。
     * wifi 部分由 wifi_manager 在开机快扫后补一行 SELFTEST: wifi ...。 */
    {
        esp_chip_info_t ci;
        esp_chip_info(&ci);
        uint32_t flsz = 0;
        esp_flash_get_size(NULL, &flsz);   /* v6：NULL=默认主 flash */
        size_t psz = esp_psram_get_size();
        sensor_t *s = esp_camera_sensor_get();
        char sensor[32] = "none";
        if (s && s->id.PID) {
            snprintf(sensor, sizeof(sensor), "%.16s/0x%04X",
                     camera_sensor_name(), s->id.PID);
        }
        ESP_LOGI(TAG, "SELFTEST: board=esp32s3-n16r8-cam fw=v0.1 chip=%s rev=v%d.%d"
                      " cores=%u flash=%uMB psram=%uMB sensor=%s heap=%uKB",
                 CONFIG_IDF_TARGET, ci.revision / 100, ci.revision % 100, ci.cores,
                 (unsigned)(flsz >> 20), (unsigned)(psz >> 20), sensor,
                 (unsigned)(esp_get_free_heap_size() >> 10));
    }

    /* ---- 5. Web server + MJPEG streamer ----------------------------- */
    {
        /* Initialize frame broadcaster (required by MJPEG streamer) */
        esp_err_t fb_err = frame_broadcaster_init();
        if (fb_err == ESP_OK) {
            ESP_LOGI(TAG, "Frame broadcaster initialized");
        } else {
            ESP_LOGE(TAG, "Frame broadcaster init failed: %s", esp_err_to_name(fb_err));
        }

        esp_err_t mjpeg_err = mjpeg_stream_init();
        if (mjpeg_err == ESP_OK) {
            ESP_LOGI(TAG, "MJPEG streamer initialized");
        } else {
            ESP_LOGE(TAG, "MJPEG streamer init failed: %s", esp_err_to_name(mjpeg_err));
        }

        /* Start frame broadcaster (grabs frames from camera in a task) */
        fb_err = frame_broadcaster_start();
        if (fb_err == ESP_OK) {
            ESP_LOGI(TAG, "Frame broadcaster started");
        } else {
            ESP_LOGE(TAG, "Frame broadcaster start failed: %s", esp_err_to_name(fb_err));
        }

        /* ---- AI pipeline (face detect + motion + QR) -------------------- */
        esp_err_t ai_err = ai_init();
        if (ai_err == ESP_OK) {
            ESP_LOGI(TAG, "AI pipeline initialized");
        } else {
            ESP_LOGE(TAG, "AI pipeline init failed: %s", esp_err_to_name(ai_err));
        }

        /* Wire AI config to live pipeline state (override defaults) */
        ai_enable(AI_FEATURE_FACE_DETECT,   config_get_ai_face_enable());
        ai_enable(AI_FEATURE_MOTION_DETECT, config_get_ai_motion_enable());
        ai_enable(AI_FEATURE_QR_DECODE,     config_get_ai_qr_enable());
        esp_err_t ota_err = ota_updater_init();
        esp_err_t http_err = web_server_start(80);
        if (http_err == ESP_OK) {
            ESP_LOGI(TAG, "Web server running on port 80");
        } else {
            ESP_LOGE(TAG, "Web server start failed: %s", esp_err_to_name(http_err));
        }

        /* ---- MJPEG streamer on port 81 (independent TCP server) ------- */
        esp_err_t mjpeg_srv_err = mjpeg_stream_server_start(81);
        if (mjpeg_srv_err == ESP_OK) {
            ESP_LOGI(TAG, "MJPEG streamer running on port 81");
        } else {
            ESP_LOGE(TAG, "MJPEG streamer start failed: %s", esp_err_to_name(mjpeg_srv_err));
        }

        /* Viewer-driven flash LED watcher (板级扩展 flash_viewers，默认关) */
        flash_viewers_start();
    }

    /* ---- 6. RTSP server (MJPEG-only, digest auth) --------------------- */
    {
        esp_err_t rtsp_err = rtsp_start();
        if (rtsp_err == ESP_OK) {
            ESP_LOGI(TAG, "RTSP server started: %s", rtsp_get_url());
        } else {
            ESP_LOGE(TAG, "RTSP server start failed: %s", esp_err_to_name(rtsp_err));
        }

    /* ---- 7. ONVIF discovery + SOAP service --------------------------- */
    {
        esp_err_t onvif_err = onvif_port_start();
        if (onvif_err == ESP_OK) {
            ESP_LOGI(TAG, "ONVIF service started (WS-Discovery + mDNS)");
        } else {
            ESP_LOGI(TAG, "ONVIF service skipped: %s", esp_err_to_name(onvif_err));
        }
    }
    }

    /* ---- Done -------------------------------------------------------- */
    ESP_LOGI(TAG, "MiBee Cam v0.1 scaffold initialized successfully");
    ESP_LOGI(TAG, "System info: chip=%s cores=%d psram=%luMB",
             CONFIG_IDF_TARGET,
             portNUM_PROCESSORS,
             (unsigned long)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / (1024 * 1024)));

    /* ---- 8. AT command listener (UART0) --------------------------- */
    at_command_init();

    /* Idle loop */
    heap_diag_dump(0);   /* 启动完成即取全景基线（WiFi/httpd/mjpeg 已建） */
    while (1) {
        /* SNTP 重试（issue #7）：开机未连网时 3b 跳过，这里每 60s 补；
         * 已同步后 time_sync_init 幂等直接返回。 */
        if (wifi_manager_is_connected() && !time_is_synced()) {
            time_sync_init();
        }

        /* httpd :80 self-heal: probe every 60s cycle.
         * 2 consecutive failures (120s unresponsive) → reboot.
         * WiFi 未连接时不计数（ai-thinker 2026-09-03 同款：掉线≠httpd 死）。
         * OTA 长事务窗口内整体跳过（PIT-058：esp_ota_write 拖慢事件循环
         * 属预期，2026-10-02 .113 两连杀后结构性修复；窗口 10min 封顶自动失效）。 */
        static int httpd_stuck_count = 0;
        if (ota_updater_busy()) {
            ESP_LOGD(TAG, "httpd probe skipped: OTA transaction grace window");
        } else if (!probe_httpd_port80()) {
            if (!wifi_manager_is_connected()) {
                httpd_stuck_count = 0;
                ESP_LOGW(TAG, "httpd probe failed but WiFi down — not counting");
            } else {
                httpd_stuck_count++;
                ESP_LOGW(TAG, "httpd :80 probe failed (%d/2)", httpd_stuck_count);
                if (httpd_stuck_count == 1) {
                    /* ai#31 推广：趁还没重启，把卡表占用者打出来 */
                    log_socket_holders();
                }
                if (httpd_stuck_count >= 2) {
                    ESP_LOGE(TAG, "httpd :80 unresponsive for 120s — rebooting");
                    esp_restart();
                }
            }
        } else {
            httpd_stuck_count = 0;
        }

        /* 堆诊断：第 1/2/3 分钟各打一次（覆盖地板形成期），之后每 30 分钟一次 */
        {
            static int diag_cycle = 0;
            if (diag_cycle >= 1 && diag_cycle <= 3) heap_diag_dump(diag_cycle);
            else if (diag_cycle > 3 && (diag_cycle % 30) == 0) heap_diag_dump(diag_cycle);
            diag_cycle++;
        }

        vTaskDelay(pdMS_TO_TICKS(60000));
        ESP_LOGD(TAG, "Heartbeat: heap=%lu PSRAM=%lu",
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
