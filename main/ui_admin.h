#pragma once

#include "fp_service.h"
#include <stdbool.h>

/* PIN-protected admin screen: students, today's report, settings, sensor.
 * All functions must be called with the LVGL lock held. */

void ui_admin_create(void);
void ui_admin_show(void);
void ui_admin_close(void);
bool ui_admin_is_visible(void);

/* TEMPORARY (camera self-test): open admin on a tab without the PIN. */
void ui_admin_debug_show_tab(int tab);
void ui_admin_handle_event(const fp_event_t *e);

/* Face enrollment progress (from face_service via main.c). */
#include "face_service.h"
void ui_admin_handle_face_enroll(const face_enroll_evt_t *e);
