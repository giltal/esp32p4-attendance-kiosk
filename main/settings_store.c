#include "settings_store.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static const char *TAG = "SETTINGS";
#define CONFIG_PATH "/sd/config.txt"
#define CONFIG_TMP  "/sd/config.tmp"

static app_settings_t s_settings;

static const app_settings_t s_defaults = {
    .institute_name = "מערכת נוכחות",
    .admin_pin      = "1234",
    .timezone       = 2,
    .summer_time    = true,
    .auto_ntp       = true,
    .wifi_ssid      = "",
    .wifi_password  = "",
    .min_scan_gap_s = 30 * 60,
    .volume         = 60,
    .face_enabled   = true,
};

static void copy_str(char *dst, size_t size, const char *src)
{
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

void settings_load(app_settings_t *s)
{
    *s = s_defaults;

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "No config file, using defaults");
        return;
    }

    char line[160];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        if      (strcmp(key, "institute") == 0)     copy_str(s->institute_name, sizeof(s->institute_name), val);
        else if (strcmp(key, "admin_pin") == 0)     copy_str(s->admin_pin, sizeof(s->admin_pin), val);
        else if (strcmp(key, "timezone") == 0)      s->timezone = atoi(val);
        else if (strcmp(key, "summer") == 0)        s->summer_time = (atoi(val) != 0);
        else if (strcmp(key, "auto_ntp") == 0)      s->auto_ntp = (atoi(val) != 0);
        else if (strcmp(key, "wifi_ssid") == 0)     copy_str(s->wifi_ssid, sizeof(s->wifi_ssid), val);
        else if (strcmp(key, "wifi_password") == 0) copy_str(s->wifi_password, sizeof(s->wifi_password), val);
        else if (strcmp(key, "inout_gap") == 0)     s->min_scan_gap_s = atoi(val);   /* old "scan_gap" ignored: new default */
        else if (strcmp(key, "volume") == 0)        s->volume = atoi(val);
        else if (strcmp(key, "face") == 0)          s->face_enabled = (atoi(val) != 0);
    }
    fclose(f);

    if (strlen(s->admin_pin) < 4) copy_str(s->admin_pin, sizeof(s->admin_pin), s_defaults.admin_pin);

    ESP_LOGI(TAG, "Loaded: '%s' tz=%d dst=%d ssid='%s' gap=%ds vol=%d",
             s->institute_name, s->timezone, s->summer_time, s->wifi_ssid,
             s->min_scan_gap_s, s->volume);
}

void settings_save(const app_settings_t *s)
{
    /* Write to a temp file first so a crash mid-write never leaves an empty config */
    FILE *f = fopen(CONFIG_TMP, "w");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s,w) failed: errno=%d", CONFIG_TMP, errno);
        return;
    }

    fprintf(f, "institute=%s\n", s->institute_name);
    fprintf(f, "admin_pin=%s\n", s->admin_pin);
    fprintf(f, "timezone=%d\n", s->timezone);
    fprintf(f, "summer=%d\n", s->summer_time ? 1 : 0);
    fprintf(f, "auto_ntp=%d\n", s->auto_ntp ? 1 : 0);
    if (s->wifi_ssid[0])     fprintf(f, "wifi_ssid=%s\n", s->wifi_ssid);
    if (s->wifi_password[0]) fprintf(f, "wifi_password=%s\n", s->wifi_password);
    fprintf(f, "inout_gap=%d\n", s->min_scan_gap_s);
    fprintf(f, "volume=%d\n", s->volume);
    fprintf(f, "face=%d\n", s->face_enabled ? 1 : 0);

    int flush_ret = fflush(f);
    int close_ret = fclose(f);
    if (flush_ret != 0 || close_ret != 0) {
        ESP_LOGE(TAG, "write failed (fflush=%d fclose=%d)", flush_ret, close_ret);
        remove(CONFIG_TMP);
        return;
    }

    remove(CONFIG_PATH);   /* FAT rename() won't overwrite */
    if (rename(CONFIG_TMP, CONFIG_PATH) != 0) {
        ESP_LOGE(TAG, "rename failed: errno=%d", errno);
        return;
    }
    ESP_LOGI(TAG, "Saved %s", CONFIG_PATH);
}

app_settings_t *settings_get(void)
{
    return &s_settings;
}
