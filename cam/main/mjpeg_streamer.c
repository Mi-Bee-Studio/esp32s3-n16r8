/*
 * MiBee Cam v0.1 — MJPEG streamer
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Serves MJPEG frames via an independent TCP server on port 81.
 * Each connection creates a FreeRTOS task that:
 *   1. Reads HTTP GET /stream request
 *   2. Subscribes to frame_broadcaster (FRAME_SUB_MJPEG)
 *   3. Loops: get_frame -> send() multipart/x-mixed-replace
 *   4. Cleanup: unsubscribe, close socket
 *
 * Max 2 concurrent clients — 3rd gets 503.
 * Frame access is non-blocking via frame_broadcaster_get_frame().
 *
 * Design: Separate TCP server on port 81 bypasses httpd entirely,
 * keeping the main web server responsive even with long-lived streams.
 */
#include "mjpeg_streamer.h"
#include "esp_log.h"
#include "frame_broadcaster.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <errno.h>
#include <netinet/tcp.h>

static const char *TAG = "mjpeg_streamer";

#define MJPEG_BOUNDARY      "frame"
#define MAX_STREAM_CLIENTS  2
#define CHUNK_SIZE          8192
#define LISTEN_BACKLOG      2
#define CLIENT_TASK_STACK   4096
#define SEND_TIMEOUT_MS     15000
#define STREAM_PORT         81
static int s_client_count = 0;

/* 客户端注册表（LRU 踢除用）：浏览器渲染器停滞时 TCP 依然健康，探测抓不到，
 * 槽位会被永久占用 → 满员时强制 shutdown 最旧连接，新连接永远能赢 */
typedef struct {
    int        fd;
    TickType_t since;
} mjpeg_client_slot_t;
static mjpeg_client_slot_t s_clients[MAX_STREAM_CLIENTS];
static SemaphoreHandle_t s_mutex = NULL;
static TaskHandle_t s_listen_task = NULL;
static int s_listen_sock = -1;
static volatile bool s_running = false;

/* Forward declarations */
static void mjpeg_listen_task(void *arg);
static void mjpeg_client_task(void *arg);
/* ------------------------------------------------------------------ */
/*  Client task — serves one MJPEG stream connection                   */
/* ------------------------------------------------------------------ */
static void mjpeg_client_task(void *arg)
{
    int client_sock = (int)(intptr_t)arg;

    /* Set send timeout so a stuck client does not hang the task */
    struct timeval tv = { .tv_sec = SEND_TIMEOUT_MS / 1000,
                          .tv_usec = (SEND_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Disable Nagle's algorithm — lower latency for small MJPEG part-headers */
    int flag = 1;
    setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    /* TCP keepalive：NAT 掉线/设备休眠型僵尸连接 ~30s 内被内核判死 */
    int ka = 1;
    setsockopt(client_sock, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
    int keep_idle = 10, keep_intvl = 5, keep_cnt = 3;
    setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle));
    setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPINTVL, &keep_intvl, sizeof(keep_intvl));
    setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPCNT, &keep_cnt, sizeof(keep_cnt));

    /* Recv timeout — prevents zombie if client connects but never sends HTTP request */
    struct timeval rcvtv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &rcvtv, sizeof(rcvtv));

    /* Read HTTP request (first 511 bytes is enough to validate) */
    char req_buf[512];
    int req_len = recv(client_sock, req_buf, sizeof(req_buf) - 1, 0);
    if (req_len <= 0) {
        ESP_LOGW(TAG, "Failed to read HTTP request (errno %d)", errno);
        close(client_sock);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_client_count--;
        xSemaphoreGive(s_mutex);
        vTaskDelete(NULL);
        return;
    }
    req_buf[req_len] = '\0';

    /* Validate: must be GET /stream (accept /stream?xxx too) */
    if (strncmp(req_buf, "GET /stream", 11) != 0) {
        ESP_LOGW(TAG, "Unexpected request: %.60s", req_buf);
        const char *resp = "HTTP/1.1 400 Bad Request\r\n"
                           "Content-Length: 0\r\n"
                           "Connection: close\r\n\r\n";
        send(client_sock, resp, strlen(resp), 0);
        close(client_sock);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_client_count--;
        xSemaphoreGive(s_mutex);
        vTaskDelete(NULL);
        return;
    }

    /* Send HTTP 200 + multipart/x-mixed-replace headers */
    char headers[512];
    int hdr_len = snprintf(headers, sizeof(headers),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" MJPEG_BOUNDARY "\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n");

    if (send(client_sock, headers, hdr_len, 0) != hdr_len) {
        ESP_LOGW(TAG, "Failed to send response headers");
        close(client_sock);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_client_count--;
        xSemaphoreGive(s_mutex);
        vTaskDelete(NULL);
        return;
    }

    /* Subscribe to frame broadcaster */
    frame_sub_t *sub = frame_broadcaster_subscribe(FRAMESUB_MJPEG);
    if (!sub) {
        ESP_LOGE(TAG, "Failed to subscribe to frame broadcaster");
        const char *err_resp = "HTTP/1.1 503 Service Unavailable\r\n"
                               "Content-Length: 18\r\n\r\nStream unavailable\r\n";
        send(client_sock, err_resp, strlen(err_resp), 0);
        close(client_sock);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_client_count--;
        xSemaphoreGive(s_mutex);
        vTaskDelete(NULL);
        return;
    }

    /* 登记到客户端注册表（LRU 踢除依据）— 必须在请求校验通过之后 */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        if (s_clients[i].fd == 0) {
            s_clients[i].fd = client_sock;
            s_clients[i].since = xTaskGetTickCount();
            break;
        }
    }
    xSemaphoreGive(s_mutex);

ESP_LOGI(TAG, "Stream client started (total %d)", s_client_count);

    /* ---- Stream loop ------------------------------------------------- */
    char part_hdr[192];
    int capture_fails = 0;

    while (1) {
        /* Dead-client probe: non-blocking recv detects TCP FIN/RST immediately.
         * On a healthy one-way MJPEG stream, recv returns -1/EAGAIN (no data from
         * client, which is expected). On a dead connection it returns 0 (FIN) or
         * -1/ECONNRESET (RST), and we exit the stream loop to free the slot. */
        char probe;
        int pr = recv(client_sock, &probe, 1, MSG_DONTWAIT);
        if (pr == 0 || (pr < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            ESP_LOGW(TAG, "Client disconnected (probe rv=%d errno=%d)", pr, errno);
            break;
        }

        frame_msg_t msg;
        if (frame_broadcaster_get_frame(sub, &msg)) {
            capture_fails = 0;

            /* Build multipart part header */
            int hdrlen = snprintf(part_hdr, sizeof(part_hdr),
                "\r\n--" MJPEG_BOUNDARY "\r\n"
                "Content-Type: image/jpeg\r\n"
                "Content-Length: %zu\r\n"
                "\r\n", msg.len);

            /* Send part header */
            if (send(client_sock, part_hdr, hdrlen, 0) != hdrlen) {
                frame_broadcaster_release(&msg);
                break;
            }

            /* Send JPEG body in CHUNK_SIZE pieces */
            size_t remaining = msg.len;
            const uint8_t *ptr = msg.data;
            while (remaining > 0) {
                size_t chunk = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;
                int sent = send(client_sock, (const char *)ptr, chunk, 0);
                if (sent <= 0) {
                    frame_broadcaster_release(&msg);
                    goto stream_done;
                }
                ptr += sent;
                remaining -= sent;
            }

            /* Trailing CRLF */
            if (send(client_sock, "\r\n", 2, 0) != 2) {
                frame_broadcaster_release(&msg);
                break;
            }

            frame_broadcaster_release(&msg);
        } else {
            capture_fails++;
            if (capture_fails >= 10) {
                ESP_LOGW(TAG, "No frames after %d attempts, ending stream", capture_fails);
                break;
            }
        }

        /* Frame-rate throttle — ~33 fps max */
        vTaskDelay(pdMS_TO_TICKS(30));
    }

stream_done:
    frame_broadcaster_unsubscribe(sub);

    /* Send closing boundary (best-effort) */
    send(client_sock, "\r\n--" MJPEG_BOUNDARY "--\r\n",
         6 + strlen(MJPEG_BOUNDARY), 0);

    close(client_sock);

/* 从注册表注销 */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        if (s_clients[i].fd == client_sock) {
            s_clients[i].fd = 0;
            break;
        }
    }
    s_client_count--;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Stream client disconnected (total %d)", s_client_count);
    vTaskDelete(NULL);
}
/* ------------------------------------------------------------------ */
/*  Listen task — accepts connections, spawns client tasks             */
/* ------------------------------------------------------------------ */

static void mjpeg_listen_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Listen task started");
    while (s_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(s_listen_sock,
                                 (struct sockaddr *)&client_addr,
                                 &addr_len);
        if (client_sock < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            /* s_running check — if stopped, exit cleanly */
            if (!s_running) break;
            ESP_LOGE(TAG, "accept() failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* 发送超时兜底（2026-09-04 家族同步）：TCP 零窗口客户端的 send()
         * 会长期阻塞占住任务；10s 超时让其走断开清理。 */
        struct timeval snd_to = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));

        /* 防锤击护栏（PIT-038 多 peer v2，自 ai/luatos 移植 2026-09-11，issue #11）：
         * 同 IP 两次接入间隔 <5s（NVR 类查看端 1-2s 重连风暴签名）→ 503 +
         * 指数退避（10s 起步翻倍、封顶 5 分钟）。churn 不挡在门口的话，
         * 设备侧先关的短命连接会把 TIME_WAIT 堆满 socket 表 → 全栈 ENOMEM
         * （espectre ping errno=12 / pthread 创建失败 / httpd 哑）。4 项每 IP
         * 独立退避表（环替换），拒绝静默计数防日志风暴。 */
        {
            enum { HAMMER_SLOTS = 4, HAMMER_MIN_GAP_MS = 5000 };
            static struct {
                struct in_addr peer;
                TickType_t last_accept;   /* 上次放行时刻 */
                TickType_t until;         /* 退避截止 */
                uint32_t backoff_ms;
                uint32_t rejected;
            } s_hammer[HAMMER_SLOTS];
            static int s_hammer_next;
            TickType_t now = xTaskGetTickCount();
            int h = -1;
            for (int i = 0; i < HAMMER_SLOTS; i++) {
                if (s_hammer[i].peer.s_addr == client_addr.sin_addr.s_addr) {
                    h = i;
                    break;
                }
            }
            if (h >= 0 && ((int32_t)(now - s_hammer[h].until) < 0 ||
                           (int32_t)(now - s_hammer[h].last_accept) <
                               pdMS_TO_TICKS(HAMMER_MIN_GAP_MS))) {
                s_hammer[h].until = now + pdMS_TO_TICKS(s_hammer[h].backoff_ms);
                s_hammer[h].backoff_ms = s_hammer[h].backoff_ms < 300000
                                             ? s_hammer[h].backoff_ms * 2 : 300000;
                if (++s_hammer[h].rejected % 50 == 1) {
                    char ipstr[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, ipstr, sizeof(ipstr));
                    ESP_LOGI(TAG, "Hammer guard: rejected %u from %s (backoff %us)",
                             (unsigned)s_hammer[h].rejected, ipstr,
                             s_hammer[h].backoff_ms / 1000);
                }
                const char *busy =
                    "HTTP/1.1 503 Service Unavailable\r\n"
                    "Content-Length: 23\r\n\r\nRetry after cooldown\r\n";
                send(client_sock, busy, strlen(busy), 0);
                close(client_sock);
                continue;
            }
            if (h < 0) {
                h = s_hammer_next;
                s_hammer_next = (s_hammer_next + 1) % HAMMER_SLOTS;
                s_hammer[h].rejected = 0;
                s_hammer[h].until = 0;
            }
            s_hammer[h].peer = client_addr.sin_addr;
            s_hammer[h].last_accept = now;
            s_hammer[h].backoff_ms = 10000;
        }

        {
            char ipstr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, ipstr, sizeof(ipstr));
            ESP_LOGI(TAG, "Stream accept from %s", ipstr);
        }

        /* 满员时踢最旧连接（LRU）：shutdown 唤醒其阻塞 send/recv → 自行清理释放槽位 */
        bool slot_ready = false;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_client_count < MAX_STREAM_CLIENTS) {
            slot_ready = true;
        } else {
            TickType_t now = xTaskGetTickCount();
            int oldest_fd = 0;
            TickType_t oldest_age = 0;
            for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
                if (s_clients[i].fd != 0) {
                    TickType_t age = (TickType_t)(now - s_clients[i].since);
                    if (age >= oldest_age) {
                        oldest_age = age;
                        oldest_fd = s_clients[i].fd;
                    }
                }
            }
            if (oldest_fd != 0) {
                ESP_LOGW(TAG, "Max clients (%d) — kicking oldest fd=%d for newcomer",
                         MAX_STREAM_CLIENTS, oldest_fd);
                shutdown(oldest_fd, SHUT_RDWR);
            }
        }
        xSemaphoreGive(s_mutex);

        for (int wait = 0; !slot_ready && wait < 20; wait++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            slot_ready = (s_client_count < MAX_STREAM_CLIENTS);
            xSemaphoreGive(s_mutex);
        }
        if (!slot_ready) {
            ESP_LOGW(TAG, "Slot still busy after kick — rejecting with 503");
            const char *reject = "HTTP/1.1 503 Service Unavailable\r\n"
                                 "Content-Length: 25\r\n\r\nMax stream connections\r\n";
            send(client_sock, reject, strlen(reject), 0);
            close(client_sock);
            continue;
        }

        /* 计数自增必须在锁内（原实现锁外自增+悬空 give，2026-09-11 issue #11 清理） */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_client_count++;
        xSemaphoreGive(s_mutex);

        /* Spawn a dedicated client task (Core 1, priority 2) */
        BaseType_t created = xTaskCreatePinnedToCore(
            mjpeg_client_task,
            "mjpeg_cli",
            CLIENT_TASK_STACK,
            (void *)(intptr_t)client_sock,
            2,
            NULL,
            1);

        if (created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create client task");
            close(client_sock);
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_client_count--;
            xSemaphoreGive(s_mutex);
        }
    }

    ESP_LOGI(TAG, "Listen task exiting");
    /* 自清全局句柄：stop()/OTA quiesce 轮询此标志判断任务已退，避免对已死
     * 任务 vTaskDelete 悬垂句柄（PIT-037，自 ai 同步 2026-09-11） */
    s_listen_task = NULL;
    vTaskDelete(NULL);
}
/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t mjpeg_stream_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    s_client_count = 0;
    ESP_LOGI(TAG, "MJPEG streamer initialized (max %d clients)", MAX_STREAM_CLIENTS);
    return ESP_OK;
}

esp_err_t mjpeg_stream_server_start(uint16_t port)
{
    /* Create TCP listen socket */
    s_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_sock < 0) {
        ESP_LOGE(TAG, "Failed to create listen socket: errno %d", errno);
        return ESP_FAIL;
    }

    int opt = 1;
    setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(s_listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "Failed to bind port %d: errno %d", port, errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        return ESP_FAIL;
    }

    if (listen(s_listen_sock, LISTEN_BACKLOG) != 0) {
        ESP_LOGE(TAG, "Failed to listen on port %d: errno %d", port, errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        return ESP_FAIL;
    }

    s_running = true;

    /* Spawn listen task on Core 1 */
    BaseType_t created = xTaskCreatePinnedToCore(
        mjpeg_listen_task,
        "mjpeg_listen",
        CLIENT_TASK_STACK,
        NULL,
        3,      /* slightly higher than client tasks */
        &s_listen_task,
        1);     /* Core 1 */

    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create listen task");
        close(s_listen_sock);
        s_listen_sock = -1;
        s_running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "MJPEG streamer started on port %d", port);
    return ESP_OK;
}

int mjpeg_stream_client_count(void)
{
    int count = 0;
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        count = s_client_count;
        xSemaphoreGive(s_mutex);
    }
    return count;
}
