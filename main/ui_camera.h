#pragma once

/* Camera preview tab (admin screen). LVGL-lock rules as in ui_common.h. */

#include "lvgl.h"

/* Build the tab content into an admin tabview page. */
void ui_camera_build_tab(lv_obj_t *page);

/* Stop the preview (tab left / admin closed). Safe to call any time. */
void ui_camera_stop(void);

/* TEMPORARY bring-up test without the UI: streams, logs FPS/colour, cycles
 * the stream off/on, dumps an 80x45 thumbnail as hex lines "CAMTHUMB:..". */
void ui_camera_selftest(void);
