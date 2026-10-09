#include "ui_main.h"
#include "ui_admin.h"
#include "ui_common.h"
#include "face_service.h"
#include "report.h"
#include "attendance.h"
#include "student_db.h"
#include "settings_store.h"
#include "bsp_rtc.h"
#include "bsp_wifi.h"
#include "board_config.h"
#include "fonts/fonts.h"
#include "esp_log.h"
#include "esp_attr.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "UI_MAIN";

#define RESULT_SHOW_MS  4000

static lv_obj_t *s_scr;
static lv_obj_t *lbl_institute;
static lv_obj_t *lbl_date;
static lv_obj_t *lbl_clock;
static lv_obj_t *card;
static lv_obj_t *ring;
static lv_obj_t *cam_view;          /* live camera view (face recognition) */
static lv_obj_t *lbl_title;
static lv_obj_t *lbl_sub;
static lv_obj_t *lbl_time;
static lv_obj_t *lbl_present_title;
static lv_obj_t *present_list;
static lv_obj_t *lbl_sensor;
static lv_obj_t *lbl_sd;
static lv_obj_t *lbl_wifi;

static lv_timer_t *s_result_timer;
static bool s_sensor_ok = false;
static int s_last_minute = -1;
static int s_last_day = -1;

EXT_RAM_BSS_ATTR static att_record_t s_records[STUDENT_MAX];

static const char *const s_weekdays[] = {
    "ראשון", "שני", "שלישי", "רביעי", "חמישי", "שישי", "שבת"
};

/* ── Idle / result card ─────────────────────────────────────────────────── */

static void ring_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_border_opa(obj, v, 0);
}

static void show_idle(void)
{
    bool face = face_service_kiosk_on();
    lv_obj_set_style_bg_color(card, CLR_PANEL, 0);
    lv_obj_add_flag(lbl_time, LV_OBJ_FLAG_HIDDEN);
    if (face) {
        lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(cam_view, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cam_view, LV_OBJ_FLAG_HIDDEN);
    }
    if (face) {
        lv_label_set_text(lbl_title, s_sensor_ok ? "הביטו במצלמה או הניחו אצבע" : "הביטו במצלמה");
        lv_label_set_text(lbl_sub, "לרישום כניסה או יציאה");
    } else if (s_sensor_ok) {
        lv_label_set_text(lbl_title, "הניחו אצבע על החיישן");
        lv_label_set_text(lbl_sub, "לרישום כניסה או יציאה");
    } else {
        lv_label_set_text(lbl_title, "החיישן אינו מחובר");
        lv_label_set_text(lbl_sub, "בדקו את החיבור לחיישן טביעות האצבע");
    }
}

static void result_timer_cb(lv_timer_t *t)
{
    s_result_timer = NULL;
    show_idle();
}

static void show_result(lv_color_t bg, const char *title, const char *sub, int time_sec)
{
    lv_obj_set_style_bg_color(card, bg, 0);
    lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(cam_view, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lbl_title, title);
    lv_label_set_text(lbl_sub, sub);
    if (time_sec >= 0) {
        char buf[6];
        attendance_fmt_time(time_sec, buf);
        lv_label_set_text(lbl_time, buf);
        lv_obj_remove_flag(lbl_time, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lbl_time, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_result_timer) lv_timer_delete(s_result_timer);
    s_result_timer = lv_timer_create(result_timer_cb, RESULT_SHOW_MS, NULL);
    lv_timer_set_repeat_count(s_result_timer, 1);
}

/* ── Present list ───────────────────────────────────────────────────────── */

void ui_main_refresh_present(void)
{
    int n = attendance_get_records(s_records, STUDENT_MAX);
    int present = 0;

    lv_obj_clean(present_list);
    for (int i = 0; i < n; i++) {
        if (!s_records[i].present) continue;
        present++;

        lv_obj_t *row = ui_box(present_list, LV_PCT(100), 40);
        char t[6];
        attendance_fmt_time(s_records[i].last_in, t);
        lv_obj_t *name = ui_label(row, &g_font32, CLR_TEXT, s_records[i].name);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, 230);
        ui_align(name, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_t *tm = ui_label(row, &g_font22, CLR_MUTED, t);
        lv_obj_set_style_base_dir(tm, LV_BASE_DIR_LTR, 0);   /* RTL BiDi would show "09:24" as "24:09" */
        ui_align(tm, LV_ALIGN_LEFT_MID, 0, 0);
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "נוכחים כעת: %d", present);
    lv_label_set_text(lbl_present_title, buf);
}

/* ── Clock / status tick ────────────────────────────────────────────────── */

static void clock_timer_cb(lv_timer_t *t)
{
    if (ui_admin_is_visible()) return;   /* don't redraw a hidden screen */

    struct tm now;
    if (bsp_rtc_get_time(&now) != ESP_OK) return;

    attendance_tick(&now);

    /* Re-render the 80px clock only when the minute changes (see HebClock devlog) */
    if (now.tm_min != s_last_minute) {
        s_last_minute = now.tm_min;
        lv_label_set_text_fmt(lbl_clock, "%02d:%02d", now.tm_hour, now.tm_min);
    }
    if (now.tm_mday != s_last_day) {
        s_last_day = now.tm_mday;
        char buf[64];
        snprintf(buf, sizeof(buf), "יום %s, %02d.%02d.%04d",
                 s_weekdays[now.tm_wday % 7], now.tm_mday, now.tm_mon + 1, now.tm_year + 1900);
        lv_label_set_text(lbl_date, buf);
        ui_main_refresh_present();   /* day rollover clears the list */

        /* On the 1st, export last month's report to the SD card (once per month) */
        static int s_exported_ym = -1;
        int y = now.tm_year + 1900, m = now.tm_mon + 1;
        if (now.tm_mday == 1 && s_exported_ym != y * 100 + m) {
            s_exported_ym = y * 100 + m;
            if (--m < 1) { m = 12; y--; }
            char path[64];
            if (report_export(y, m, path, sizeof(path)) == ESP_OK) {
                ESP_LOGI(TAG, "Monthly report exported: %s", path);
            }
        }
    }

    lv_obj_set_style_text_color(lbl_wifi,
        bsp_wifi_is_connected() ? lv_color_hex(0x55DD55) : lv_color_hex(0x886666), 0);
}

/* ── Admin entry ────────────────────────────────────────────────────────── */

static void open_admin(void *user)
{
    ui_admin_show();
}

static void admin_btn_cb(lv_event_t *e)
{
    ui_pin_prompt(open_admin, NULL);
}

/* ── Build ──────────────────────────────────────────────────────────────── */

void ui_main_create(bool sd_ok)
{
    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, CLR_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_base_dir(s_scr, LV_BASE_DIR_RTL, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Header: institute + date on the right, clock on the left */
    lbl_institute = ui_label(s_scr, &g_font48, CLR_GOLD, settings_get()->institute_name);
    ui_align(lbl_institute, LV_ALIGN_TOP_RIGHT, -24, 10);

    lbl_date = ui_label(s_scr, &g_font32, CLR_MUTED, "");
    ui_align(lbl_date, LV_ALIGN_TOP_RIGHT, -24, 66);

    lbl_clock = ui_label(s_scr, &font_clock_80, CLR_TEXT, "--:--");
    lv_obj_set_style_base_dir(lbl_clock, LV_BASE_DIR_LTR, 0);
    ui_align(lbl_clock, LV_ALIGN_TOP_LEFT, 24, 12);

    /* Scan result card (right) */
    card = ui_panel(s_scr, 620, 420);
    ui_align(card, LV_ALIGN_TOP_RIGHT, -16, 120);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);

    ring = lv_obj_create(card);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 150, 150);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 10, 0);
    lv_obj_set_style_border_color(ring, CLR_GOLD, 0);
    ui_align(ring, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_t *ring_lbl = ui_label(ring, &lv_font_montserrat_32, CLR_GOLD, LV_SYMBOL_DOWN);
    lv_obj_center(ring_lbl);

    /* Live camera view for face recognition (frames supplied by face_service) */
    cam_view = lv_canvas_create(card);
    lv_obj_set_size(cam_view, FACE_VIEW_W, FACE_VIEW_H);
    lv_obj_set_style_radius(cam_view, 12, 0);
    lv_obj_set_style_clip_corner(cam_view, true, 0);
    ui_align(cam_view, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_add_flag(cam_view, LV_OBJ_FLAG_HIDDEN);
    face_service_set_kiosk_view(cam_view);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ring);
    lv_anim_set_exec_cb(&a, ring_anim_cb);
    lv_anim_set_values(&a, LV_OPA_20, LV_OPA_COVER);
    lv_anim_set_duration(&a, 1200);
    lv_anim_set_playback_duration(&a, 1200);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    lbl_title = ui_label(card, &g_font48, CLR_TEXT, "");
    lv_obj_set_width(lbl_title, LV_PCT(100));
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(lbl_title, LV_TEXT_ALIGN_CENTER, 0);
    ui_align(lbl_title, LV_ALIGN_TOP_MID, 0, 200);

    lbl_sub = ui_label(card, &g_font32, CLR_TEXT, "");
    lv_obj_set_width(lbl_sub, LV_PCT(100));
    lv_label_set_long_mode(lbl_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl_sub, LV_TEXT_ALIGN_CENTER, 0);
    ui_align(lbl_sub, LV_ALIGN_TOP_MID, 0, 270);

    lbl_time = ui_label(card, &font_clock_80, CLR_TEXT, "");
    lv_obj_set_style_base_dir(lbl_time, LV_BASE_DIR_LTR, 0);
    ui_align(lbl_time, LV_ALIGN_TOP_MID, 0, 50);

    /* Present-now list (left) */
    lv_obj_t *pp = ui_panel(s_scr, 360, 420);
    ui_align(pp, LV_ALIGN_TOP_LEFT, 16, 120);
    lv_obj_remove_flag(pp, LV_OBJ_FLAG_SCROLLABLE);

    lbl_present_title = ui_label(pp, &g_font32, CLR_GOLD, "");
    ui_align(lbl_present_title, LV_ALIGN_TOP_RIGHT, 0, 0);

    present_list = ui_box(pp, LV_PCT(100), 340);
    ui_align(present_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(present_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(present_list, LV_DIR_VER);
    lv_obj_set_flex_flow(present_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(present_list, 4, 0);

    /* Footer: sensor / SD / WiFi status, version, admin button */
    lbl_sensor = ui_label(s_scr, &g_font22, CLR_MUTED, "חיישן: מתחבר...");
    ui_align(lbl_sensor, LV_ALIGN_BOTTOM_RIGHT, -24, -14);

    lbl_sd = ui_label(s_scr, &g_font22, sd_ok ? CLR_MUTED : CLR_ERR,
                      sd_ok ? "כרטיס SD: תקין" : "אין כרטיס SD - הנתונים לא נשמרים!");
    ui_align(lbl_sd, LV_ALIGN_BOTTOM_RIGHT, -260, -14);

    lv_obj_t *btn = ui_button(s_scr, LV_SYMBOL_SETTINGS, CLR_BTN, 64, 48, admin_btn_cb, NULL);
    ui_align(btn, LV_ALIGN_BOTTOM_LEFT, 16, -4);

    lbl_wifi = ui_label(s_scr, &lv_font_montserrat_24, lv_color_hex(0x886666), LV_SYMBOL_WIFI);
    ui_align(lbl_wifi, LV_ALIGN_BOTTOM_LEFT, 100, -14);

    lv_obj_t *ver = ui_label(s_scr, &lv_font_montserrat_16, CLR_MUTED, "v" APP_VERSION);
    ui_align(ver, LV_ALIGN_BOTTOM_LEFT, 150, -18);

    show_idle();
    ui_main_refresh_present();
    clock_timer_cb(NULL);
    lv_timer_create(clock_timer_cb, 1000, NULL);
    ESP_LOGI(TAG, "Main screen ready");
}

void ui_main_show(void)
{
    lv_screen_load(s_scr);
    s_last_minute = -1;   /* clock may have been changed in admin */
    s_last_day = -1;
    show_idle();
    ui_main_reload_settings();
    clock_timer_cb(NULL);
    ui_main_refresh_present();
}

void ui_main_reload_settings(void)
{
    lv_label_set_text(lbl_institute, settings_get()->institute_name);
}

void ui_main_handle_event(const fp_event_t *e)
{
    char sub[128];

    switch (e->type) {
    case FP_EVT_SENSOR_STATUS:
        s_sensor_ok = e->ok;
        lv_label_set_text(lbl_sensor, e->ok ? "חיישן: מחובר" : "חיישן: לא מחובר");
        lv_obj_set_style_text_color(lbl_sensor, e->ok ? CLR_MUTED : CLR_ERR, 0);
        if (!s_result_timer) show_idle();
        break;

    case FP_EVT_SCAN_MATCH:
        if (e->att.repeat) {
            char prev[8], next[8];
            ui_fmt_time_in_rtl(e->att.prev_sec, prev);
            ui_fmt_time_in_rtl(e->att.next_ok_sec, next);
            if (e->att.dir == ATT_IN)
                snprintf(sub, sizeof(sub), "כניסה כבר נרשמה ב-%s\nיציאה אפשרית מ-%s", prev, next);
            else
                snprintf(sub, sizeof(sub), "יציאה כבר נרשמה ב-%s", prev);
            show_result(CLR_INFO, e->student.name, sub, e->att.time_sec);
        } else {
            char prev[8];
            ui_fmt_time_in_rtl(e->att.prev_sec, prev);
            if (e->att.dir == ATT_IN)
                snprintf(sub, sizeof(sub), "כניסה - ברוכים הבאים!%s",
                         e->att.log_ok ? "" : "\n(שגיאה בשמירה לכרטיס SD)");
            else
                snprintf(sub, sizeof(sub), "יציאה - להתראות! (נכנסת ב-%s)%s", prev,
                         e->att.log_ok ? "" : "\n(שגיאה בשמירה לכרטיס SD)");
            show_result(e->att.dir == ATT_IN ? CLR_IN : CLR_OUT, e->student.name, sub,
                        e->att.time_sec);
            ui_main_refresh_present();
        }
        break;

    case FP_EVT_SCAN_UNKNOWN:
        if (e->source == FP_SRC_FACE)
            show_result(CLR_ERR, "הפנים לא זוהו", "התקרבו והביטו ישר במצלמה, או פנו למנהל", -1);
        else
            show_result(CLR_ERR, "טביעת אצבע לא מזוהה", "נסו שוב, או פנו למנהל לרישום", -1);
        break;

    case FP_EVT_SCAN_BAD_IMAGE:
        show_result(CLR_ERR, "הסריקה לא נקלטה", "הניחו את האצבע במלואה ונסו שוב", -1);
        break;

    default:
        break;
    }
}
