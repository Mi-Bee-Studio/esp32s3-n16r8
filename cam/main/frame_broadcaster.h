/*
 * MiBee Cam v0.1 — Frame broadcaster / latest-frame cache
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Decouples camera frame producers from consumers.  A single FreeRTOS task
 * grabs frames via esp_camera_fb_get(), copies the JPEG payload into an
 * internal PSRAM managed buffer, and returns the camera fb immediately.
 *
 * Consumers subscribe and call get_frame() to snapshot the latest frame
 * (non-blocking, always returns a PSRAM copy).  The copy must be released
 * via release().
 *
 * Why copy instead of zero-copy forwarding?
 *   esp_camera_fb_get() returns from a queue of fb_count=2 buffers.  If N>2
 *   consumers each held a raw fb pointer, the queue would drain to empty and
 *   fb_get would deadlock.  Copying to a managed buffer (single-slot latest)
 *   solves this without a refcount scheme.
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum simultaneous subscribers. */
#define FBROADCAST_MAX_SUBSCRIBERS  4

/** Subscriber type — used for diagnostic logging. */
typedef enum {
    FRAMESUB_MJPEG = 0,
    FRAMESUB_RTSP,
    FRAMESUB_AI_PIPELINE,
    FRAMESUB_OTHER,
} frame_sub_type_t;

/**
 * Owned JPEG buffer — returned by get_frame().
 * Caller MUST call frame_broadcaster_release() when done.
 */
typedef struct {
    uint8_t *data;     /* heap_caps_malloc'd in PSRAM */
    size_t   len;      /* JPEG size in bytes */
    uint32_t seq;      /* monotonic frame sequence number */
} frame_msg_t;

/** Opaque subscriber handle. */
typedef struct frame_sub frame_sub_t;

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialize the frame broadcaster.
 * Call once after camera_init() before any subscribe/start.
 */
esp_err_t frame_broadcaster_init(void);

/**
 * @brief Start the broadcaster FreeRTOS task.
 * The task loops: fb_get -> copy to managed buffer -> fb_return.
 */
esp_err_t frame_broadcaster_start(void);

/**
 * @brief Stop the broadcaster task.
 */
void frame_broadcaster_stop(void);

/**
 * @brief Deinitialize and free all resources.
 */
void frame_broadcaster_deinit(void);

/* ------------------------------------------------------------------ */
/*  Subscriber management                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief Register a subscriber.
 * @param type  Subscriber type for diagnostic logging
 * @return Opaque handle, or NULL if no free slots
 */
frame_sub_t *frame_broadcaster_subscribe(frame_sub_type_t type);

/**
 * @brief Unregister a subscriber.
 * @param sub  Handle returned by subscribe()
 */
void frame_broadcaster_unsubscribe(frame_sub_t *sub);

/* ------------------------------------------------------------------ */
/*  Frame access                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Snapshot the latest frame (non-blocking).
 *
 * Returns a PSRAM copy of the internal managed buffer.
 * Caller MUST call frame_broadcaster_release() to free the copy.
 *
 * @param sub  Subscriber handle
 * @param msg  Output: frame data (allocated PSRAM copy)
 * @return true if a frame was available, false otherwise
 */
bool frame_broadcaster_get_frame(frame_sub_t *sub, frame_msg_t *msg);

/**
 * @brief Release a frame snapshot previously returned by get_frame().
 * @param msg  Frame to release
 */
void frame_broadcaster_release(frame_msg_t *msg);

#ifdef __cplusplus
}
#endif
