#include "ui_common.h"
#include "settings_store.h"
#include "fonts/fonts.h"
#include <stdio.h>
#include <string.h>

lv_font_t g_font22;
lv_font_t g_font32;
lv_font_t g_font48;

void ui_fonts_init(void)
{
    /* The generated fonts are const; copy them so a fallback can be attached
     * (the David fonts have no LV_SYMBOL_* glyphs). */
    g_font22 = font_heb_22;
    g_font32 = font_heb_32;
    g_font48 = font_heb_48;
    g_font22.fallback = &lv_font_montserrat_20;
    g_font32.fallback = &lv_font_montserrat_32;
    g_font48.fallback = &lv_font_montserrat_32;
}

/* ── RTL-safe alignment ─────────────────────────────────────────────────── */

void ui_align(lv_obj_t *obj, lv_align_t align, int32_t x, int32_t y)
{
    lv_obj_t *parent = lv_obj_get_parent(obj);
    if (parent && lv_obj_get_style_base_dir(parent, LV_PART_MAIN) == LV_BASE_DIR_RTL) {
        /* LVGL (lv_obj_pos.c) in an RTL parent: *_LEFT,x lands at physical right
         * inset x; *_RIGHT,x lands at physical left at x; *_MID offsets are negated. */
        switch (align) {
        case LV_ALIGN_TOP_LEFT:     align = LV_ALIGN_TOP_RIGHT;    break;
        case LV_ALIGN_LEFT_MID:     align = LV_ALIGN_RIGHT_MID;    break;
        case LV_ALIGN_BOTTOM_LEFT:  align = LV_ALIGN_BOTTOM_RIGHT; break;
        case LV_ALIGN_TOP_RIGHT:    align = LV_ALIGN_TOP_LEFT;    x = -x; break;
        case LV_ALIGN_RIGHT_MID:    align = LV_ALIGN_LEFT_MID;    x = -x; break;
        case LV_ALIGN_BOTTOM_RIGHT: align = LV_ALIGN_BOTTOM_LEFT; x = -x; break;
        default:                    x = -x; break;
        }
    }
    lv_obj_align(obj, align, x, y);
}

void ui_fmt_time_in_rtl(int sec, char *out)
{
    if (sec < 0) {
        strcpy(out, "--:--");
        return;
    }
    unsigned h = (unsigned)(sec / 3600) % 24u, m = (unsigned)(sec / 60) % 60u;
    snprintf(out, 8, "%02u:%02u", m, h);   /* displayed as HH:MM after BiDi reordering */
}

/* ── Basic widgets ──────────────────────────────────────────────────────── */

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_color_t bg,
                    int w, int h, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_color(b, lv_color_lighten(bg, 40), LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = ui_label(b, h >= 64 ? &g_font32 : &g_font22, CLR_TEXT, text);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    return b;
}

lv_obj_t *ui_box(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *ui_panel(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, CLR_PANEL, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 16, 0);
    lv_obj_set_style_pad_all(o, 16, 0);
    return o;
}

/* ── Keyboard ───────────────────────────────────────────────────────────── */

#define KEY_HEB "עב"
#define KEY_EN  "EN"
#define KEY_NUM "123"
#define F LV_KEYBOARD_CTRL_BUTTON_FLAGS

/* Israeli standard layout */
static const char *const kb_map_heb[] = {
    "ק", "ר", "א", "ט", "ו", "ן", "ם", "פ", LV_SYMBOL_BACKSPACE, "\n",
    "ש", "ד", "ג", "כ", "ע", "י", "ח", "ל", "ך", "ף", "\n",
    "ז", "ס", "ב", "ה", "נ", "מ", "צ", "ת", "ץ", "'", "\n",
    KEY_EN, KEY_NUM, " ", "-", LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t kb_ctrl_heb[] = {
    4, 4, 4, 4, 4, 4, 4, 4, F | 6,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    F | 5, F | 5, 14, 4, F | 6,
};

static const char *const kb_map_en[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "'", "\n",
    "z", "x", "c", "v", "b", "n", "m", "-", ".", "\n",
    KEY_HEB, KEY_NUM, " ", "@", LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t kb_ctrl_en[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, F | 6,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4,
    F | 5, F | 5, 14, 4, F | 6,
};

static const char *const kb_map_num[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "-", "/", ".", "(", ")", "'", "@", "_", "!", "?", "\n",
    KEY_HEB, KEY_EN, " ", LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t kb_ctrl_num[] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, F | 6,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    F | 5, F | 5, 18, F | 6,
};

static void kb_event_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_current_target_obj(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *txt = lv_buttonmatrix_get_button_text(kb, id);
    if (!txt) return;

    if (strcmp(txt, KEY_HEB) == 0) { lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1); return; }
    if (strcmp(txt, KEY_EN) == 0)  { lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_2); return; }
    if (strcmp(txt, KEY_NUM) == 0) { lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_3); return; }

    lv_keyboard_def_event_cb(e);
}

lv_obj_t *ui_keyboard_create(lv_obj_t *parent)
{
    lv_obj_t *kb = lv_keyboard_create(parent);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, kb_map_heb, kb_ctrl_heb);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_2, kb_map_en, kb_ctrl_en);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_3, kb_map_num, kb_ctrl_num);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1);

    /* Replace the default handler so our layout-switch keys aren't typed */
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_set_size(kb, LV_PCT(100), 260);
    lv_obj_set_style_base_dir(kb, LV_BASE_DIR_LTR, 0);   /* maps are in visual order */
    lv_obj_set_style_text_font(kb, &g_font32, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, CLR_PANEL, 0);
    lv_obj_set_style_bg_color(kb, CLR_BTN, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, CLR_BTN_ACT, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(kb, CLR_PANEL_HI, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, CLR_TEXT, LV_PART_ITEMS);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    return kb;
}

void ui_keyboard_attach(lv_obj_t *kb, lv_obj_t *textarea, bool hebrew)
{
    lv_keyboard_set_textarea(kb, textarea);
    lv_keyboard_set_mode(kb, hebrew ? LV_KEYBOARD_MODE_USER_1 : LV_KEYBOARD_MODE_USER_2);
    lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(kb);
}

/* ── Modal ──────────────────────────────────────────────────────────────── */

static lv_obj_t *s_modal_bg;

lv_obj_t *ui_modal_open(int w, int h)
{
    ui_modal_close();
    s_modal_bg = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_modal_bg);
    lv_obj_set_size(s_modal_bg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_modal_bg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_modal_bg, LV_OPA_60, 0);
    lv_obj_set_style_base_dir(s_modal_bg, LV_BASE_DIR_RTL, 0);
    lv_obj_add_flag(s_modal_bg, LV_OBJ_FLAG_CLICKABLE);   /* swallow clicks behind */

    lv_obj_t *card = ui_panel(s_modal_bg, w, h);
    lv_obj_set_style_bg_color(card, CLR_PANEL_HI, 0);
    lv_obj_set_style_border_color(card, CLR_DIVIDER, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_center(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

void ui_modal_close(void)
{
    if (s_modal_bg) {
        lv_obj_delete_async(s_modal_bg);   /* safe from inside the modal's own callbacks */
        s_modal_bg = NULL;
    }
}

bool ui_modal_is_open(void)
{
    return s_modal_bg != NULL;
}

/* ── Confirm dialog ─────────────────────────────────────────────────────── */

static ui_action_cb_t s_confirm_cb;
static void *s_confirm_user;

static void confirm_ok_cb(lv_event_t *e)
{
    ui_action_cb_t cb = s_confirm_cb;
    void *user = s_confirm_user;
    ui_modal_close();
    if (cb) cb(user);
}

static void modal_cancel_cb(lv_event_t *e)
{
    ui_modal_close();
}

void ui_confirm(const char *text, const char *ok_text, lv_color_t ok_color,
                ui_action_cb_t on_ok, void *user)
{
    s_confirm_cb = on_ok;
    s_confirm_user = user;

    lv_obj_t *card = ui_modal_open(620, 300);
    lv_obj_t *l = ui_label(card, &g_font32, CLR_TEXT, text);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    ui_align(l, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *ok = ui_button(card, ok_text, ok_color, 240, 72, confirm_ok_cb, NULL);
    ui_align(ok, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_t *cancel = ui_button(card, "ביטול", CLR_BTN, 240, 72, modal_cancel_cb, NULL);
    ui_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

/* ── Toast ──────────────────────────────────────────────────────────────── */

static lv_obj_t *s_toast;

static void toast_timer_cb(lv_timer_t *t)
{
    if (s_toast) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
}

void ui_toast(const char *text, lv_color_t color)
{
    if (s_toast) lv_obj_delete(s_toast);
    s_toast = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_toast, &g_font32, 0);
    lv_obj_set_style_text_color(s_toast, CLR_TEXT, 0);
    lv_obj_set_style_bg_color(s_toast, color, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, 14, 0);
    lv_obj_set_style_pad_hor(s_toast, 30, 0);
    lv_obj_set_style_pad_ver(s_toast, 14, 0);
    lv_label_set_text(s_toast, text);
    ui_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -30);

    lv_timer_t *t = lv_timer_create(toast_timer_cb, 2500, NULL);
    lv_timer_set_repeat_count(t, 1);
}

/* ── PIN prompt ─────────────────────────────────────────────────────────── */

static ui_action_cb_t s_pin_cb;
static void *s_pin_user;
static lv_obj_t *s_pin_ta;
static lv_obj_t *s_pin_title;

static const char *const pin_map[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""
};

static void pin_btnm_cb(lv_event_t *e)
{
    lv_obj_t *bm = lv_event_get_current_target_obj(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(bm);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *txt = lv_buttonmatrix_get_button_text(bm, id);

    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_delete_char(s_pin_ta);
    } else if (strcmp(txt, LV_SYMBOL_OK) == 0) {
        if (strcmp(lv_textarea_get_text(s_pin_ta), settings_get()->admin_pin) == 0) {
            ui_action_cb_t cb = s_pin_cb;
            void *user = s_pin_user;
            ui_modal_close();
            if (cb) cb(user);
        } else {
            lv_textarea_set_text(s_pin_ta, "");
            lv_label_set_text(s_pin_title, "קוד שגוי, נסו שוב");
            lv_obj_set_style_text_color(s_pin_title, CLR_ERR, 0);
        }
    } else {
        lv_textarea_add_text(s_pin_ta, txt);
    }
}

void ui_pin_prompt(ui_action_cb_t on_ok, void *user)
{
    s_pin_cb = on_ok;
    s_pin_user = user;

    lv_obj_t *card = ui_modal_open(440, 560);

    s_pin_title = ui_label(card, &g_font32, CLR_TEXT, "הזינו קוד מנהל");
    ui_align(s_pin_title, LV_ALIGN_TOP_MID, 0, 0);

    s_pin_ta = lv_textarea_create(card);
    lv_textarea_set_one_line(s_pin_ta, true);
    lv_textarea_set_password_mode(s_pin_ta, true);
    lv_textarea_set_max_length(s_pin_ta, 8);
    lv_obj_set_width(s_pin_ta, 260);
    lv_obj_set_style_text_font(s_pin_ta, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_align(s_pin_ta, LV_TEXT_ALIGN_CENTER, 0);
    ui_align(s_pin_ta, LV_ALIGN_TOP_MID, 0, 50);

    lv_obj_t *bm = lv_buttonmatrix_create(card);
    lv_buttonmatrix_set_map(bm, pin_map);
    lv_obj_set_size(bm, 380, 310);
    lv_obj_set_style_base_dir(bm, LV_BASE_DIR_LTR, 0);
    lv_obj_set_style_text_font(bm, &lv_font_montserrat_32, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_bg_color(bm, CLR_BTN, LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm, CLR_TEXT, LV_PART_ITEMS);
    ui_align(bm, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_add_event_cb(bm, pin_btnm_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *cancel = ui_button(card, "ביטול", CLR_BTN, 200, 56, modal_cancel_cb, NULL);
    ui_align(cancel, LV_ALIGN_BOTTOM_MID, 0, 0);
}
