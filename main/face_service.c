#include "face_service.h"
#include "face_detect.h"
#include "face_recog.h"
#include "face_db.h"
#include "camera.h"
#include "attendance.h"
#include "student_db.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "driver/ppa.h"
#include <string.h>

static const char *TAG = "FACE_SVC";

/* Working image for detection/recognition: 1288x728 * 0.5, not mirrored */
#define WORK_SCALE        0.5f
#define WORK_W            644
#define WORK_H            364
#define VIEW_SCALE        0.25f
#define BUF_ALIGN         128

#define PROCESS_MIN_MS    150       /* ~6 analysed frames/s */
#define MATCH_THR         0.55f     /* similarity needed to count as a match */
#define CONFIRM_FRAMES    3         /* consecutive matches before accepting */
#define MIN_FACE_W        80        /* px in the work image (~160 px full frame): standing in front, not passing by */
#define SAME_COOLDOWN_MS  8000      /* ignore the same student right after a scan */
#define UNKNOWN_FRAMES    6         /* unmatched frames before "not recognised" */
#define UNKNOWN_COOLDOWN_MS 5000
#define MAX_FACES         4

#define ENROLL_MIN_W      70
#define ENROLL_GAP_MS     400       /* between samples */
#define ENROLL_TIMEOUT_MS 30000

typedef enum { MODE_OFF, MODE_KIOSK, MODE_ENROLL } mode_t_;

static fp_event_cb_t s_scan_cb;
static face_enroll_cb_t s_enroll_cb;

static volatile mode_t_ s_mode = MODE_OFF;
static volatile bool s_kiosk_wanted;

/* LVGL objects — read/written only with the LVGL lock held */
static lv_obj_t *s_kiosk_view;
static lv_obj_t *s_enroll_view;

static ppa_client_handle_t s_ppa;
static uint8_t *s_work;
static uint8_t *s_view[2];
static size_t s_work_size, s_view_size;
static int s_view_back;
static int64_t s_last_us;

/* kiosk state (camera task) */
static uint16_t s_streak_id;
static int s_streak;
static uint16_t s_last_id;
static int64_t s_last_accept_us;
static int s_unknown_frames;
static int64_t s_last_unknown_us;

/* enroll state (camera task) */
static uint16_t s_enroll_id;
static int s_enroll_n;
static int64_t s_enroll_start_us, s_enroll_last_us;
static float *s_samples;               /* [SAMPLES * FACE_FEAT_MAX_LEN] */

static float s_feat[FACE_FEAT_MAX_LEN];

/* ── Buffers / PPA ──────────────────────────────────────────────────────── */

static esp_err_t buffers_init(void)
{
    if (s_work) return ESP_OK;
    s_work_size = (WORK_W * WORK_H * 2 + BUF_ALIGN - 1) & ~(BUF_ALIGN - 1);
    s_view_size = (FACE_VIEW_W * FACE_VIEW_H * 2 + BUF_ALIGN - 1) & ~(BUF_ALIGN - 1);
    s_work = heap_caps_aligned_calloc(BUF_ALIGN, 1, s_work_size, MALLOC_CAP_SPIRAM);
    s_view[0] = heap_caps_aligned_calloc(BUF_ALIGN, 1, s_view_size, MALLOC_CAP_SPIRAM);
    s_view[1] = heap_caps_aligned_calloc(BUF_ALIGN, 1, s_view_size, MALLOC_CAP_SPIRAM);
    s_samples = heap_caps_calloc(FACE_DB_SAMPLES_PER_STUDENT * FACE_FEAT_MAX_LEN, sizeof(float),
                                 MALLOC_CAP_SPIRAM);
    if (!s_work || !s_view[0] || !s_view[1] || !s_samples) return ESP_ERR_NO_MEM;
    ppa_client_config_t cfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    return ppa_register_client(&cfg, &s_ppa);
}

static esp_err_t scale(const uint8_t *src, int w, int h, uint8_t *dst, size_t dst_size,
                       int dw, int dh, float sc, bool mirror)
{
    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = src, .pic_w = w, .pic_h = h, .block_w = w, .block_h = h,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = dst, .buffer_size = dst_size, .pic_w = dw, .pic_h = dh,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = sc, .scale_y = sc,
        .mirror_x = mirror,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_ppa, &op);
}

/* Draw a 2-px rectangle into the (mirrored) view for a box in work coordinates. */
static void draw_box(uint16_t *view, const face_box_t *f, uint16_t color)
{
    const float k = VIEW_SCALE / WORK_SCALE;   /* work -> view */
    int x0 = FACE_VIEW_W - 1 - (int)(f->x1 * k), x1 = FACE_VIEW_W - 1 - (int)(f->x0 * k);
    int y0 = (int)(f->y0 * k), y1 = (int)(f->y1 * k);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > FACE_VIEW_W - 1) x1 = FACE_VIEW_W - 1;
    if (y1 > FACE_VIEW_H - 1) y1 = FACE_VIEW_H - 1;
    for (int t = 0; t < 2; t++) {
        for (int x = x0; x <= x1; x++) {
            if (y0 + t <= y1) view[(y0 + t) * FACE_VIEW_W + x] = color;
            if (y1 - t >= y0) view[(y1 - t) * FACE_VIEW_W + x] = color;
        }
        for (int y = y0; y <= y1; y++) {
            if (x0 + t <= x1) view[y * FACE_VIEW_W + x0 + t] = color;
            if (x1 - t >= x0) view[y * FACE_VIEW_W + x1 - t] = color;
        }
    }
}

#define C_GREEN  0x07E0
#define C_YELLOW 0xFFE0
#define C_RED    0xF800
#define C_WHITE  0xFFFF

static void show_view(void)
{
    if (!lvgl_port_lock(30)) return;
    lv_obj_t *canvas = (s_mode == MODE_ENROLL) ? s_enroll_view : s_kiosk_view;
    if (canvas) {
        lv_canvas_set_buffer(canvas, s_view[s_view_back], FACE_VIEW_W, FACE_VIEW_H, LV_COLOR_FORMAT_RGB565);
        s_view_back ^= 1;
    }
    lvgl_port_unlock();
}

static int largest_face(const face_box_t *faces, int n)
{
    int best = -1, area = 0;
    for (int i = 0; i < n; i++) {
        int a = (faces[i].x1 - faces[i].x0) * (faces[i].y1 - faces[i].y0);
        if (a > area) { area = a; best = i; }
    }
    return best;
}

/* ── Kiosk recognition ──────────────────────────────────────────────────── */

static void beep_scan(const att_result_t *a)
{
    if (a->repeat) {
        bsp_audio_play_tone(1000, 80);
    } else if (a->dir == ATT_IN) {
        bsp_audio_play_tone(880, 90);  bsp_audio_play_tone(1320, 140);
    } else {
        bsp_audio_play_tone(1320, 90); bsp_audio_play_tone(880, 140);
    }
}

static uint16_t kiosk_process(const face_box_t *faces, int n, int64_t now)
{
    int bi = largest_face(faces, n);
    if (bi < 0 || faces[bi].x1 - faces[bi].x0 < MIN_FACE_W) {
        s_streak = 0;
        s_unknown_frames = 0;
        return C_WHITE;
    }
    if (face_db_student_count() == 0) return C_WHITE;   /* nobody enrolled yet */

    int ms = 0;
    if (face_recog_extract(s_work, WORK_W, WORK_H, &faces[bi], s_feat, &ms) != ESP_OK) return C_YELLOW;

    uint16_t id = 0;
    float sim = 0;
    face_db_match(s_feat, face_recog_feat_len(), 0, &id, &sim);
    ESP_LOGD(TAG, "best #%u sim %.2f (%d ms)", id, sim, ms);

    if (sim < MATCH_THR) {
        s_streak = 0;
        if (++s_unknown_frames >= UNKNOWN_FRAMES &&
            now - s_last_unknown_us > UNKNOWN_COOLDOWN_MS * 1000LL) {
            s_unknown_frames = 0;
            s_last_unknown_us = now;
            ESP_LOGI(TAG, "unknown face (best #%u sim %.2f)", id, sim);
            fp_event_t e = { .type = FP_EVT_SCAN_UNKNOWN, .source = FP_SRC_FACE };
            if (s_scan_cb) s_scan_cb(&e);
            bsp_audio_play_tone(300, 350);
        }
        return C_RED;
    }

    s_unknown_frames = 0;
    if (id == s_streak_id) s_streak++;
    else { s_streak_id = id; s_streak = 1; }
    if (s_streak < CONFIRM_FRAMES) return C_YELLOW;

    s_streak = 0;
    if (id == s_last_id && now - s_last_accept_us < SAME_COOLDOWN_MS * 1000LL) return C_GREEN;

    fp_event_t e = { .type = FP_EVT_SCAN_MATCH, .source = FP_SRC_FACE, .ok = true };
    if (!student_db_get(id, &e.student)) return C_RED;   /* stale DB entry */
    e.att = attendance_register(&e.student);
    s_last_id = id;
    s_last_accept_us = now;
    ESP_LOGI(TAG, "recognised #%u %s (sim %.2f, face %d px)", id, e.student.name, sim,
             faces[bi].x1 - faces[bi].x0);
    if (s_scan_cb) s_scan_cb(&e);
    beep_scan(&e.att);
    return C_GREEN;
}

/* ── Enrollment ─────────────────────────────────────────────────────────── */

static void enroll_emit(face_enroll_evt_type_t type, face_enroll_result_t res,
                        const char *hint, const student_t *dup)
{
    face_enroll_evt_t e = {
        .type = type, .count = s_enroll_n, .total = FACE_DB_SAMPLES_PER_STUDENT,
        .hint = hint, .result = res,
    };
    if (dup) e.dup = *dup;
    if (s_enroll_cb) s_enroll_cb(&e);
}

static void enroll_finish(face_enroll_result_t res, const student_t *dup)
{
    s_mode = s_kiosk_wanted ? MODE_KIOSK : MODE_OFF;
    if (s_mode == MODE_OFF) camera_stop();
    enroll_emit(FACE_ENROLL_DONE, res, NULL, dup);
    if (res == FACE_ENROLL_OK) {
        bsp_audio_play_tone(880, 90); bsp_audio_play_tone(1320, 140);
    } else if (res != FACE_ENROLL_CANCELLED) {
        bsp_audio_play_tone(300, 350);
    }
}

static uint16_t enroll_process(const face_box_t *faces, int n, int64_t now)
{
    if (now - s_enroll_start_us > ENROLL_TIMEOUT_MS * 1000LL) {
        enroll_finish(FACE_ENROLL_TIMEOUT, NULL);
        return C_RED;
    }
    if (n == 0) {
        enroll_emit(FACE_ENROLL_PROGRESS, 0, "הביטו ישר אל המצלמה", NULL);
        return C_WHITE;
    }
    if (n > 1) {
        enroll_emit(FACE_ENROLL_PROGRESS, 0, "רק אדם אחד מול המצלמה", NULL);
        return C_RED;
    }
    if (faces[0].x1 - faces[0].x0 < ENROLL_MIN_W) {
        enroll_emit(FACE_ENROLL_PROGRESS, 0, "התקרבו למצלמה", NULL);
        return C_YELLOW;
    }
    if (now - s_enroll_last_us < ENROLL_GAP_MS * 1000LL) return C_YELLOW;

    int len = face_recog_feat_len();
    float *dst = &s_samples[s_enroll_n * FACE_FEAT_MAX_LEN];
    if (face_recog_extract(s_work, WORK_W, WORK_H, &faces[0], dst, NULL) != ESP_OK) {
        enroll_finish(FACE_ENROLL_ERROR, NULL);
        return C_RED;
    }
    len = face_recog_feat_len();

    if (s_enroll_n == 0) {
        /* Refuse a face that already belongs to another student */
        uint16_t other = 0;
        float sim = 0;
        if (face_db_match(dst, len, s_enroll_id, &other, &sim) && sim >= MATCH_THR) {
            student_t dup = {0};
            student_db_get(other, &dup);
            ESP_LOGW(TAG, "enroll #%u: face matches #%u (sim %.2f)", s_enroll_id, other, sim);
            enroll_finish(FACE_ENROLL_DUPLICATE, &dup);
            return C_RED;
        }
    }

    s_enroll_n++;
    s_enroll_last_us = now;
    enroll_emit(FACE_ENROLL_PROGRESS, 0,
                s_enroll_n < FACE_DB_SAMPLES_PER_STUDENT ? "הזיזו מעט את הראש" : NULL, NULL);
    bsp_audio_play_tone(1000, 50);

    if (s_enroll_n >= FACE_DB_SAMPLES_PER_STUDENT) {
        /* pack samples contiguously for face_db */
        for (int i = 1; i < s_enroll_n; i++) {
            memmove(&s_samples[i * len], &s_samples[i * FACE_FEAT_MAX_LEN], len * sizeof(float));
        }
        esp_err_t r = face_db_set_student(s_enroll_id, s_samples, s_enroll_n, len);
        enroll_finish(r == ESP_OK ? FACE_ENROLL_OK : FACE_ENROLL_ERROR, NULL);
    }
    return C_GREEN;
}

/* ── Camera frame callback (camera task) ────────────────────────────────── */

static void frame_cb(const uint8_t *rgb565, int w, int h, void *user)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_us < PROCESS_MIN_MS * 1000LL) return;
    s_last_us = now;
    if (s_mode == MODE_OFF) return;
    if (w * WORK_SCALE > WORK_W || h * WORK_SCALE > WORK_H) return;

    if (scale(rgb565, w, h, s_work, s_work_size, WORK_W, WORK_H, WORK_SCALE, false) != ESP_OK) return;
    if (scale(rgb565, w, h, s_view[s_view_back], s_view_size, FACE_VIEW_W, FACE_VIEW_H,
              VIEW_SCALE, true) != ESP_OK) return;

    face_box_t faces[MAX_FACES];
    int n = face_detect_run(s_work, WORK_W, WORK_H, faces, MAX_FACES, NULL);
    if (n < 0) n = 0;

    uint16_t color = (s_mode == MODE_ENROLL) ? enroll_process(faces, n, now)
                                             : kiosk_process(faces, n, now);
    int bi = largest_face(faces, n);
    if (bi >= 0) draw_box((uint16_t *)s_view[s_view_back], &faces[bi], color);
    show_view();
}

/* ── API ────────────────────────────────────────────────────────────────── */

void face_service_init(fp_event_cb_t scan_cb, face_enroll_cb_t enroll_cb)
{
    s_scan_cb = scan_cb;
    s_enroll_cb = enroll_cb;
    if (buffers_init() != ESP_OK) ESP_LOGE(TAG, "buffer allocation failed");
}

void face_service_set_kiosk(bool on)
{
    s_kiosk_wanted = on;
    if (s_mode == MODE_ENROLL) return;          /* enrollment keeps the camera */
    if (on) {
        s_mode = MODE_KIOSK;
        s_streak = 0;
        s_unknown_frames = 0;
        camera_start(frame_cb, NULL);
    } else if (s_mode == MODE_KIOSK) {
        s_mode = MODE_OFF;
        camera_stop();
    }
}

bool face_service_kiosk_on(void)
{
    return s_mode == MODE_KIOSK;
}

void face_service_set_kiosk_view(lv_obj_t *canvas)
{
    s_kiosk_view = canvas;
}

esp_err_t face_service_enroll_start(uint16_t student_id, lv_obj_t *canvas)
{
    if (!s_work) return ESP_ERR_NO_MEM;
    s_enroll_id = student_id;
    s_enroll_n = 0;
    s_enroll_start_us = esp_timer_get_time();
    s_enroll_last_us = 0;
    s_enroll_view = canvas;
    s_mode = MODE_ENROLL;
    return camera_start(frame_cb, NULL);
}

void face_service_enroll_cancel(void)
{
    s_enroll_view = NULL;
    if (s_mode == MODE_ENROLL) {
        s_mode = s_kiosk_wanted ? MODE_KIOSK : MODE_OFF;
        if (s_mode == MODE_OFF) camera_stop();
    }
}
