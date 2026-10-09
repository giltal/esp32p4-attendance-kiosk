#pragma once

#include "fp_service.h"
#include <stdbool.h>

/* Main (kiosk) screen: clock, scan result card, list of present students.
 * All functions must be called with the LVGL lock held. */

void ui_main_create(bool sd_ok);

/* Show the main screen again (after admin closes). */
void ui_main_show(void);

/* Handle a fingerprint-service event. */
void ui_main_handle_event(const fp_event_t *e);

/* Rebuild the "present now" list. */
void ui_main_refresh_present(void);

/* Re-read institute name etc. after settings changed. */
void ui_main_reload_settings(void);
