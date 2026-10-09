#include "ui_admin.h"
#include "ui_main.h"
#include "ui_common.h"
#include "ui_camera.h"
#include "face_service.h"
#include "face_db.h"
#include "report.h"
#include "esp_attr.h"
#include "student_db.h"
#include "attendance.h"
#include "settings_store.h"
#include "bsp_rtc.h"
#include "bsp_wifi.h"
#include "bsp_audio.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "driver/uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

static const char *TAG = "UI_ADMIN";

#define INACTIVITY_CLOSE_MS  (3 * 60 * 1000)
#define TAB_CAMERA           4
#define ID_FINGER(id, f)     ((void *)(uintptr_t)(((id) << 1) | (f)))
#define GET_ID(p)            ((uint16_t)((uintptr_t)(p) >> 1))
#define GET_FINGER(p)        ((int)((uintptr_t)(p) & 1))

static lv_obj_t *s_scr;
static lv_obj_t *s_tv;
static lv_obj_t *s_kb;
static bool s_visible;

/* Students tab */
static lv_obj_t *stu_list;
static lv_obj_t *stu_count_lbl;

/* Report tab */
static lv_obj_t *rep_table;
static lv_obj_t *rep_summary;
EXT_RAM_BSS_ATTR static att_record_t s_records[STUDENT_MAX];

/* Settings tab */
static lv_obj_t *ta_inst, *ta_pin, *ta_ssid, *ta_pass;
static lv_obj_t *sw_dst, *sw_ntp, *sw_face;
static lv_obj_t *dd_tz, *dd_gap, *sl_vol;
static lv_obj_t *dd_day, *dd_mon, *dd_year, *dd_hour, *dd_min;

/* Sensor tab */
static lv_obj_t *sen_status;
static lv_obj_t *sen_detail;

/* Enrollment dialog */
static bool s_enrolling;
static lv_obj_t *enr_main, *enr_sub, *enr_btn_lbl;

/* Name editor */
static uint16_t s_edit_id;      /* 0 = new student */
static lv_obj_t *s_name_ta;

static const int s_gap_values[] = { 60, 5 * 60, 10 * 60, 15 * 60, 30 * 60, 60 * 60, 120 * 60 };
#define TZ_MIN  (-12)
#define YEAR_MIN 2025

static void students_refresh(void);
static void sd_face_cb(lv_event_t *e);
static void report_refresh(void);
static void enroll_open(uint16_t id, int finger);
static void student_dialog_open(uint16_t id);

/* ══ Helpers ═══════════════════════════════════════════════════════════════ */

static lv_obj_t *tab_page_setup(lv_obj_t *page)
{
    lv_obj_set_style_bg_color(page, CLR_BG, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page, 16, 0);
    return page;
}

static lv_obj_t *row_box(lv_obj_t *parent, int h)
{
    lv_obj_t *r = ui_box(parent, LV_PCT(100), h);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 16, 0);
    return r;
}

/* Settings row: fixed-width caption on the right, widget(s) after it. */
static lv_obj_t *setting_row(lv_obj_t *parent, const char *caption)
{
    lv_obj_t *r = row_box(parent, 64);
    lv_obj_t *l = ui_label(r, &g_font32, CLR_TEXT, caption);
    lv_obj_set_width(l, 260);
    return r;
}

static void style_dropdown(lv_obj_t *dd, int w)
{
    lv_obj_set_width(dd, w);
    lv_obj_set_style_text_font(dd, &g_font22, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dd), &g_font22, 0);
    lv_obj_set_style_bg_color(dd, CLR_PANEL_HI, 0);
    lv_obj_set_style_text_color(dd, CLR_TEXT, 0);
}

static lv_obj_t *number_dropdown(lv_obj_t *parent, int from, int to, const char *fmt, int w)
{
    static char opts[512];
    int pos = 0;
    for (int v = from; v <= to && pos < (int)sizeof(opts) - 8; v++) {
        pos += snprintf(opts + pos, sizeof(opts) - pos, fmt, v);
        if (v < to) opts[pos++] = '\n';
    }
    opts[pos] = '\0';
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, opts);
    style_dropdown(dd, w);
    return dd;
}

static void hide_keyboard(void)
{
    if (s_kb) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_kb, NULL);
    }
}

/* ══ Enrollment dialog ═════════════════════════════════════════════════════ */

static void enroll_btn_cb(lv_event_t *e)
{
    if (s_enrolling) {
        fp_service_cancel();
        s_enrolling = false;
    }
    ui_modal_close();
    students_refresh();
}

static void enroll_open(uint16_t id, int finger)
{
    student_t s;
    if (!student_db_get(id, &s)) return;

    lv_obj_t *card = ui_modal_open(760, 440);
    char buf[128];
    snprintf(buf, sizeof(buf), "רישום אצבע %d - %s", finger + 1, s.name);
    lv_obj_t *t = ui_label(card, &g_font32, CLR_GOLD, buf);
    ui_align(t, LV_ALIGN_TOP_MID, 0, 0);

    enr_main = ui_label(card, &g_font48, CLR_TEXT, "מתחבר לחיישן...");
    lv_obj_set_width(enr_main, LV_PCT(100));
    lv_obj_set_style_text_align(enr_main, LV_TEXT_ALIGN_CENTER, 0);
    ui_align(enr_main, LV_ALIGN_TOP_MID, 0, 90);

    enr_sub = ui_label(card, &g_font32, CLR_MUTED, "");
    lv_obj_set_width(enr_sub, LV_PCT(100));
    lv_label_set_long_mode(enr_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(enr_sub, LV_TEXT_ALIGN_CENTER, 0);
    ui_align(enr_sub, LV_ALIGN_TOP_MID, 0, 170);

    lv_obj_t *b = ui_button(card, "ביטול", CLR_BTN, 260, 72, enroll_btn_cb, NULL);
    ui_align(b, LV_ALIGN_BOTTOM_MID, 0, 0);
    enr_btn_lbl = lv_obj_get_child(b, 0);

    s_enrolling = true;
    fp_service_enroll(id, finger);
}

static void enroll_handle_event(const fp_event_t *e)
{
    if (!s_enrolling || !ui_modal_is_open()) return;

    if (e->type == FP_EVT_ENROLL_STEP) {
        switch (e->step) {
        case 1: {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s (%d/3)",
                     e->ok ? (e->count == 1 ? "הניחו אצבע על החיישן" : "הניחו שוב את אותה האצבע")
                           : "לא נקלט היטב - נסו שוב", e->count);
            lv_label_set_text(enr_main, buf);
            lv_label_set_text(enr_sub, e->count == 1 ? "לחצו בעדינות עם כל כרית האצבע"
                                                     : "בזווית מעט שונה");
            break;
        }
        case 2:
            lv_label_set_text(enr_main, "הרימו את האצבע");
            lv_label_set_text(enr_sub, "");
            break;
        case 4:
            lv_label_set_text(enr_main, "שומר...");
            lv_label_set_text(enr_sub, "");
            break;
        }
        return;
    }

    if (e->type != FP_EVT_ENROLL_DONE) return;
    s_enrolling = false;
    lv_label_set_text(enr_btn_lbl, "סגור");
    lv_obj_set_style_text_color(enr_main, e->ok ? lv_color_hex(0x55DD55) : CLR_ERR, 0);

    char buf[128];
    switch (e->enroll_result) {
    case FP_ENROLL_OK:
        lv_label_set_text(enr_main, "האצבע נרשמה בהצלחה!");
        lv_label_set_text(enr_sub, "מומלץ לרשום גם אצבע שנייה");
        break;
    case FP_ENROLL_DUPLICATE:
        lv_label_set_text(enr_main, "האצבע כבר רשומה");
        snprintf(buf, sizeof(buf), "שייכת ל: %s", e->student.name);
        lv_label_set_text(enr_sub, buf);
        break;
    case FP_ENROLL_MISMATCH:
        lv_label_set_text(enr_main, "הסריקות לא תאמו");
        lv_label_set_text(enr_sub, "יש להניח את אותה האצבע בשתי הסריקות. נסו שוב.");
        break;
    case FP_ENROLL_TIMEOUT:
        lv_label_set_text(enr_main, "תם הזמן");
        lv_label_set_text(enr_sub, "לא הונחה אצבע. נסו שוב.");
        break;
    case FP_ENROLL_FULL:
        lv_label_set_text(enr_main, "זיכרון החיישן מלא");
        lv_label_set_text(enr_sub, "מחקו תלמידים ישנים או נקו תבניות יתומות");
        break;
    case FP_ENROLL_CANCELLED:
        lv_label_set_text(enr_main, "בוטל");
        lv_label_set_text(enr_sub, "");
        break;
    default:
        lv_label_set_text(enr_main, "שגיאת חיישן");
        lv_label_set_text(enr_sub, "בדקו את חיבור החיישן ונסו שוב");
        break;
    }
    students_refresh();
}

/* ══ Name editor ═══════════════════════════════════════════════════════════ */

static void name_save(void)
{
    char name[STUDENT_NAME_MAX];
    strlcpy(name, lv_textarea_get_text(s_name_ta), sizeof(name));

    /* trim */
    char *p = name;
    while (*p == ' ') p++;
    char *end = p + strlen(p);
    while (end > p && end[-1] == ' ') *--end = '\0';
    if (*p == '\0') {
        ui_toast("יש להזין שם", CLR_ERR);
        return;
    }

    uint16_t id = s_edit_id;
    if (id == 0) {
        esp_err_t ret = student_db_add(p, &id);
        if (ret == ESP_ERR_NO_MEM) { ui_toast("הגעתם למספר התלמידים המרבי", CLR_ERR); return; }
        if (ret != ESP_OK)         ui_toast("שגיאה בשמירה לכרטיס SD", CLR_ERR);
        students_refresh();
        enroll_open(id, 0);   /* replaces this modal */
    } else {
        if (student_db_rename(id, p) != ESP_OK) ui_toast("שגיאה בשמירה", CLR_ERR);
        ui_modal_close();
        students_refresh();
    }
}

static void name_save_cb(lv_event_t *e)   { name_save(); }
static void name_cancel_cb(lv_event_t *e) { ui_modal_close(); }

static void name_editor_open(uint16_t id, const char *initial)
{
    s_edit_id = id;
    lv_obj_t *card = ui_modal_open(1000, 580);

    lv_obj_t *t = ui_label(card, &g_font32, CLR_GOLD, id ? "שינוי שם" : "תלמיד חדש - שם מלא");
    ui_align(t, LV_ALIGN_TOP_RIGHT, 0, 6);

    s_name_ta = lv_textarea_create(card);
    lv_textarea_set_one_line(s_name_ta, true);
    lv_textarea_set_max_length(s_name_ta, STUDENT_NAME_MAX / 2);  /* Hebrew = 2 bytes/char */
    lv_obj_set_width(s_name_ta, 560);
    lv_obj_set_style_text_font(s_name_ta, &g_font32, 0);
    lv_textarea_set_text(s_name_ta, initial ? initial : "");
    ui_align(s_name_ta, LV_ALIGN_TOP_RIGHT, 0, 56);

    lv_obj_t *save = ui_button(card, "שמירה", CLR_IN, 160, 60, name_save_cb, NULL);
    ui_align(save, LV_ALIGN_TOP_LEFT, 180, 56);
    lv_obj_t *cancel = ui_button(card, "ביטול", CLR_BTN, 160, 60, name_cancel_cb, NULL);
    ui_align(cancel, LV_ALIGN_TOP_LEFT, 0, 56);

    lv_obj_t *kb = ui_keyboard_create(card);
    ui_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(kb, name_save_cb, LV_EVENT_READY, NULL);
    ui_keyboard_attach(kb, s_name_ta, true);
    lv_obj_add_state(s_name_ta, LV_STATE_FOCUSED);
}

/* ══ Student dialog ════════════════════════════════════════════════════════ */

static void sd_enroll_cb(lv_event_t *e)
{
    void *u = lv_event_get_user_data(e);
    enroll_open(GET_ID(u), GET_FINGER(u));
}

static void sd_rename_cb(lv_event_t *e)
{
    uint16_t id = GET_ID(lv_event_get_user_data(e));
    student_t s;
    if (student_db_get(id, &s)) name_editor_open(id, s.name);
}

static void do_delete_student(void *user)
{
    uint16_t id = GET_ID(user);
    int slots[STUDENT_FINGERS];
    if (student_db_remove(id, slots) == ESP_OK) {
        for (int k = 0; k < STUDENT_FINGERS; k++) fp_service_delete_slot(slots[k]);
        face_db_remove_student(id);
        ui_toast("התלמיד נמחק", CLR_INFO);
    }
    students_refresh();
}

static void sd_delete_cb(lv_event_t *e)
{
    void *u = lv_event_get_user_data(e);
    student_t s;
    if (!student_db_get(GET_ID(u), &s)) return;
    char buf[192];
    snprintf(buf, sizeof(buf), "למחוק את %s?\nטביעות האצבע שלו יימחקו מהחיישן.", s.name);
    ui_confirm(buf, "מחיקה", CLR_ERR, do_delete_student, u);
}

static void sd_close_cb(lv_event_t *e) { ui_modal_close(); }

static void student_dialog_open(uint16_t id)
{
    student_t s;
    if (!student_db_get(id, &s)) return;

    lv_obj_t *card = ui_modal_open(760, 540);
    lv_obj_t *t = ui_label(card, &g_font48, CLR_GOLD, s.name);
    ui_align(t, LV_ALIGN_TOP_MID, 0, 0);

    char buf[64];
    snprintf(buf, sizeof(buf), "מספר תלמיד: %u", s.id);
    lv_obj_t *idl = ui_label(card, &g_font22, CLR_MUTED, buf);
    ui_align(idl, LV_ALIGN_TOP_MID, 0, 60);

    lv_obj_t *grid = ui_box(card, LV_PCT(100), 350);
    ui_align(grid, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(grid, 14, 0);
    lv_obj_set_style_pad_column(grid, 14, 0);

    for (int f = 0; f < STUDENT_FINGERS; f++) {
        bool has = s.finger[f] != STUDENT_NO_SLOT;
        snprintf(buf, sizeof(buf), "%s אצבע %d", has ? "החלפת" : "רישום", f + 1);
        ui_button(grid, buf, has ? CLR_BTN : CLR_IN, 330, 72, sd_enroll_cb, ID_FINGER(id, f));
    }
    bool has_face = face_db_count_for(id) > 0;
    ui_button(grid, has_face ? "החלפת פנים" : "רישום פנים", has_face ? CLR_BTN : CLR_IN, 330, 72,
              sd_face_cb, ID_FINGER(id, 0));
    ui_button(grid, "שינוי שם", CLR_BTN, 330, 72, sd_rename_cb, ID_FINGER(id, 0));
    ui_button(grid, "מחיקת תלמיד", CLR_ERR, 330, 72, sd_delete_cb, ID_FINGER(id, 0));
    ui_button(grid, "סגור", CLR_PANEL, 674, 60, sd_close_cb, NULL);
}

/* ══ Face enrollment dialog ═══════════════════════════════════════════════ */

static bool s_face_enrolling;
static lv_obj_t *fe_count, *fe_hint, *fe_btn_lbl;

static void face_enroll_btn_cb(lv_event_t *e)
{
    face_service_enroll_cancel();      /* detaches the canvas before the modal goes */
    s_face_enrolling = false;
    ui_modal_close();
    students_refresh();
}

static void face_enroll_open(uint16_t id)
{
    student_t s;
    if (!student_db_get(id, &s)) return;

    lv_obj_t *card = ui_modal_open(760, 520);
    char buf[128];
    snprintf(buf, sizeof(buf), "רישום פנים - %s", s.name);
    lv_obj_t *t = ui_label(card, &g_font32, CLR_GOLD, buf);
    ui_align(t, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *view = lv_canvas_create(card);
    lv_obj_set_size(view, FACE_VIEW_W, FACE_VIEW_H);
    lv_obj_set_style_radius(view, 12, 0);
    lv_obj_set_style_clip_corner(view, true, 0);
    lv_obj_set_style_bg_color(view, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(view, LV_OPA_COVER, 0);
    ui_align(view, LV_ALIGN_TOP_MID, 0, 50);

    fe_count = ui_label(card, &g_font48, CLR_TEXT, "0 / 5");
    ui_align(fe_count, LV_ALIGN_TOP_MID, 0, 245);
    fe_hint = ui_label(card, &g_font32, CLR_MUTED, "מפעיל מצלמה...");
    lv_obj_set_width(fe_hint, LV_PCT(100));
    lv_obj_set_style_text_align(fe_hint, LV_TEXT_ALIGN_CENTER, 0);
    ui_align(fe_hint, LV_ALIGN_TOP_MID, 0, 310);

    lv_obj_t *b = ui_button(card, "ביטול", CLR_BTN, 260, 64, face_enroll_btn_cb, NULL);
    ui_align(b, LV_ALIGN_BOTTOM_MID, 0, 0);
    fe_btn_lbl = lv_obj_get_child(b, 0);

    if (face_service_enroll_start(id, view) != ESP_OK) {
        lv_label_set_text(fe_hint, "שגיאה בהפעלת המצלמה");
        lv_label_set_text(fe_btn_lbl, "סגור");
        return;
    }
    s_face_enrolling = true;
}

void ui_admin_handle_face_enroll(const face_enroll_evt_t *e)
{
    if (!s_face_enrolling || !ui_modal_is_open()) return;

    if (e->type == FACE_ENROLL_PROGRESS) {
        lv_label_set_text_fmt(fe_count, "%d / %d", e->count, e->total);
        lv_label_set_text(fe_hint, e->hint ? e->hint : "");
        return;
    }

    s_face_enrolling = false;
    lv_label_set_text(fe_btn_lbl, "סגור");
    lv_obj_set_style_text_color(fe_hint, e->result == FACE_ENROLL_OK ? lv_color_hex(0x55DD55) : CLR_ERR, 0);
    char buf[128];
    switch (e->result) {
    case FACE_ENROLL_OK:
        lv_label_set_text_fmt(fe_count, "%d / %d", e->total, e->total);
        lv_label_set_text(fe_hint, "הפנים נרשמו בהצלחה!");
        break;
    case FACE_ENROLL_DUPLICATE:
        snprintf(buf, sizeof(buf), "הפנים כבר רשומות ל: %s", e->dup.name);
        lv_label_set_text(fe_hint, buf);
        break;
    case FACE_ENROLL_TIMEOUT:
        lv_label_set_text(fe_hint, "תם הזמן - נסו שוב");
        break;
    default:
        lv_label_set_text(fe_hint, "שגיאה ברישום הפנים");
        break;
    }
    students_refresh();
}

static void sd_face_cb(lv_event_t *e)
{
    face_enroll_open(GET_ID(lv_event_get_user_data(e)));
}

/* ══ Students tab ══════════════════════════════════════════════════════════ */

static void stu_row_cb(lv_event_t *e)
{
    student_dialog_open(GET_ID(lv_event_get_user_data(e)));
}

static void stu_add_cb(lv_event_t *e)
{
    name_editor_open(0, "");
}

static void students_refresh(void)
{
    if (!stu_list) return;
    lv_obj_clean(stu_list);

    int n = student_db_count();
    int missing = 0;
    for (int i = 0; i < n; i++) {
        student_t s;
        if (!student_db_get_at(i, &s)) break;
        int fingers = (s.finger[0] != STUDENT_NO_SLOT) + (s.finger[1] != STUDENT_NO_SLOT);
        bool face = face_db_count_for(s.id) > 0;
        if (fingers == 0 && !face) missing++;

        lv_obj_t *row = ui_panel(stu_list, LV_PCT(100), 64);
        lv_obj_set_style_pad_ver(row, 6, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, CLR_PANEL_HI, LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, stu_row_cb, LV_EVENT_CLICKED, ID_FINGER(s.id, 0));

        lv_obj_t *name = ui_label(row, &g_font32, CLR_TEXT, s.name);
        ui_align(name, LV_ALIGN_RIGHT_MID, 0, 0);

        char buf[64];
        snprintf(buf, sizeof(buf), "אצבעות: %d | פנים: %s", fingers, face ? "כן" : "לא");
        lv_obj_t *st = ui_label(row, &g_font22, (fingers || face) ? CLR_MUTED : CLR_OUT, buf);
        ui_align(st, LV_ALIGN_LEFT_MID, 0, 0);
    }

    char buf[96];
    if (missing) snprintf(buf, sizeof(buf), "סה\"כ %d תלמידים (%d לא רשומים)", n, missing);
    else         snprintf(buf, sizeof(buf), "סה\"כ %d תלמידים", n);
    lv_label_set_text(stu_count_lbl, buf);
}

static void build_students_tab(lv_obj_t *page)
{
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = row_box(page, 72);
    ui_button(top, LV_SYMBOL_PLUS " הוספת תלמיד", CLR_IN, 280, 64, stu_add_cb, NULL);
    stu_count_lbl = ui_label(top, &g_font32, CLR_MUTED, "");

    stu_list = ui_box(page, LV_PCT(100), 420);
    ui_align(stu_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(stu_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(stu_list, LV_DIR_VER);
    lv_obj_set_flex_flow(stu_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(stu_list, 8, 0);
}

/* ══ Report tab ════════════════════════════════════════════════════════════ */

/* The table is laid out LTR with the columns in reverse order (it still reads
 * right-to-left on screen): in an RTL table LVGL's BiDi turns "09:24" into
 * "24:09", because it treats ':' as a neutral separator between two numbers. */
#define COL_NAME   3
#define COL_IN     2
#define COL_OUT    1
#define COL_STATE  0

static void report_refresh(void)
{
    if (!rep_table) return;
    int n = attendance_get_records(s_records, STUDENT_MAX);
    int total = student_db_count();
    int present = 0;

    lv_table_set_row_count(rep_table, 1);
    int row = 1;
    char t1[6], t2[6];

    for (int i = 0; i < n; i++) {
        att_record_t *r = &s_records[i];
        if (r->present) present++;
        attendance_fmt_time(r->first_in, t1);
        attendance_fmt_time(r->present ? ATT_NO_TIME : r->last_out, t2);
        lv_table_set_cell_value(rep_table, row, COL_NAME, r->name);
        lv_table_set_cell_value(rep_table, row, COL_IN, t1);
        lv_table_set_cell_value(rep_table, row, COL_OUT, t2);
        lv_table_set_cell_value(rep_table, row, COL_STATE, r->present ? "בפנים" : "יצא");
        row++;
    }

    /* Students not seen today */
    for (int i = 0; i < total; i++) {
        student_t s;
        if (!student_db_get_at(i, &s)) break;
        bool seen = false;
        for (int k = 0; k < n && !seen; k++) seen = (s_records[k].id == s.id);
        if (seen) continue;
        lv_table_set_cell_value(rep_table, row, COL_NAME, s.name);
        lv_table_set_cell_value(rep_table, row, COL_IN, "--:--");
        lv_table_set_cell_value(rep_table, row, COL_OUT, "--:--");
        lv_table_set_cell_value(rep_table, row, COL_STATE, "לא הגיע");
        row++;
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "נוכחים: %d    הגיעו היום: %d    סה\"כ תלמידים: %d",
             present, n, total);
    lv_label_set_text(rep_summary, buf);
}

static void rep_refresh_cb(lv_event_t *e) { report_refresh(); }

static void do_mark_all_out(void *user)
{
    int n = attendance_mark_all_out();
    char buf[64];
    snprintf(buf, sizeof(buf), "סומנה יציאה ל-%d תלמידים", n);
    ui_toast(buf, CLR_INFO);
    report_refresh();
}

static void rep_all_out_cb(lv_event_t *e)
{
    ui_confirm("לסמן יציאה לכל מי שעדיין בפנים?", "סימון יציאה", CLR_OUT, do_mark_all_out, NULL);
}

static void rep_monthly_cb(lv_event_t *e);

static void build_report_tab(lv_obj_t *page)
{
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = row_box(page, 72);
    ui_button(top, LV_SYMBOL_REFRESH " רענון", CLR_BTN, 170, 60, rep_refresh_cb, NULL);
    ui_button(top, "סימון יציאה לכולם", CLR_OUT, 260, 60, rep_all_out_cb, NULL);
    ui_button(top, "דוח חודשי", CLR_BTN, 170, 60, rep_monthly_cb, NULL);
    rep_summary = ui_label(top, &g_font22, CLR_MUTED, "");

    lv_obj_t *wrap = ui_box(page, LV_PCT(100), 420);
    ui_align(wrap, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

    rep_table = lv_table_create(wrap);
    ui_align(rep_table, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_base_dir(rep_table, LV_BASE_DIR_LTR, 0);
    lv_obj_set_style_text_align(rep_table, LV_TEXT_ALIGN_RIGHT, LV_PART_ITEMS);
    lv_table_set_column_count(rep_table, 4);
    lv_table_set_column_width(rep_table, COL_NAME, 420);
    lv_table_set_column_width(rep_table, COL_IN, 170);
    lv_table_set_column_width(rep_table, COL_OUT, 170);
    lv_table_set_column_width(rep_table, COL_STATE, 180);
    lv_obj_set_style_text_font(rep_table, &g_font22, LV_PART_ITEMS);
    lv_obj_set_style_text_color(rep_table, CLR_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(rep_table, CLR_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_border_color(rep_table, CLR_DIVIDER, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(rep_table, 8, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(rep_table, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rep_table, 0, 0);
    lv_table_set_cell_value(rep_table, 0, COL_NAME, "שם");
    lv_table_set_cell_value(rep_table, 0, COL_IN, "כניסה ראשונה");
    lv_table_set_cell_value(rep_table, 0, COL_OUT, "יציאה");
    lv_table_set_cell_value(rep_table, 0, COL_STATE, "מצב");
}

/* ══ Monthly report dialog ════════════════════════════════════════════════ */

/* LTR table, columns in reverse order (times must not be BiDi-reordered) */
#define MCOL_NAME     4
#define MCOL_DAYS     3
#define MCOL_HOURS    2
#define MCOL_ARRIVE   1
#define MCOL_MISSING  0

EXT_RAM_BSS_ATTR static report_row_t s_month_rows[STUDENT_MAX + 100];
static int s_rep_year, s_rep_month;
static lv_obj_t *mr_title, *mr_table, *mr_summary;

static void monthly_refresh(void)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "דוח נוכחות - %s %d", report_month_name(s_rep_month), s_rep_year);
    lv_label_set_text(mr_title, buf);

    int n = report_build(s_rep_year, s_rep_month, s_month_rows, STUDENT_MAX + 100);
    lv_table_set_row_count(mr_table, n + 1);
    int active = 0;
    char t[16];
    for (int i = 0; i < n; i++) {
        const report_row_t *r = &s_month_rows[i];
        if (r->days_present) active++;
        lv_table_set_cell_value(mr_table, i + 1, MCOL_NAME, r->name);
        lv_table_set_cell_value_fmt(mr_table, i + 1, MCOL_DAYS, "%d", r->days_present);
        snprintf(t, sizeof(t), "%02d:%02d", r->total_min / 60, r->total_min % 60);
        lv_table_set_cell_value(mr_table, i + 1, MCOL_HOURS, r->days_present ? t : "-");
        if (r->avg_arrival_min >= 0) {
            snprintf(t, sizeof(t), "%02d:%02d", r->avg_arrival_min / 60, r->avg_arrival_min % 60);
            lv_table_set_cell_value(mr_table, i + 1, MCOL_ARRIVE, t);
        } else {
            lv_table_set_cell_value(mr_table, i + 1, MCOL_ARRIVE, "-");
        }
        lv_table_set_cell_value_fmt(mr_table, i + 1, MCOL_MISSING, "%d", r->missing_exits);
    }
    snprintf(buf, sizeof(buf), "%d תלמידים, %d הגיעו לפחות פעם אחת", n, active);
    lv_label_set_text(mr_summary, buf);
}

static void monthly_prev_cb(lv_event_t *e)
{
    if (--s_rep_month < 1) { s_rep_month = 12; s_rep_year--; }
    monthly_refresh();
}

static void monthly_next_cb(lv_event_t *e)
{
    if (++s_rep_month > 12) { s_rep_month = 1; s_rep_year++; }
    monthly_refresh();
}

static void monthly_export_cb(lv_event_t *e)
{
    char path[64], msg[96];
    if (report_export(s_rep_year, s_rep_month, path, sizeof(path)) == ESP_OK) {
        snprintf(msg, sizeof(msg), "נשמר בכרטיס: %s", path + 4);   /* drop "/sd/" */
        ui_toast(msg, CLR_IN);
    } else {
        ui_toast("שגיאה בשמירת הדוח", CLR_ERR);
    }
}

static void monthly_close_cb(lv_event_t *e) { ui_modal_close(); }

static void monthly_open(void)
{
    struct tm now = {0};
    bsp_rtc_get_time(&now);
    s_rep_year = now.tm_year + 1900;
    s_rep_month = now.tm_mon + 1;

    lv_obj_t *card = ui_modal_open(1000, 580);

    mr_title = ui_label(card, &g_font32, CLR_GOLD, "");
    ui_align(mr_title, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_t *prev = ui_button(card, "חודש קודם", CLR_BTN, 170, 52, monthly_prev_cb, NULL);
    ui_align(prev, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t *next = ui_button(card, "חודש הבא", CLR_BTN, 170, 52, monthly_next_cb, NULL);
    ui_align(next, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *wrap = ui_box(card, LV_PCT(100), 380);
    ui_align(wrap, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_add_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(wrap, LV_DIR_VER);

    mr_table = lv_table_create(wrap);
    ui_align(mr_table, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_base_dir(mr_table, LV_BASE_DIR_LTR, 0);
    lv_obj_set_style_text_align(mr_table, LV_TEXT_ALIGN_RIGHT, LV_PART_ITEMS);
    lv_table_set_column_count(mr_table, 5);
    lv_table_set_column_width(mr_table, MCOL_NAME, 330);
    lv_table_set_column_width(mr_table, MCOL_DAYS, 140);
    lv_table_set_column_width(mr_table, MCOL_HOURS, 150);
    lv_table_set_column_width(mr_table, MCOL_ARRIVE, 150);
    lv_table_set_column_width(mr_table, MCOL_MISSING, 160);
    lv_obj_set_style_text_font(mr_table, &g_font22, LV_PART_ITEMS);
    lv_obj_set_style_text_color(mr_table, CLR_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(mr_table, CLR_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_border_color(mr_table, CLR_DIVIDER, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(mr_table, 6, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(mr_table, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mr_table, 0, 0);
    lv_table_set_cell_value(mr_table, 0, MCOL_NAME, "שם");
    lv_table_set_cell_value(mr_table, 0, MCOL_DAYS, "ימי נוכחות");
    lv_table_set_cell_value(mr_table, 0, MCOL_HOURS, "סה\"כ שעות");
    lv_table_set_cell_value(mr_table, 0, MCOL_ARRIVE, "הגעה ממוצעת");
    lv_table_set_cell_value(mr_table, 0, MCOL_MISSING, "ללא יציאה");

    mr_summary = ui_label(card, &g_font22, CLR_MUTED, "");
    ui_align(mr_summary, LV_ALIGN_BOTTOM_RIGHT, 0, -14);

    lv_obj_t *exp = ui_button(card, LV_SYMBOL_SAVE " יצוא לכרטיס SD", CLR_IN, 260, 60, monthly_export_cb, NULL);
    ui_align(exp, LV_ALIGN_BOTTOM_LEFT, 180, 0);
    lv_obj_t *close = ui_button(card, "סגור", CLR_BTN, 160, 60, monthly_close_cb, NULL);
    ui_align(close, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    monthly_refresh();
}

static void rep_monthly_cb(lv_event_t *e) { monthly_open(); }

/* ══ Settings tab ══════════════════════════════════════════════════════════ */

static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target_obj(e);
    bool hebrew = (ta == ta_inst);
    ui_keyboard_attach(s_kb, ta, hebrew);
    if (ta == ta_pin) lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_USER_3);
    lv_obj_scroll_to_view_recursive(ta, LV_ANIM_ON);
}

static void kb_done_cb(lv_event_t *e)
{
    hide_keyboard();
}

static lv_obj_t *settings_textarea(lv_obj_t *row, int max_len, bool password)
{
    lv_obj_t *ta = lv_textarea_create(row);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_password_mode(ta, password);
    lv_obj_set_width(ta, 480);
    lv_obj_set_style_text_font(ta, &g_font32, 0);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    return ta;
}

static void settings_load_widgets(void)
{
    const app_settings_t *s = settings_get();
    lv_textarea_set_text(ta_inst, s->institute_name);
    lv_textarea_set_text(ta_pin, s->admin_pin);
    lv_textarea_set_text(ta_ssid, s->wifi_ssid);
    lv_textarea_set_text(ta_pass, s->wifi_password);
    if (s->summer_time) lv_obj_add_state(sw_dst, LV_STATE_CHECKED); else lv_obj_remove_state(sw_dst, LV_STATE_CHECKED);
    if (s->auto_ntp)    lv_obj_add_state(sw_ntp, LV_STATE_CHECKED); else lv_obj_remove_state(sw_ntp, LV_STATE_CHECKED);
    if (s->face_enabled) lv_obj_add_state(sw_face, LV_STATE_CHECKED); else lv_obj_remove_state(sw_face, LV_STATE_CHECKED);
    lv_dropdown_set_selected(dd_tz, s->timezone - TZ_MIN);
    int gi = 4;   /* 30 min */
    for (int i = 0; i < (int)(sizeof(s_gap_values) / sizeof(s_gap_values[0])); i++) {
        if (s_gap_values[i] == s->min_scan_gap_s) gi = i;
    }
    lv_dropdown_set_selected(dd_gap, gi);
    lv_slider_set_value(sl_vol, s->volume, LV_ANIM_OFF);

    struct tm now;
    if (bsp_rtc_get_time(&now) == ESP_OK) {
        lv_dropdown_set_selected(dd_day, now.tm_mday - 1);
        lv_dropdown_set_selected(dd_mon, now.tm_mon);
        int y = now.tm_year + 1900 - YEAR_MIN;
        lv_dropdown_set_selected(dd_year, y < 0 ? 0 : y);
        lv_dropdown_set_selected(dd_hour, now.tm_hour);
        lv_dropdown_set_selected(dd_min, now.tm_min);
    }
}

static bool valid_pin(const char *p)
{
    size_t n = strlen(p);
    if (n < 4 || n > 8) return false;
    for (size_t i = 0; i < n; i++) if (!isdigit((unsigned char)p[i])) return false;
    return true;
}

static void settings_save_cb(lv_event_t *e)
{
    hide_keyboard();
    app_settings_t *s = settings_get();

    const char *pin = lv_textarea_get_text(ta_pin);
    if (!valid_pin(pin)) {
        ui_toast("קוד מנהל: 4-8 ספרות בלבד", CLR_ERR);
        return;
    }

    bool wifi_changed = strcmp(s->wifi_ssid, lv_textarea_get_text(ta_ssid)) != 0 ||
                        strcmp(s->wifi_password, lv_textarea_get_text(ta_pass)) != 0;

    strlcpy(s->institute_name, lv_textarea_get_text(ta_inst), sizeof(s->institute_name));
    strlcpy(s->admin_pin, pin, sizeof(s->admin_pin));
    strlcpy(s->wifi_ssid, lv_textarea_get_text(ta_ssid), sizeof(s->wifi_ssid));
    strlcpy(s->wifi_password, lv_textarea_get_text(ta_pass), sizeof(s->wifi_password));
    s->summer_time = lv_obj_has_state(sw_dst, LV_STATE_CHECKED);
    s->auto_ntp = lv_obj_has_state(sw_ntp, LV_STATE_CHECKED);
    s->face_enabled = lv_obj_has_state(sw_face, LV_STATE_CHECKED);
    s->timezone = (int)lv_dropdown_get_selected(dd_tz) + TZ_MIN;
    s->min_scan_gap_s = s_gap_values[lv_dropdown_get_selected(dd_gap)];
    s->volume = lv_slider_get_value(sl_vol);

    settings_save(s);
    bsp_audio_set_volume(s->volume);
    if (wifi_changed && s->wifi_ssid[0]) bsp_wifi_reconnect(s->wifi_ssid, s->wifi_password);
    ui_main_reload_settings();
    ui_toast("ההגדרות נשמרו", CLR_IN);
}

static void clock_set_cb(lv_event_t *e)
{
    struct tm t = {0};
    t.tm_mday = lv_dropdown_get_selected(dd_day) + 1;
    t.tm_mon  = lv_dropdown_get_selected(dd_mon);
    t.tm_year = lv_dropdown_get_selected(dd_year) + YEAR_MIN - 1900;
    t.tm_hour = lv_dropdown_get_selected(dd_hour);
    t.tm_min  = lv_dropdown_get_selected(dd_min);
    t.tm_isdst = 0;
    mktime(&t);   /* normalises (e.g. 31.02) and fills tm_wday */
    if (bsp_rtc_set_time(&t) == ESP_OK) ui_toast("השעון עודכן", CLR_IN);
    else                                ui_toast("שגיאה בעדכון השעון", CLR_ERR);
}

static void clock_ntp_cb(lv_event_t *e)
{
    if (!bsp_wifi_is_connected()) {
        ui_toast("אין חיבור לרשת WiFi", CLR_ERR);
        return;
    }
    bsp_wifi_sync_time();
    ui_toast("מסנכרן שעון מהרשת...", CLR_INFO);
}

static lv_obj_t *settings_switch(lv_obj_t *row)
{
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 90, 44);
    return sw;
}

static void build_settings_tab(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 10, 0);
    lv_obj_set_style_pad_bottom(page, 280, 0);   /* room to scroll above the keyboard */

    lv_obj_t *r;
    r = setting_row(page, "שם המוסד");
    ta_inst = settings_textarea(r, 30, false);

    r = setting_row(page, "קוד מנהל");
    ta_pin = settings_textarea(r, 8, false);

    r = setting_row(page, "רשת WiFi");
    ta_ssid = settings_textarea(r, 32, false);

    r = setting_row(page, "סיסמת WiFi");
    ta_pass = settings_textarea(r, 64, true);

    r = setting_row(page, "אזור זמן (UTC)");
    dd_tz = number_dropdown(r, TZ_MIN, 14, "%+d", 160);

    r = setting_row(page, "שעון קיץ");
    sw_dst = settings_switch(r);

    r = setting_row(page, "עדכון שעון מהרשת");
    sw_ntp = settings_switch(r);

    r = setting_row(page, "זיהוי פנים בכניסה");
    sw_face = settings_switch(r);

    r = setting_row(page, "זמן בין כניסה ליציאה");
    dd_gap = lv_dropdown_create(r);
    lv_dropdown_set_options(dd_gap, "דקה\n5 דקות\n10 דקות\n15 דקות\n30 דקות\nשעה\nשעתיים");
    style_dropdown(dd_gap, 220);

    r = setting_row(page, "עוצמת צפצוף");
    sl_vol = lv_slider_create(r);
    lv_slider_set_range(sl_vol, 0, 100);
    lv_obj_set_width(sl_vol, 400);

    r = row_box(page, 80);
    ui_button(r, LV_SYMBOL_SAVE " שמירת הגדרות", CLR_IN, 340, 68, settings_save_cb, NULL);

    lv_obj_t *sep = ui_box(page, LV_PCT(100), 2);
    lv_obj_set_style_bg_color(sep, CLR_DIVIDER, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    r = setting_row(page, "תאריך");
    dd_day  = number_dropdown(r, 1, 31, "%02d", 110);
    dd_mon  = number_dropdown(r, 1, 12, "%02d", 110);
    dd_year = number_dropdown(r, YEAR_MIN, YEAR_MIN + 15, "%d", 140);

    r = setting_row(page, "שעה");
    dd_hour = number_dropdown(r, 0, 23, "%02d", 110);
    dd_min  = number_dropdown(r, 0, 59, "%02d", 110);

    r = row_box(page, 80);
    ui_button(r, "קביעת שעון", CLR_BTN, 260, 64, clock_set_cb, NULL);
    ui_button(r, "סנכרון מהרשת", CLR_BTN, 260, 64, clock_ntp_cb, NULL);
}

/* ══ Sensor tab ════════════════════════════════════════════════════════════ */

static void sen_refresh_cb(lv_event_t *e)
{
    lv_label_set_text(sen_status, "בודק...");
    fp_service_refresh_status();
}

static void sen_cleanup_cb(lv_event_t *e)
{
    fp_service_cleanup_orphans();
}

static void do_wipe(void *user)
{
    fp_service_wipe_all();
}

static void sen_wipe_cb(lv_event_t *e)
{
    ui_confirm("למחוק את כל טביעות האצבע מהחיישן?\nכל התלמידים יצטרכו להירשם מחדש.",
               "מחיקת הכל", CLR_ERR, do_wipe, NULL);
}

static void build_sensor_tab(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 16, 0);

    sen_status = ui_label(page, &g_font48, CLR_TEXT, "");
    sen_detail = ui_label(page, &g_font32, CLR_MUTED, "");

    char buf[128];
    snprintf(buf, sizeof(buf), "חיבור: UART%d  TX=GPIO%d  RX=GPIO%d  %d baud",
             FP_UART_NUM, FP_UART_TX_GPIO, FP_UART_RX_GPIO, FP_UART_BAUD);
    ui_label(page, &g_font22, CLR_MUTED, buf);

    lv_obj_t *r = row_box(page, 80);
    ui_button(r, LV_SYMBOL_REFRESH " בדיקת חיבור", CLR_BTN, 280, 68, sen_refresh_cb, NULL);
    ui_button(r, "ניקוי תבניות יתומות", CLR_BTN, 300, 68, sen_cleanup_cb, NULL);
    ui_button(r, "מחיקת כל הטביעות", CLR_ERR, 300, 68, sen_wipe_cb, NULL);

    ui_label(page, &g_font22, CLR_MUTED,
             "תבנית יתומה = טביעה ששמורה בחיישן אך אינה שייכת לאף תלמיד.");
}

/* ══ Screen ════════════════════════════════════════════════════════════════ */

static void close_btn_cb(lv_event_t *e)
{
    ui_admin_close();
}

static void tab_changed_cb(lv_event_t *e)
{
    hide_keyboard();
    uint32_t tab = lv_tabview_get_tab_active(s_tv);
    if (tab != TAB_CAMERA) ui_camera_stop();
    if (tab == 0) students_refresh();
    else if (tab == 1) report_refresh();
}

static void inactivity_timer_cb(lv_timer_t *t)
{
    if (s_visible && !s_enrolling && !s_face_enrolling &&
        lv_display_get_inactive_time(NULL) > INACTIVITY_CLOSE_MS) {
        ESP_LOGI(TAG, "Admin idle — closing");
        ui_admin_close();
    }
}

void ui_admin_create(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, CLR_BG, 0);
    lv_obj_set_style_base_dir(s_scr, LV_BASE_DIR_RTL, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    s_tv = lv_tabview_create(s_scr);
    lv_obj_set_size(s_tv, LV_PCT(100), LV_PCT(100));
    lv_tabview_set_tab_bar_size(s_tv, 70);
    lv_obj_set_style_bg_color(s_tv, CLR_BG, 0);

    lv_obj_t *bar = lv_tabview_get_tab_bar(s_tv);
    lv_obj_set_style_text_font(bar, &g_font32, 0);
    lv_obj_set_style_bg_color(bar, CLR_PANEL, 0);
    lv_obj_set_style_text_color(bar, CLR_TEXT, 0);
    lv_obj_set_style_pad_left(bar, 170, 0);   /* room for the close button */

    build_students_tab(tab_page_setup(lv_tabview_add_tab(s_tv, "תלמידים")));
    build_report_tab(tab_page_setup(lv_tabview_add_tab(s_tv, "נוכחות היום")));
    build_settings_tab(tab_page_setup(lv_tabview_add_tab(s_tv, "הגדרות")));
    build_sensor_tab(tab_page_setup(lv_tabview_add_tab(s_tv, "חיישן")));
    ui_camera_build_tab(tab_page_setup(lv_tabview_add_tab(s_tv, "מצלמה")));
    lv_obj_add_event_cb(s_tv, tab_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *close = ui_button(s_scr, LV_SYMBOL_CLOSE " יציאה", CLR_ERR, 150, 56, close_btn_cb, NULL);
    ui_align(close, LV_ALIGN_TOP_LEFT, 8, 7);

    s_kb = ui_keyboard_create(s_scr);
    ui_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(s_kb, kb_done_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, kb_done_cb, LV_EVENT_CANCEL, NULL);

    lv_timer_create(inactivity_timer_cb, 5000, NULL);
}

void ui_admin_show(void)
{
    if (s_visible) return;
    s_visible = true;
    fp_service_set_scanning(false);
    face_service_set_kiosk(false);
    fp_service_refresh_status();
    settings_load_widgets();
    students_refresh();
    report_refresh();
    lv_tabview_set_active(s_tv, 0, LV_ANIM_OFF);
    lv_screen_load(s_scr);
}

void ui_admin_close(void)
{
    if (!s_visible) return;
    if (s_enrolling) {
        fp_service_cancel();
        s_enrolling = false;
    }
    face_service_enroll_cancel();
    s_face_enrolling = false;
    ui_modal_close();
    hide_keyboard();
    ui_camera_stop();
    s_visible = false;
    fp_service_set_scanning(true);
    face_service_set_kiosk(settings_get()->face_enabled);
    ui_main_show();
}

void ui_admin_debug_show_tab(int tab)
{
    ui_admin_show();
    lv_tabview_set_active(s_tv, tab, LV_ANIM_OFF);
}

bool ui_admin_is_visible(void)
{
    return s_visible;
}

void ui_admin_handle_event(const fp_event_t *e)
{
    char buf[96];
    switch (e->type) {
    case FP_EVT_SENSOR_STATUS:
        lv_label_set_text(sen_status, e->ok ? "החיישן מחובר" : "החיישן אינו מגיב");
        lv_obj_set_style_text_color(sen_status, e->ok ? lv_color_hex(0x55DD55) : CLR_ERR, 0);
        if (e->ok) {
            snprintf(buf, sizeof(buf), "טביעות שמורות: %u מתוך %u", e->templates, e->capacity);
            lv_label_set_text(sen_detail, buf);
        } else {
            lv_label_set_text(sen_detail, "בדקו חיווט, מתח 3.3V וקצב תקשורת");
        }
        break;

    case FP_EVT_ENROLL_STEP:
    case FP_EVT_ENROLL_DONE:
        enroll_handle_event(e);
        break;

    case FP_EVT_OP_DONE:
        if (e->step == FP_OP_CLEANUP) {
            snprintf(buf, sizeof(buf), "נמחקו %d תבניות יתומות", e->count);
            ui_toast(buf, CLR_INFO);
        } else if (e->step == FP_OP_WIPE) {
            ui_toast(e->ok ? "כל הטביעות נמחקו" : "המחיקה נכשלה", e->ok ? CLR_INFO : CLR_ERR);
            students_refresh();
        }
        break;

    default:
        break;
    }
}
