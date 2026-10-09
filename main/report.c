#include "report.h"
#include "bsp_rtc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

static const char *TAG = "REPORT";

#define LOG_DIR     "/sd/logs"
#define REPORT_DIR  "/sd/reports"
#define UTF8_BOM    "\xEF\xBB\xBF"
#define MAX_PEOPLE  (STUDENT_MAX + 100)   /* current students + people only seen in old logs */
#define NO_TIME     (-1)

typedef struct {
    int16_t first_in;     /* minutes since midnight */
    int16_t last_out;
    int16_t open_in;      /* IN without a matching OUT yet */
    int16_t minutes;      /* time inside that day */
    uint8_t present;
    uint8_t missing_exit;
} day_t;

typedef struct {
    uint16_t id;
    char     name[STUDENT_NAME_MAX];
    day_t    day[31];
} person_t;

static person_t *s_people;          /* PSRAM, MAX_PEOPLE */
static int s_count;
static int s_days_in_month;
EXT_RAM_BSS_ATTR static report_row_t s_rows[MAX_PEOPLE];

static const char *const s_months[] = {
    "ינואר", "פברואר", "מרץ", "אפריל", "מאי", "יוני",
    "יולי", "אוגוסט", "ספטמבר", "אוקטובר", "נובמבר", "דצמבר"
};

const char *report_month_name(int month)
{
    return (month >= 1 && month <= 12) ? s_months[month - 1] : "";
}

static int days_in_month(int year, int month)
{
    static const int dm[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return (month == 2 && leap) ? 29 : dm[month - 1];
}

static void reset_person(person_t *p)
{
    for (int d = 0; d < 31; d++) {
        p->day[d] = (day_t){ .first_in = NO_TIME, .last_out = NO_TIME, .open_in = NO_TIME };
    }
}

static person_t *find_or_add(uint16_t id, const char *name)
{
    for (int i = 0; i < s_count; i++) {
        if (s_people[i].id == id) return &s_people[i];
    }
    if (s_count >= MAX_PEOPLE) return NULL;
    person_t *p = &s_people[s_count++];
    p->id = id;
    strlcpy(p->name, name ? name : "", sizeof(p->name));
    reset_person(p);
    return p;
}

static void parse_day(int year, int month, int d, bool is_today)
{
    char path[48];
    snprintf(path, sizeof(path), LOG_DIR "/%04d-%02d-%02d.csv", year, month, d);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        if (strncmp(p, UTF8_BOM, 3) == 0) p += 3;
        p[strcspn(p, "\r\n")] = '\0';

        int hh, mm, ss, consumed = 0;
        unsigned id;
        char dir[10];
        if (sscanf(p, "%*10[^,],%d:%d:%d,%u,%9[^,],%n", &hh, &mm, &ss, &id, dir, &consumed) < 5 ||
            consumed == 0) {
            continue;   /* header / malformed */
        }
        person_t *ps = find_or_add(id, p + consumed);
        if (!ps) break;
        day_t *dy = &ps->day[d - 1];
        int t = hh * 60 + mm;

        if (strcmp(dir, "IN") == 0) {
            if (dy->open_in == NO_TIME) dy->open_in = t;
            if (dy->first_in == NO_TIME) dy->first_in = t;
            dy->present = 1;
        } else if (strcmp(dir, "DAY_END") == 0) {
            /* automatic end-of-day close: keep the visit open so it counts as
             * "exit not recorded" and adds no hours */
        } else if (dy->open_in != NO_TIME) {     /* OUT / AUTO_OUT */
            dy->minutes += t - dy->open_in;
            dy->open_in = NO_TIME;
            dy->last_out = t;
        }
    }
    fclose(f);

    /* An IN left open at the end of a past day = exit not recorded */
    if (!is_today) {
        for (int i = 0; i < s_count; i++) {
            day_t *dy = &s_people[i].day[d - 1];
            if (dy->open_in != NO_TIME) dy->missing_exit = 1;
        }
    }
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp(((const report_row_t *)a)->name, ((const report_row_t *)b)->name);
}

int report_build(int year, int month, report_row_t *rows, int max)
{
    if (!s_people) {
        s_people = heap_caps_calloc(MAX_PEOPLE, sizeof(person_t), MALLOC_CAP_SPIRAM);
        if (!s_people) return 0;
    }
    s_count = 0;
    s_days_in_month = days_in_month(year, month);

    /* Every current student appears, even with zero days */
    for (int i = 0; i < student_db_count(); i++) {
        student_t st;
        if (student_db_get_at(i, &st)) find_or_add(st.id, st.name);
    }

    struct tm now = {0};
    bsp_rtc_get_time(&now);
    for (int d = 1; d <= s_days_in_month; d++) {
        bool today = (now.tm_year + 1900 == year && now.tm_mon + 1 == month && now.tm_mday == d);
        parse_day(year, month, d, today);
    }

    int n = s_count < max ? s_count : max;
    for (int i = 0; i < n; i++) {
        person_t *p = &s_people[i];
        report_row_t *r = &rows[i];
        memset(r, 0, sizeof(*r));
        r->id = p->id;
        strlcpy(r->name, p->name, sizeof(r->name));
        int arrive_sum = 0;
        for (int d = 0; d < s_days_in_month; d++) {
            day_t *dy = &p->day[d];
            if (!dy->present) continue;
            r->days_present++;
            r->missing_exits += dy->missing_exit;
            r->total_min += dy->minutes;
            arrive_sum += dy->first_in;
        }
        r->avg_arrival_min = r->days_present ? arrive_sum / r->days_present : NO_TIME;
    }
    qsort(rows, n, sizeof(report_row_t), cmp_name);
    ESP_LOGI(TAG, "%04d-%02d: %d people", year, month, n);
    return n;
}

static void fmt_hm(char *out, size_t len, int minutes)
{
    if (minutes < 0) out[0] = '\0';
    else snprintf(out, len, "%02d:%02d", minutes / 60, minutes % 60);
}

esp_err_t report_export(int year, int month, char *out_path, size_t out_len)
{
    int n = report_build(year, month, s_rows, MAX_PEOPLE);

    mkdir(REPORT_DIR, 0775);
    char path[48], tmp[48];
    snprintf(path, sizeof(path), REPORT_DIR "/%04d-%02d.csv", year, month);
    snprintf(tmp, sizeof(tmp), REPORT_DIR "/%04d-%02d.tmp", year, month);

    FILE *f = fopen(tmp, "w");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed: errno=%d", tmp, errno);
        return ESP_FAIL;
    }

    fputs(UTF8_BOM "מספר,שם,ימי נוכחות,סה״כ שעות,ממוצע הגעה,ימים ללא יציאה", f);
    for (int d = 1; d <= s_days_in_month; d++) fprintf(f, ",%d", d);
    fputc('\n', f);

    char a[16], b[16];
    for (int i = 0; i < n; i++) {
        const report_row_t *r = &s_rows[i];
        fmt_hm(a, sizeof(a), r->total_min);
        fmt_hm(b, sizeof(b), r->avg_arrival_min);
        fprintf(f, "%u,%s,%d,%s,%s,%d", r->id, r->name, r->days_present, a, b, r->missing_exits);

        const person_t *p = NULL;
        for (int k = 0; k < s_count && !p; k++) if (s_people[k].id == r->id) p = &s_people[k];
        for (int d = 0; d < s_days_in_month; d++) {
            const day_t *dy = p ? &p->day[d] : NULL;
            if (!dy || !dy->present) {
                fputc(',', f);
                continue;
            }
            fmt_hm(a, sizeof(a), dy->first_in);
            if (dy->open_in != NO_TIME) strcpy(b, "?");
            else fmt_hm(b, sizeof(b), dy->last_out);
            fprintf(f, ",%s-%s", a, b);
        }
        fputc('\n', f);
    }

    bool ok = (fflush(f) == 0);
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        remove(tmp);
        return ESP_FAIL;
    }
    remove(path);   /* FAT rename() won't overwrite */
    if (rename(tmp, path) != 0) {
        ESP_LOGE(TAG, "rename failed: errno=%d", errno);
        return ESP_FAIL;
    }
    if (out_path) strlcpy(out_path, path, out_len);
    ESP_LOGI(TAG, "Exported %s (%d rows)", path, n);
    return ESP_OK;
}
