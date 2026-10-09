#include "ui_camera.h"
#include "ui_common.h"
#include "camera.h"
#include "face_detect.h"
#include "ui_admin.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "driver/ppa.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "UI_CAMERA";

/* 1288x728 * 0.625 = 805x455 — fits the admin tab; PPA scale step is 1/16 */
#define PREVIEW_SCALE     0.625f
#define PREVIEW_W         805
#define PREVIEW_H         455
#define PREVIEW_MIN_MS    100          /* ~10 fps max, keeps LVGL responsive */
#define BUF_ALIGN         128          /* L1+L2 cache line for PSRAM on P4 */

static lv_obj_t *s_canvas;
static lv_obj_t *s_btn_lbl;
static lv_obj_t *s_info;
static lv_timer_t *s_info_timer;

static ppa_client_handle_t s_ppa;
static uint8_t *s_out[2];
static size_t s_out_size;
static int s_back;
static int64_t s_last_us;
static volatile uint32_t s_shown;
static bool s_mirror = true;

/* Face detection overlay */
#define MAX_FACES  4
static lv_obj_t *s_box[MAX_FACES];
static face_box_t s_faces[MAX_FACES];
static volatile int s_nfaces;
static volatile int s_det_ms;
static bool s_detect_on = true;

/* ── Buffers / PPA ──────────────────────────────────────────────────────── */

static esp_err_t preview_alloc(void)
{
    if (s_out[0]) return ESP_OK;
    s_out_size = (PREVIEW_W * PREVIEW_H * 2 + BUF_ALIGN - 1) & ~(BUF_ALIGN - 1);
    for (int i = 0; i < 2; i++) {
        s_out[i] = heap_caps_aligned_calloc(BUF_ALIGN, 1, s_out_size, MALLOC_CAP_SPIRAM);
        if (!s_out[i]) return ESP_ERR_NO_MEM;
    }
    ppa_client_config_t cfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    return ppa_register_client(&cfg, &s_ppa);
}

/* Scale a camera frame into out (PREVIEW_W x PREVIEW_H RGB565). */
static esp_err_t scale_frame(const uint8_t *rgb565, int w, int h, uint8_t *out)
{
    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = rgb565, .pic_w = w, .pic_h = h,
            .block_w = w, .block_h = h, .block_offset_x = 0, .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = out, .buffer_size = s_out_size,
            .pic_w = PREVIEW_W, .pic_h = PREVIEW_H, .block_offset_x = 0, .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = PREVIEW_SCALE,
        .scale_y = PREVIEW_SCALE,
        .mirror_x = s_mirror,          /* selfie view */
        .mirror_y = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_ppa, &op);
}

/* ── Live preview (camera task context) ─────────────────────────────────── */

static void preview_frame_cb(const uint8_t *rgb565, int w, int h, void *user)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_us < PREVIEW_MIN_MS * 1000) return;
    s_last_us = now;

    if (w * PREVIEW_SCALE > PREVIEW_W || h * PREVIEW_SCALE > PREVIEW_H) return;
    if (scale_frame(rgb565, w, h, s_out[s_back]) != ESP_OK) return;

    /* Detect on the scaled (and mirrored) frame, so boxes map 1:1 onto the preview */
    face_box_t faces[MAX_FACES];
    int n = 0, ms = 0;
    if (s_detect_on) {
        n = face_detect_run(s_out[s_back], PREVIEW_W, PREVIEW_H, faces, MAX_FACES, &ms);
        if (n < 0) n = 0;
        s_det_ms = ms;
    }

    if (lvgl_port_lock(50)) {
        if (s_canvas) {
            lv_canvas_set_buffer(s_canvas, s_out[s_back], PREVIEW_W, PREVIEW_H, LV_COLOR_FORMAT_RGB565);
            for (int i = 0; i < MAX_FACES; i++) {
                if (i < n) {
                    lv_obj_set_pos(s_box[i], faces[i].x0, faces[i].y0);
                    lv_obj_set_size(s_box[i], faces[i].x1 - faces[i].x0, faces[i].y1 - faces[i].y0);
                    lv_obj_remove_flag(s_box[i], LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(s_box[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
            memcpy(s_faces, faces, sizeof(face_box_t) * n);
            s_nfaces = n;
            s_back ^= 1;
            s_shown++;
        }
        lvgl_port_unlock();
    }
}

/* ── UI ─────────────────────────────────────────────────────────────────── */

static void info_timer_cb(lv_timer_t *t)
{
    static uint32_t last_frames, last_shown;
    uint32_t f = camera_frame_count(), s = s_shown;
    camera_state_t st = camera_state();
    if (st == CAMERA_STARTING) {
        lv_label_set_text(s_info, "מפעיל מצלמה...");
    } else if (st == CAMERA_ERROR) {
        lv_label_set_text_fmt(s_info, "שגיאה במצלמה: %s", camera_error());
        lv_label_set_text(s_btn_lbl, LV_SYMBOL_PLAY " הפעלת מצלמה");
        lv_timer_pause(s_info_timer);
    } else if (camera_is_streaming()) {
        if (s_detect_on) {
            lv_label_set_text_fmt(s_info, "faces: %d (%d%%) | detect %d ms | camera %lu fps | display %lu fps",
                                  s_nfaces, s_nfaces ? (int)(s_faces[0].score * 100) : 0, s_det_ms,
                                  (unsigned long)(f - last_frames), (unsigned long)(s - last_shown));
        } else {
            lv_label_set_text_fmt(s_info, "%dx%d | camera %lu fps | display %lu fps",
                                  camera_width(), camera_height(),
                                  (unsigned long)(f - last_frames), (unsigned long)(s - last_shown));
        }
    }
    last_frames = f;
    last_shown = s;
}

static void hide_boxes(void)
{
    for (int i = 0; i < MAX_FACES; i++) {
        if (s_box[i]) lv_obj_add_flag(s_box[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_nfaces = 0;
}

void ui_camera_stop(void)
{
    camera_stop();
    hide_boxes();
    if (s_btn_lbl) lv_label_set_text(s_btn_lbl, LV_SYMBOL_PLAY " הפעלת מצלמה");
    if (s_info_timer) lv_timer_pause(s_info_timer);
}

static void preview_start(void)
{
    if (preview_alloc() != ESP_OK) {
        ui_toast("אין מספיק זיכרון לתצוגה", CLR_ERR);
        return;
    }
    lv_label_set_text(s_info, "מפעיל מצלמה...");
    /* Non-blocking: the camera task initialises the hardware (and the face model
     * lazily on the first frame); progress/errors show up via info_timer_cb. */
    if (camera_start(preview_frame_cb, NULL) != ESP_OK) {
        lv_label_set_text(s_info, "שגיאה ביצירת משימת המצלמה");
        ui_toast("שגיאה בהפעלת המצלמה", CLR_ERR);
        return;
    }
    lv_label_set_text(s_btn_lbl, LV_SYMBOL_STOP " עצירה");
    lv_timer_resume(s_info_timer);
}

static void toggle_cb(lv_event_t *e)
{
    camera_state_t st = camera_state();
    if (st == CAMERA_STREAMING || st == CAMERA_STARTING) {
        ui_camera_stop();
        lv_label_set_text(s_info, "המצלמה כבויה");
        return;
    }
    preview_start();
}

static void mirror_cb(lv_event_t *e)
{
    s_mirror = !s_mirror;
}

static void detect_cb(lv_event_t *e)
{
    s_detect_on = !s_detect_on;
    if (!s_detect_on) hide_boxes();
}

void ui_camera_build_tab(lv_obj_t *page)
{
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *col = ui_box(page, 170, LV_PCT(100));
    ui_align(col, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 12, 0);

    lv_obj_t *b = ui_button(col, LV_SYMBOL_PLAY " הפעלת מצלמה", CLR_IN, 170, 64, toggle_cb, NULL);
    s_btn_lbl = lv_obj_get_child(b, 0);
    ui_button(col, "היפוך מראה", CLR_BTN, 170, 56, mirror_cb, NULL);
    ui_button(col, "זיהוי פנים", CLR_BTN, 170, 56, detect_cb, NULL);

    s_canvas = lv_canvas_create(page);
    if (preview_alloc() == ESP_OK) {
        lv_canvas_set_buffer(s_canvas, s_out[0], PREVIEW_W, PREVIEW_H, LV_COLOR_FORMAT_RGB565);
        lv_canvas_fill_bg(s_canvas, lv_color_black(), LV_OPA_COVER);
    }
    ui_align(s_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_base_dir(s_canvas, LV_BASE_DIR_LTR, 0);   /* box coords are physical */

    for (int i = 0; i < MAX_FACES; i++) {
        s_box[i] = lv_obj_create(s_canvas);
        lv_obj_remove_style_all(s_box[i]);
        lv_obj_set_style_border_width(s_box[i], 3, 0);
        lv_obj_set_style_border_color(s_box[i], lv_color_hex(0x00E676), 0);
        lv_obj_set_style_radius(s_box[i], 8, 0);
        lv_obj_remove_flag(s_box[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_box[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_info = ui_label(page, &g_font22, CLR_MUTED, "המצלמה כבויה");
    lv_obj_set_style_base_dir(s_info, LV_BASE_DIR_AUTO, 0);
    ui_align(s_info, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    s_info_timer = lv_timer_create(info_timer_cb, 1000, NULL);
    lv_timer_pause(s_info_timer);
}

/* ── TEMPORARY headless self-test ───────────────────────────────────────── */

static volatile bool s_dump_req;
static volatile bool s_ppa_req;
static volatile uint32_t s_stat_r, s_stat_g, s_stat_b, s_stat_n;

static void selftest_frame_cb(const uint8_t *rgb565, int w, int h, void *user)
{
    const uint16_t *px = (const uint16_t *)rgb565;
    uint32_t r = 0, g = 0, b = 0, n = 0;
    for (int y = 0; y < h; y += 32) {
        for (int x = 0; x < w; x += 32) {
            uint16_t p = px[y * w + x];
            r += (p >> 11) & 0x1F; g += (p >> 5) & 0x3F; b += p & 0x1F; n++;
        }
    }
    s_stat_r = r * 255 / (31 * n); s_stat_g = g * 255 / (63 * n); s_stat_b = b * 255 / (31 * n);
    s_stat_n = n;

    if (s_ppa_req) {
        s_ppa_req = false;
        int64_t t = esp_timer_get_time();
        esp_err_t r = scale_frame(rgb565, w, h, s_out[0]);
        ESP_LOGI(TAG, "PPA scale %dx%d -> %dx%d (mirror=%d): %s in %lld us", w, h,
                 PREVIEW_W, PREVIEW_H, s_mirror, esp_err_to_name(r), esp_timer_get_time() - t);
        if (r == ESP_OK) {
            face_box_t fb[MAX_FACES];
            int dms = 0;
            int nf = face_detect_run(s_out[0], PREVIEW_W, PREVIEW_H, fb, MAX_FACES, &dms);
            ESP_LOGI(TAG, "face detect: %d faces in %d ms", nf, dms);
            for (int i = 0; i < nf; i++) {
                ESP_LOGI(TAG, "  face %d: (%d,%d)-(%d,%d) score %.2f", i, fb[i].x0, fb[i].y0,
                         fb[i].x1, fb[i].y1, fb[i].score);
            }
        }
        if (r == ESP_OK) {
            static char l2[80 * 4 + 16];
            const uint16_t *o = (const uint16_t *)s_out[0];
            for (int ty = 0; ty < 45; ty++) {
                int pos = snprintf(l2, sizeof(l2), "CAMTHUMB2:%02d:", ty);
                for (int tx = 0; tx < 80; tx++) {
                    pos += snprintf(l2 + pos, sizeof(l2) - pos, "%04X", o[(ty * 10) * PREVIEW_W + tx * 10]);
                }
                printf("%s\n", l2);
            }
            fflush(stdout);
        }
    }

    if (s_dump_req) {
        s_dump_req = false;
        /* 80x45 thumbnail, every 16th pixel, as RGB565 hex */
        static char line[80 * 4 + 16];
        for (int ty = 0; ty < 45; ty++) {
            int pos = snprintf(line, sizeof(line), "CAMTHUMB:%02d:", ty);
            for (int tx = 0; tx < 80; tx++) {
                pos += snprintf(line + pos, sizeof(line) - pos, "%04X", px[(ty * 16) * w + tx * 16]);
            }
            printf("%s\n", line);
        }
        fflush(stdout);
    }
}

static void selftest_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGW(TAG, "===== Camera self-test =====");
    if (preview_alloc() != ESP_OK) {
        ESP_LOGE(TAG, "preview_alloc failed");
        vTaskDelete(NULL);
    }
    int64_t t0 = esp_timer_get_time();
    if (camera_start(selftest_frame_cb, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "camera_start failed");
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "start took %lld ms", (esp_timer_get_time() - t0) / 1000);

    for (int round = 1; round <= 2; round++) {
        vTaskDelay(pdMS_TO_TICKS(300));          /* let the task reset its counter */
        uint32_t f0 = camera_frame_count();
        for (int s = 1; s <= 4; s++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "round %d t=%ds frames=%lu avgRGB=(%lu,%lu,%lu)", round, s,
                     (unsigned long)(camera_frame_count() - f0),
                     (unsigned long)s_stat_r, (unsigned long)s_stat_g, (unsigned long)s_stat_b);
            if (round == 1 && s == 3) s_dump_req = true;   /* after AE/AWB settle */
            if (round == 2 && s == 2) s_ppa_req = true;    /* preview path */
        }
        ESP_LOGI(TAG, "round %d: %.1f fps", round, (camera_frame_count() - f0) / 4.0f);
        camera_stop();
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (round == 1) {
            ESP_LOGI(TAG, "restarting stream (off/on cycle test)");
            camera_start(selftest_frame_cb, NULL);
        }
    }
    /* Phase 3: the real preview on the admin camera tab for 10 s */
    ESP_LOGI(TAG, "phase 3: on-screen preview");
    if (lvgl_port_lock(0)) {
        ui_admin_debug_show_tab(4);
        preview_start();
        lvgl_port_unlock();
    }
    uint32_t sh0 = s_shown, f0 = camera_frame_count();
    for (int s = 1; s <= 10; s++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "preview t=%ds camera=%lu display=%lu frames | heap int=%u KB psram=%u KB",
                 s, (unsigned long)(camera_frame_count() - f0), (unsigned long)(s_shown - sh0),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
    if (lvgl_port_lock(0)) {
        ui_admin_close();
        lvgl_port_unlock();
    }
    ESP_LOGW(TAG, "===== Camera self-test done =====");
    vTaskDelete(NULL);
}

void ui_camera_selftest(void)
{
    xTaskCreate(selftest_task, "cam_test", 4096, NULL, 3, NULL);
}
