#pragma once

/*
 * Face detection (ESP-DL human_face_detect, MSR+MNP model stored in flash).
 * Not thread-safe: call from one task (the camera task).
 */

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_KEYPOINTS 5   /* left eye, mouth-left, nose, right eye, mouth-right (model order) */

typedef struct {
    int   x0, y0, x1, y1;          /* box in input-image pixels */
    float score;                   /* 0..1 */
    int   kp[FACE_KEYPOINTS * 2];  /* x,y pairs */
} face_box_t;

/* Load the model (slow the first time — call once before streaming). */
esp_err_t face_detect_init(void);

/* Detect faces in an RGB565 (little-endian) image. Returns the number of
 * faces written to out (sorted by score), or -1 on error. *ms = run time. */
int face_detect_run(const uint8_t *rgb565, int width, int height,
                    face_box_t *out, int max, int *ms);

#ifdef __cplusplus
}
#endif
