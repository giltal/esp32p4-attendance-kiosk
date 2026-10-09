#pragma once

/*
 * Face recognition service. Owns the camera while the kiosk (main screen) is
 * active or an enrollment is running; all processing happens in the camera
 * task (see camera.h).
 *
 * Kiosk: detect -> pick the largest face -> signature -> match against
 * face_db. A student is accepted after FACE_CONFIRM_FRAMES consecutive
 * matches; the scan is registered with attendance and reported through the
 * same fp_event_t path as a fingerprint scan (source = FP_SRC_FACE).
 *
 * The live view (small mirrored frame with the face box drawn in) goes to an
 * LVGL canvas. Functions taking lv_obj_t* must be called with the LVGL lock.
 */

#include "esp_err.h"
#include "fp_service.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#define FACE_VIEW_W   322     /* 1288x728 * 0.25 */
#define FACE_VIEW_H   182

typedef enum {
    FACE_ENROLL_PROGRESS,     /* count/total, hint */
    FACE_ENROLL_DONE,         /* result (+ dup for FACE_ENROLL_DUPLICATE) */
} face_enroll_evt_type_t;

typedef enum {
    FACE_ENROLL_OK = 0,
    FACE_ENROLL_DUPLICATE,    /* this face already belongs to `dup` */
    FACE_ENROLL_TIMEOUT,
    FACE_ENROLL_CANCELLED,
    FACE_ENROLL_ERROR,
} face_enroll_result_t;

typedef struct {
    face_enroll_evt_type_t type;
    int count, total;
    const char *hint;         /* Hebrew UI hint, may be NULL */
    face_enroll_result_t result;
    student_t dup;
} face_enroll_evt_t;

typedef void (*face_enroll_cb_t)(const face_enroll_evt_t *e);

/* scan_cb receives FP_EVT_SCAN_MATCH / FP_EVT_SCAN_UNKNOWN with source=FP_SRC_FACE. */
void face_service_init(fp_event_cb_t scan_cb, face_enroll_cb_t enroll_cb);

/* Kiosk recognition on/off (main screen shown and feature enabled). */
void face_service_set_kiosk(bool on);
bool face_service_kiosk_on(void);

/* Canvas for the kiosk live view (main screen). LVGL lock held. */
void face_service_set_kiosk_view(lv_obj_t *canvas);

/* Start enrolling a student's face; live view goes to `canvas`. LVGL lock held. */
esp_err_t face_service_enroll_start(uint16_t student_id, lv_obj_t *canvas);

/* Abort an enrollment (also detaches its canvas). LVGL lock held. */
void face_service_enroll_cancel(void);
