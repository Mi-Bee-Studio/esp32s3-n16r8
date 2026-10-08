/*
 * MiBee Cam v0.1 — Frame broadcaster implementation
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Implements a single-producer, multi-consumer frame distribution model:
 *
 *   Broadcaster task
 *     └─ esp_camera_fb_get()  ── blocks until frame ready
 *     └─ memcpy to internal cache (PSRAM)
 *     └─ esp_camera_fb_return()
 *     └─ every ~5 s: log framerate + subscriber count
 *
 *   Consumer (any task)
 *     └─ frame_broadcaster_subscribe()
 *     └─ frame_broadcaster_get_frame()  ── copies from cache (non-blocking)
 *     └─ process frame ...
 *     └─ frame_broadcaster_release()
 *     └─ frame_broadcaster_unsubscribe()
 *
 * The camera fb queue (fb_count=2) is never held across consumer boundaries,
 * preventing the deadlock that would occur if 3+ consumers held raw fb refs.
 */

#include "frame_broadcaster.h"
#include "watchdog.h"
#include "camera_driver.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "frame_broadcaster";

/* ------------------------------------------------------------------ */
/*  Internal structures                                                */
/* ------------------------------------------------------------------ */

struct frame_sub {
    frame_sub_type_t type;
    bool             active;
};

/** Subscriber slot array. */
static struct frame_sub s_subs[FBROADCAST_MAX_SUBSCRIBERS];

/** Protects subscribe() / unsubscribe(). */
static SemaphoreHandle_t s_mutex;

/** Protects the managed frame cache. */
static SemaphoreHandle_t s_cache_mutex;

/** Single-slot managed buffer — always holds the latest frame. */
static frame_msg_t s_cache = {0};

/** Monotonic frame sequence counter. */
static uint32_t s_seq = 0;

/** Broadcaster FreeRTOS task handle. */
static TaskHandle_t s_task = NULL;

/** Flag to signal the broadcaster task to stop. */
static volatile bool s_running = false;

/** Task handle waiting for broadcaster to stop (true-join). */
static TaskHandle_t s_stop_waiter = NULL;
/* ---- FPS tracking ------------------------------------------------- */

static uint32_t s_frame_count = 0;
static uint32_t s_last_fps_log_ms = 0;

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static int count_active_subs(void)
{
    int count = 0;
    for (int i = 0; i < FBROADCAST_MAX_SUBSCRIBERS; i++) {
        if (s_subs[i].active) count++;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/*  Internal publish — updates the managed cache                       */
/* ------------------------------------------------------------------ */

static void publish_frame(const uint8_t *jpeg_data, size_t jpeg_len)
{
    if (!jpeg_data || jpeg_len == 0) return;

    s_seq++;

    /* ---- Update the single-slot latest frame cache -------------- */
    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);

    if (s_cache.data) {
        free(s_cache.data);
        s_cache.data = NULL;
    }

    s_cache.data = heap_caps_malloc(jpeg_len, MALLOC_CAP_SPIRAM);
    if (s_cache.data) {
        memcpy(s_cache.data, jpeg_data, jpeg_len);
        s_cache.len  = jpeg_len;
        s_cache.seq  = s_seq;
    }

    xSemaphoreGive(s_cache_mutex);

    /* ---- FPS logging every ~5 s -------------------------------- */
    s_frame_count++;
    uint32_t now = pdTICKS_TO_MS(xTaskGetTickCount());
    if (s_last_fps_log_ms == 0) {
        s_last_fps_log_ms = now;
    } else if (now - s_last_fps_log_ms >= 5000) {
        float fps = (float)s_frame_count * 1000.0f / (float)(now - s_last_fps_log_ms);
        ESP_LOGI(TAG, "~%.1f fps  subs=%d  seq=%lu",
                 (double)fps, count_active_subs(), (unsigned long)s_seq);
        s_frame_count = 0;
        s_last_fps_log_ms = now;
    }
}

/* ------------------------------------------------------------------ */
/*  Broadcaster task — camera_frame producer                           */
/* ------------------------------------------------------------------ */

static void broadcaster_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Broadcaster task started");
    watchdog_register_current("bcast");
    s_last_fps_log_ms = 0;
    s_frame_count = 0;

    while (s_running) {
        camera_fb_t *fb = esp_camera_fb_get();
        watchdog_feed_current();
        if (fb) {
            publish_frame(fb->buf, fb->len);
            esp_camera_fb_return(fb);
        } else {
            ESP_LOGW(TAG, "esp_camera_fb_get returned NULL");
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    /* Signal the waiter that we have exited the loop (true-join) */
    if (s_stop_waiter) {
        xTaskNotifyGive(s_stop_waiter);
        s_stop_waiter = NULL;
    }

    ESP_LOGI(TAG, "Broadcaster task stopped");
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t frame_broadcaster_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_cache_mutex = xSemaphoreCreateMutex();

    if (!s_mutex || !s_cache_mutex) {
        ESP_LOGE(TAG, "Failed to create mutexes");
        if (s_mutex)       vSemaphoreDelete(s_mutex);
        if (s_cache_mutex) vSemaphoreDelete(s_cache_mutex);
        return ESP_FAIL;
    }

    memset(s_subs, 0, sizeof(s_subs));
    s_seq = 0;
    s_running = false;
    s_task = NULL;

    ESP_LOGI(TAG, "Initialized (%d subscriber slots)", FBROADCAST_MAX_SUBSCRIBERS);
    return ESP_OK;
}

esp_err_t frame_broadcaster_start(void)
{
    if (s_task != NULL) {
        ESP_LOGW(TAG, "Task already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_running = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        broadcaster_task,
        "fbroadcast",
        4096,            /* stack — shallow call chain */
        NULL,
        5,               /* priority above idle */
        &s_task,
        1                /* app core (core 1) */
    );

    if (ret != pdPASS) {
        s_running = false;
        s_task = NULL;
        ESP_LOGE(TAG, "Failed to create task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Task started on core %d", 1);
    return ESP_OK;
}

void frame_broadcaster_stop(void)
{
    s_running = false;
    if (s_task) {
        s_stop_waiter = xTaskGetCurrentTaskHandle();
        /* Notify task so it checks s_running promptly */
        xTaskNotifyGive(s_task);
        /* Wait for the task to signal it has exited the loop */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        s_task = NULL;
        s_stop_waiter = NULL;
    }
}

void frame_broadcaster_deinit(void)
{
    frame_broadcaster_stop();

    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
    if (s_cache.data) {
        free(s_cache.data);
        s_cache.data = NULL;
        s_cache.len = 0;
    }
    xSemaphoreGive(s_cache_mutex);

    if (s_cache_mutex) {
        vSemaphoreDelete(s_cache_mutex);
        s_cache_mutex = NULL;
    }
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    ESP_LOGI(TAG, "Deinitialized");
}

frame_sub_t *frame_broadcaster_subscribe(frame_sub_type_t type)
{
    if (!s_mutex) {
        ESP_LOGE(TAG, "Subscribe called before init");
        return NULL;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    frame_sub_t *sub = NULL;
    for (int i = 0; i < FBROADCAST_MAX_SUBSCRIBERS; i++) {
        if (!s_subs[i].active) {
            sub = &s_subs[i];
            break;
        }
    }

    if (!sub) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "No free subscriber slots");
        return NULL;
    }

    sub->type   = type;
    sub->active = true;

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Subscriber registered type=%d", (int)type);
    return sub;
}

void frame_broadcaster_unsubscribe(frame_sub_t *sub)
{
    if (!sub) return;
    if (!s_mutex) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (sub->active) {
        sub->active = false;
        ESP_LOGI(TAG, "Subscriber type=%d unregistered", (int)sub->type);
    }

    xSemaphoreGive(s_mutex);
}

bool frame_broadcaster_get_frame(frame_sub_t *sub, frame_msg_t *msg)
{
    if (!sub || !sub->active || !msg) return false;
    if (!s_cache_mutex) return false;

    xSemaphoreTake(s_cache_mutex, portMAX_DELAY);

    if (!s_cache.data) {
        xSemaphoreGive(s_cache_mutex);
        return false;
    }

    msg->data = heap_caps_malloc(s_cache.len, MALLOC_CAP_SPIRAM);
    if (!msg->data) {
        xSemaphoreGive(s_cache_mutex);
        return false;
    }

    memcpy(msg->data, s_cache.data, s_cache.len);
    msg->len = s_cache.len;
    msg->seq = s_cache.seq;

    xSemaphoreGive(s_cache_mutex);
    return true;
}

void frame_broadcaster_release(frame_msg_t *msg)
{
    if (msg && msg->data) {
        free(msg->data);
        msg->data = NULL;
        msg->len  = 0;
    }
}
