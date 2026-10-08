/**
 * @file flash_viewers.h
 * @brief Viewer-driven flash LED (board-local extension, default OFF).
 *
 * When enabled in config (`flash_viewers`), drives the flash LED on while
 * any stream/capture client is present (MJPEG :81 viewer, RTSP pull via
 * stream count, /api/capture snapshot) and off after a grace period once
 * the last viewer leaves. Grace absorbs short reconnect churn (NVR-style
 * reconnect loops) so the LED does not strobe.
 *
 * When disabled (default), the module is inert and manual /api/led control
 * behaves exactly as before.
 */

#ifndef FLASH_VIEWERS_H
#define FLASH_VIEWERS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start the 1 Hz watcher task (call once after the streamer is up). */
void flash_viewers_start(void);

/** Mark capture activity (photo/snapshot) so the LED stays on for it. */
void flash_viewers_notify_capture(void);

/** Current watcher state for /api/status: 1 when LED is forced on. */
uint8_t flash_viewers_active(void);

#ifdef __cplusplus
}
#endif

#endif // FLASH_VIEWERS_H
