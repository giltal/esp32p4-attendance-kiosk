#pragma once

/*
 * OV02C10 MIPI-CSI camera (via esp_video): 1288x728 RAW10 -> ISP -> RGB565.
 *
 * All hardware work happens in the camera task: camera_start() only requests
 * streaming and returns immediately (safe to call from the LVGL task). The
 * hardware is initialised on the first start and never torn down (esp_video
 * deinit/re-init is known to hang). Poll camera_state() for progress/errors.
 *
 * Frames are delivered to the callback from the camera task; the callback must
 * return quickly and must not keep the buffer pointer.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef void (*camera_frame_cb_t)(const uint8_t *rgb565, int width, int height, void *user);

typedef enum {
    CAMERA_IDLE = 0,
    CAMERA_STARTING,     /* initialising hardware / starting stream */
    CAMERA_STREAMING,
    CAMERA_ERROR,        /* see camera_error() */
} camera_state_t;

/* Request streaming; frames go to cb. Non-blocking. */
esp_err_t camera_start(camera_frame_cb_t cb, void *user);

/* Stop streaming (takes effect after the current frame). Non-blocking. */
void camera_stop(void);

camera_state_t camera_state(void);
bool camera_is_streaming(void);

/* Short English description of the last failure (CAMERA_ERROR). */
const char *camera_error(void);

/* Frame size (valid once streaming has started). */
int camera_width(void);
int camera_height(void);

/* Frames received since the stream started (for FPS display). */
uint32_t camera_frame_count(void);
