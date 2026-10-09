#pragma once

/* Shared UI helpers: fonts, palette, widgets, dialogs, Hebrew keyboard.
 * All functions must be called with the LVGL lock held. */

#include "lvgl.h"
#include <stdbool.h>

/* Hebrew+Latin David fonts with Montserrat fallback for LV_SYMBOL_* glyphs */
extern lv_font_t g_font22;
extern lv_font_t g_font32;
extern lv_font_t g_font48;

/* Palette */
#define CLR_BG        lv_color_hex(0x1A1A2E)
#define CLR_PANEL     lv_color_hex(0x222244)
#define CLR_PANEL_HI  lv_color_hex(0x2C2C58)
#define CLR_DIVIDER   lv_color_hex(0x334466)
#define CLR_TEXT      lv_color_hex(0xFFFFFF)
#define CLR_MUTED     lv_color_hex(0x8899AA)
#define CLR_GOLD      lv_color_hex(0xE8D44D)
#define CLR_IN        lv_color_hex(0x2E9E5B)   /* entry — green */
#define CLR_OUT       lv_color_hex(0xD9822B)   /* exit — orange */
#define CLR_ERR       lv_color_hex(0xC0392B)   /* error — red */
#define CLR_INFO      lv_color_hex(0x3A6EA5)   /* neutral — blue */
#define CLR_BTN       lv_color_hex(0x334477)
#define CLR_BTN_ACT   lv_color_hex(0x4466AA)

void ui_fonts_init(void);

/* lv_obj_align() with PHYSICAL left/right, also inside RTL parents.
 * (LVGL mirrors LEFT/RIGHT alignments when the parent's base_dir is RTL.) */
void ui_align(lv_obj_t *obj, lv_align_t align, int32_t x, int32_t y);

/* Format seconds-since-midnight as a time to embed INSIDE Hebrew (RTL) text.
 * LVGL's BiDi reverses "09:24" to "24:09" in RTL paragraphs (':' is neutral), so
 * the halves are written swapped and come out right. For a label/cell that holds
 * only a time, use an LTR label with the normal format instead. out >= 8 bytes. */
void ui_fmt_time_in_rtl(int sec, char *out);

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_color_t bg,
                    int w, int h, lv_event_cb_t cb, void *user_data);

/* Plain container with no padding/border, optional flex flow. */
lv_obj_t *ui_box(lv_obj_t *parent, int w, int h);

/* Panel with rounded corners and the panel colour. */
lv_obj_t *ui_panel(lv_obj_t *parent, int w, int h);

/* On-screen keyboard with Hebrew / English / digits layouts. */
lv_obj_t *ui_keyboard_create(lv_obj_t *parent);
void ui_keyboard_attach(lv_obj_t *kb, lv_obj_t *textarea, bool hebrew);

/* Modal confirm dialog. on_ok runs only if the user confirms. */
typedef void (*ui_action_cb_t)(void *user);
void ui_confirm(const char *text, const char *ok_text, lv_color_t ok_color,
                ui_action_cb_t on_ok, void *user);

/* Short message shown on top of everything for a few seconds. */
void ui_toast(const char *text, lv_color_t color);

/* Ask for the admin PIN; on_ok runs if it matches. */
void ui_pin_prompt(ui_action_cb_t on_ok, void *user);

/* Full-screen modal backdrop on the top layer; returns the centered card. */
lv_obj_t *ui_modal_open(int w, int h);
void ui_modal_close(void);
bool ui_modal_is_open(void);
