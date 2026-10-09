#pragma once

/*
 * Fingerprint service — a FreeRTOS task that owns the sensor.
 *
 * Normal mode: polls for a finger, identifies it, registers attendance and
 * reports the result. Admin mode (scanning paused): executes requests from
 * the UI — enroll, delete, wipe, status.
 *
 * Events are delivered through the callback from the service task; the
 * callback must take the LVGL lock itself before touching widgets.
 */

#include "esp_err.h"
#include "attendance.h"
#include "student_db.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    FP_EVT_SENSOR_STATUS,   /* ok, capacity, templates */
    FP_EVT_SCAN_MATCH,      /* student, att */
    FP_EVT_SCAN_UNKNOWN,    /* finger not in library */
    FP_EVT_SCAN_BAD_IMAGE,  /* could not read finger, try again */
    FP_EVT_ENROLL_STEP,     /* step: 1 = place finger, 2 = lift, 3 = place again, 4 = saving */
    FP_EVT_ENROLL_DONE,     /* ok; on failure: code + (duplicate) student */
    FP_EVT_OP_DONE,         /* ok, step = fp_op_t, count = templates removed */
} fp_evt_type_t;

typedef enum {
    FP_OP_DELETE = 0,
    FP_OP_WIPE,
    FP_OP_CLEANUP,
} fp_op_t;

typedef enum {
    FP_ENROLL_OK = 0,
    FP_ENROLL_CANCELLED,
    FP_ENROLL_TIMEOUT,
    FP_ENROLL_DUPLICATE,    /* finger already belongs to `student` */
    FP_ENROLL_MISMATCH,     /* two captures didn't match */
    FP_ENROLL_FULL,         /* sensor library full */
    FP_ENROLL_SENSOR_ERR,
} fp_enroll_result_t;

typedef enum {
    FP_SRC_FINGER = 0,
    FP_SRC_FACE,            /* scan events from face_service */
} fp_source_t;

typedef struct {
    fp_evt_type_t type;
    fp_source_t   source;
    bool          ok;
    int           step;
    fp_enroll_result_t enroll_result;
    student_t     student;
    att_result_t  att;
    uint16_t      capacity;
    uint16_t      templates;
    int           count;
} fp_event_t;

typedef void (*fp_event_cb_t)(const fp_event_t *evt);

esp_err_t fp_service_start(fp_event_cb_t cb);

/* Pause/resume attendance scanning (paused while the admin screen is open). */
void fp_service_set_scanning(bool enabled);

/* Requests (non-blocking; result arrives as an event) */
void fp_service_enroll(uint16_t student_id, int finger_idx);
void fp_service_cancel(void);
void fp_service_delete_slot(int slot);
void fp_service_wipe_all(void);          /* empty library + clear DB references */
void fp_service_cleanup_orphans(void);   /* delete templates no student references */
void fp_service_refresh_status(void);    /* re-handshake, emits FP_EVT_SENSOR_STATUS */

bool fp_service_sensor_ok(void);
