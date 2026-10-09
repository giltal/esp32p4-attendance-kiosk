#pragma once

/*
 * Monthly attendance report, computed from the daily logs
 * /sd/logs/YYYY-MM-DD.csv (written by attendance.c).
 *
 * Per student: days present, total time inside (sum of IN -> OUT/AUTO_OUT
 * pairs), average first arrival, days with no exit recorded (today excluded).
 * Export: /sd/reports/YYYY-MM.csv (UTF-8 with BOM) — summary columns plus
 * one column per day ("08:05-14:30", "08:05-?" when the exit is missing).
 *
 * Not thread-safe: call from one task (the LVGL task).
 */

#include "esp_err.h"
#include "student_db.h"
#include <stdint.h>

typedef struct {
    uint16_t id;
    char     name[STUDENT_NAME_MAX];
    int      days_present;
    int      missing_exits;
    int      total_min;        /* minutes inside, whole month */
    int      avg_arrival_min;  /* minutes since midnight, -1 if never present */
} report_row_t;

/* Build the report for a month. Rows are sorted by name; returns the row count
 * (current students + anyone who appears in that month's logs). */
int report_build(int year, int month, report_row_t *rows, int max);

/* Build and write /sd/reports/YYYY-MM.csv; the path is returned in out_path. */
esp_err_t report_export(int year, int month, char *out_path, size_t out_len);

/* Hebrew month name, month = 1..12. */
const char *report_month_name(int month);
