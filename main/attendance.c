#include "attendance.h"
#include "settings_store.h"
#include "bsp_rtc.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

static const char *TAG = "ATTENDANCE";

#define LOG_DIR   "/sd/logs"
#define UTF8_BOM  "\xEF\xBB\xBF"

EXT_RAM_BSS_ATTR static att_record_t s_rec[STUDENT_MAX];
static int s_rec_count = 0;
static int s_day_key = -1;          /* yyyymmdd of the loaded day */
static char s_date_str[40];         /* "YYYY-MM-DD" */
static SemaphoreHandle_t s_lock;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static int day_key(const struct tm *t)
{
    return (t->tm_year + 1900) * 10000 + (t->tm_mon + 1) * 100 + t->tm_mday;
}

static void log_path(char *out, size_t size)
{
    snprintf(out, size, LOG_DIR "/%s.csv", s_date_str);
}

static att_record_t *find_or_add(uint16_t id, const char *name)
{
    for (int i = 0; i < s_rec_count; i++) {
        if (s_rec[i].id == id) {
            if (name) strlcpy(s_rec[i].name, name, sizeof(s_rec[i].name));
            return &s_rec[i];
        }
    }
    if (s_rec_count >= STUDENT_MAX) return NULL;
    att_record_t *r = &s_rec[s_rec_count++];
    memset(r, 0, sizeof(*r));
    r->id = id;
    r->first_in = r->last_in = r->last_out = r->last_scan = ATT_NO_TIME;
    if (name) strlcpy(r->name, name, sizeof(r->name));
    return r;
}

static void apply(att_record_t *r, att_dir_t dir, int sec)
{
    if (dir == ATT_IN) {
        if (r->first_in == ATT_NO_TIME) r->first_in = sec;
        r->last_in = sec;
        r->present = true;
    } else {
        r->last_out = sec;
        r->present = false;
    }
    r->last_scan = sec;
    r->scans++;
}

static bool append_log(int sec, uint16_t id, const char *dir, const char *name)
{
    char path[72];
    log_path(path, sizeof(path));

    struct stat st;
    bool is_new = (stat(path, &st) != 0);

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed: errno=%d", path, errno);
        return false;
    }
    if (is_new) fputs(UTF8_BOM "date,time,id,direction,name\n", f);
    fprintf(f, "%s,%02d:%02d:%02d,%u,%s,%s\n", s_date_str,
            sec / 3600, (sec / 60) % 60, sec % 60, id, dir, name);
    bool ok = (fflush(f) == 0);
    ok = (fclose(f) == 0) && ok;
    if (!ok) ESP_LOGE(TAG, "write to %s failed", path);
    return ok;
}

/* Load the given day: reset state and replay its log file. Caller holds lock. */
static void load_day_locked(const struct tm *t)
{
    s_rec_count = 0;
    s_day_key = day_key(t);
    snprintf(s_date_str, sizeof(s_date_str), "%04d-%02d-%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);

    char path[72];
    log_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGI(TAG, "New day %s — no log yet", s_date_str);
        return;
    }

    char line[160];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        if (strncmp(p, UTF8_BOM, 3) == 0) p += 3;
        p[strcspn(p, "\r\n")] = '\0';

        /* date,HH:MM:SS,id,dir,name */
        int hh, mm, ss;
        unsigned id;
        char dir[10];
        int consumed = 0;
        if (sscanf(p, "%*10[^,],%d:%d:%d,%u,%9[^,],%n", &hh, &mm, &ss, &id, dir, &consumed) < 5 ||
            consumed == 0) {
            continue;   /* header or malformed */
        }
        att_record_t *r = find_or_add(id, p + consumed);
        if (!r) break;
        apply(r, strcmp(dir, "IN") == 0 ? ATT_IN : ATT_OUT, hh * 3600 + mm * 60 + ss);
        ESP_LOGI(TAG, "  replay %02d:%02d:%02d #%u %-8s %s", hh, mm, ss, id, dir, p + consumed);
        n++;
    }
    fclose(f);
    ESP_LOGI(TAG, "Replayed %d events for %s (%d students, %d present)",
             n, s_date_str, s_rec_count, attendance_present_count());
}

/*
 * End of day: everyone still inside the loaded day gets a DAY_END exit at
 * 23:59:59. Reports treat DAY_END as "exit not recorded" (no hours counted).
 * Caller holds lock.
 */
#define DAY_END_SEC  (24 * 3600 - 1)

static void close_day_locked(void)
{
    int n = 0;
    for (int i = 0; i < s_rec_count; i++) {
        if (!s_rec[i].present) continue;
        apply(&s_rec[i], ATT_OUT, DAY_END_SEC);
        append_log(DAY_END_SEC, s_rec[i].id, "DAY_END", s_rec[i].name);
        n++;
    }
    if (n) ESP_LOGW(TAG, "%s: %d student(s) never checked out — closed at end of day", s_date_str, n);
}

esp_err_t attendance_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    mkdir(LOG_DIR, 0775);

    struct tm now;
    if (bsp_rtc_get_time(&now) != ESP_OK) {
        ESP_LOGE(TAG, "RTC unavailable — attendance timestamps will be wrong");
        memset(&now, 0, sizeof(now));
        now.tm_year = 125; now.tm_mday = 1;
    }
    LOCK();
    /* If the device was off at midnight, close yesterday's open entries first */
    struct tm yday = now;
    yday.tm_mday -= 1;
    yday.tm_isdst = 0;
    mktime(&yday);                 /* normalise across month/year boundaries */
    load_day_locked(&yday);
    close_day_locked();
    load_day_locked(&now);
    UNLOCK();
    return ESP_OK;
}

void attendance_tick(const struct tm *now)
{
    if (!s_lock || day_key(now) == s_day_key) return;
    LOCK();
    if (day_key(now) != s_day_key) {
        ESP_LOGI(TAG, "Day changed — closing %s and resetting attendance", s_date_str);
        close_day_locked();        /* auto check-out for anyone still inside */
        load_day_locked(now);
    }
    UNLOCK();
}

att_result_t attendance_register(const student_t *s)
{
    att_result_t res = { .dir = ATT_IN, .repeat = false, .time_sec = 0, .log_ok = true,
                         .prev_sec = ATT_NO_TIME, .next_ok_sec = ATT_NO_TIME };

    struct tm now;
    bsp_rtc_get_time(&now);
    attendance_tick(&now);
    int sec = now.tm_hour * 3600 + now.tm_min * 60 + now.tm_sec;
    res.time_sec = sec;

    LOCK();
    att_record_t *r = find_or_add(s->id, s->name);
    if (!r) {
        UNLOCK();
        res.log_ok = false;
        return res;
    }

    int gap = settings_get()->min_scan_gap_s;
    res.prev_sec = r->last_scan;
    if (r->last_scan != ATT_NO_TIME && sec - r->last_scan >= 0 && sec - r->last_scan < gap) {
        res.repeat = true;
        res.dir = r->present ? ATT_IN : ATT_OUT;
        res.next_ok_sec = r->last_scan + gap;
        UNLOCK();
        ESP_LOGI(TAG, "#%u %s repeat (%s at %02d:%02d, next change allowed %02d:%02d)", s->id, s->name,
                 r->present ? "IN" : "OUT", r->last_scan / 3600, (r->last_scan / 60) % 60,
                 (res.next_ok_sec / 3600) % 24, (res.next_ok_sec / 60) % 60);
        return res;
    }

    res.dir = r->present ? ATT_OUT : ATT_IN;
    apply(r, res.dir, sec);
    res.log_ok = append_log(sec, s->id, res.dir == ATT_IN ? "IN" : "OUT", s->name);
    UNLOCK();

    ESP_LOGI(TAG, "#%u %s %s", s->id, s->name, res.dir == ATT_IN ? "IN" : "OUT");
    return res;
}

int attendance_present_count(void)
{
    int n = 0;
    for (int i = 0; i < s_rec_count; i++) {
        if (s_rec[i].present) n++;
    }
    return n;
}

static int cmp_recent_first(const void *a, const void *b)
{
    return ((const att_record_t *)b)->last_scan - ((const att_record_t *)a)->last_scan;
}

int attendance_get_records(att_record_t *out, int max)
{
    LOCK();
    int n = s_rec_count < max ? s_rec_count : max;
    memcpy(out, s_rec, n * sizeof(att_record_t));
    UNLOCK();
    qsort(out, n, sizeof(att_record_t), cmp_recent_first);
    return n;
}

int attendance_mark_all_out(void)
{
    struct tm now;
    bsp_rtc_get_time(&now);
    attendance_tick(&now);
    int sec = now.tm_hour * 3600 + now.tm_min * 60 + now.tm_sec;

    int n = 0;
    LOCK();
    for (int i = 0; i < s_rec_count; i++) {
        if (!s_rec[i].present) continue;
        apply(&s_rec[i], ATT_OUT, sec);
        append_log(sec, s_rec[i].id, "AUTO_OUT", s_rec[i].name);
        n++;
    }
    UNLOCK();
    ESP_LOGI(TAG, "Marked %d students out", n);
    return n;
}

void attendance_fmt_time(int sec, char out[6])
{
    if (sec == ATT_NO_TIME) {
        strcpy(out, "--:--");
    } else {
        snprintf(out, 6, "%02u:%02u", (unsigned)(sec / 3600) % 24u, (unsigned)(sec / 60) % 60u);
    }
}
