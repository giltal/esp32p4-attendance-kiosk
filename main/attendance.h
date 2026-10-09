#pragma once

/*
 * Attendance logic: per-day IN/OUT state with auto-toggle.
 *
 * Every accepted scan is appended to /sd/logs/YYYY-MM-DD.csv:
 *     date,time,id,direction,name
 *     2026-10-08,08:01:12,7,IN,ישראל ישראלי
 * direction is IN, OUT, or AUTO_OUT (closed by "mark everyone out").
 *
 * On boot today's file is replayed, so a reboot never loses who is inside.
 * All functions are thread-safe.
 */

#include "esp_err.h"
#include "student_db.h"
#include <stdbool.h>
#include <time.h>

#define ATT_NO_TIME   (-1)

typedef enum {
    ATT_IN  = 0,
    ATT_OUT = 1,
} att_dir_t;

typedef struct {
    uint16_t id;
    char     name[STUDENT_NAME_MAX];
    int      first_in;     /* seconds since midnight, ATT_NO_TIME if none */
    int      last_in;
    int      last_out;
    int      last_scan;
    int      scans;
    bool     present;
} att_record_t;

typedef struct {
    att_dir_t dir;         /* direction recorded (or current state if repeat) */
    bool      repeat;      /* scanned again within the min-gap: nothing logged */
    int       time_sec;    /* seconds since midnight of this scan */
    int       prev_sec;    /* time of this student's previous event today (ATT_NO_TIME if none) */
    int       next_ok_sec; /* repeat: earliest time the next IN/OUT is accepted */
    bool      log_ok;      /* false if writing the SD log failed */
} att_result_t;

esp_err_t attendance_init(void);

/* Register a scan for a student (reads RTC for the timestamp). */
att_result_t attendance_register(const student_t *s);

/* Call periodically (e.g. each second) — resets state at midnight. */
void attendance_tick(const struct tm *now);

int attendance_present_count(void);

/* Copy today's records (students seen today), most recent activity first. */
int attendance_get_records(att_record_t *out, int max);

/* Log AUTO_OUT for everyone still inside. Returns how many were closed. */
int attendance_mark_all_out(void);

/* Format seconds-since-midnight as HH:MM. */
void attendance_fmt_time(int sec, char out[6]);
