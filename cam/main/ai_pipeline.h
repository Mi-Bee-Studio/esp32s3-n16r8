/*
 * MiBee Cam v0.1 — AI Pipeline API (face detection + motion + QR decode)
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runs on Core 1 at priority 5.  Subscribes to the frame broadcaster
 * and processes each frame for:
 *   - Face detection (ESP-DL HumanFaceDetect MSRMNP_S8_V1)
 *   - Motion detection (frame-difference on grayscale)
 *   - QR decode (quirc)
 *
 * Results live in a mutex-protected struct for web server polling.
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Feature flags                                                      */
/* ------------------------------------------------------------------ */

/** Feature identifiers for ai_enable() / ai_is_enabled(). */
typedef enum {
    AI_FEATURE_FACE_DETECT   = 0,
    AI_FEATURE_MOTION_DETECT = 1,
    AI_FEATURE_QR_DECODE     = 2,
    AI_FEATURE_COUNT         = 3
} ai_feature_t;

/* ------------------------------------------------------------------ */
/*  Result types                                                       */
/* ------------------------------------------------------------------ */

/** Single detected face bounding box. */
typedef struct {
    int     x;           /*!< left edge  (0 .. width-1) */
    int     y;           /*!< top edge   (0 .. height-1) */
    int     w;           /*!< box width  (right_x - x) */
    int     h;           /*!< box height (bottom_y - y) */
    float   confidence;  /*!< detection score 0..1 */
} ai_face_box_t;

/** Face detection result. */
#define AI_MAX_FACES  10
typedef struct {
    int            count;
    ai_face_box_t  faces[AI_MAX_FACES];
} ai_face_result_t;

/** Motion detection result (frame-difference score). */
typedef struct {
    int score;  /*!< 0 (no motion) .. 100 (full change), threshold ~15 */
} ai_motion_result_t;

/** QR decode result. */
#define AI_MAX_QR_CODES     4
#define AI_QR_STRING_LEN  256
typedef struct {
    int     count;
    char    strings[AI_MAX_QR_CODES][AI_QR_STRING_LEN];
} ai_qr_result_t;

/** Aggregated AI result — populated by ai_process_frame(). */
typedef struct {
    ai_face_result_t   face;
    ai_motion_result_t motion;
    ai_qr_result_t     qr;
    uint32_t           frame_seq;  /*!< frame_broadcaster sequence number */
} ai_result_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise the AI pipeline.
 *
 * Loads face detection model (from flash rodata), allocates grayscale
 * buffers in PSRAM, creates quirc decoder instance and mutexes.
 *
 * Logs internal free heap before and after model load and asserts
 * > 30 KB remains free.
 *
 * @return ESP_OK on success.
 */
esp_err_t ai_init(void);

/**
 * @brief Process one camera frame through the enabled AI features.
 *
 * JPEG decode → (face detect) + (motion detect) + (QR decode).
 * Results are written into an internal mutex-protected struct
 * readable via ai_get_result().
 *
 * @param fb  Camera frame (must be PIXFORMAT_JPEG, VGA).
 */
void ai_process_frame(camera_fb_t *fb);

/**
 * @brief Snapshot the latest AI results (thread-safe).
 *
 * @param out  Filled with a copy of the current results.
 * @return true if a result is available (at least one frame processed),
 *         false otherwise.
 */
bool ai_get_result(ai_result_t *out);

/**
 * @brief Enable or disable a specific AI feature.
 *
 * Disabled features are skipped in the processing loop, saving
 * CPU time and memory bandwidth.
 */
void ai_enable(ai_feature_t feature, bool enable);

/**
 * @brief Check whether an AI feature is currently enabled.
 */
bool ai_is_enabled(ai_feature_t feature);

/**
 * @brief Start the AI FreeRTOS task on Core 1.
 *
 * Called automatically by ai_init().  Call again after ai_stop_task()
 * to restart processing.
 *
 * @return ESP_OK on success.
 */
esp_err_t ai_start_task(void);

/**
 * @brief Stop the AI FreeRTOS task.
 */
void ai_stop_task(void);

#ifdef __cplusplus
}
#endif
