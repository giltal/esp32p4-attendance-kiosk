#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

typedef struct {
    char institute_name[64]; /* Shown in the main-screen header (UTF-8) */
    char admin_pin[9];       /* Digits only, 4-8 chars */
    int  timezone;           /* UTC offset (e.g. 2 for Israel) */
    bool summer_time;        /* DST enabled */
    bool auto_ntp;           /* Auto clock sync from NTP */
    char wifi_ssid[33];      /* WiFi SSID (max 32 chars + NUL) */
    char wifi_password[65];  /* WiFi password (max 64 chars + NUL) */
    int  min_scan_gap_s;     /* Min. time between a student's IN and OUT (s); repeats inside it are ignored */
    int  volume;             /* Beep volume 0-100 */
    bool face_enabled;       /* Face recognition at the door (camera) */
} app_settings_t;

/* Load settings from /sd/config.txt. Returns defaults if file missing. */
void settings_load(app_settings_t *s);

/* Save settings to /sd/config.txt. */
void settings_save(const app_settings_t *s);

/* Get pointer to the current live settings (singleton). */
app_settings_t *settings_get(void);

#ifdef __cplusplus
}
#endif
