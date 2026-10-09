#pragma once

/*
 * Student registry, persisted to /sd/students.csv (UTF-8 with BOM so Excel
 * shows Hebrew names correctly):
 *
 *     id,finger1,finger2,name
 *     1,0,-1,ישראל ישראלי
 *
 * finger1/finger2 are template slots in the sensor (-1 = not enrolled).
 * All functions are thread-safe.
 */

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define STUDENT_MAX          300
#define STUDENT_NAME_MAX     64
#define STUDENT_FINGERS      2
#define STUDENT_NO_SLOT      (-1)

typedef struct {
    uint16_t id;
    int16_t  finger[STUDENT_FINGERS];   /* sensor slot or STUDENT_NO_SLOT */
    char     name[STUDENT_NAME_MAX];
} student_t;

esp_err_t student_db_load(void);

int  student_db_count(void);

/* Copy the student at list position `index` (0..count-1). */
bool student_db_get_at(int index, student_t *out);

bool student_db_get(uint16_t id, student_t *out);

/* Find the student owning a sensor template slot. */
bool student_db_find_by_slot(int slot, student_t *out);

/* True if any student references this slot. */
bool student_db_slot_in_use(int slot);

/* Add a student; returns the new id via out_id. Saves to SD. */
esp_err_t student_db_add(const char *name, uint16_t *out_id);

esp_err_t student_db_rename(uint16_t id, const char *name);

/* Assign a sensor slot to finger index 0/1 (STUDENT_NO_SLOT to clear).
 * Returns the previous slot via out_old_slot (may be NULL). Saves to SD. */
esp_err_t student_db_set_finger(uint16_t id, int finger_idx, int slot, int *out_old_slot);

/* Clear every finger reference (after the sensor library was emptied). */
esp_err_t student_db_clear_all_fingers(void);

/* Remove a student; returns their slots so the caller can delete templates. */
esp_err_t student_db_remove(uint16_t id, int out_slots[STUDENT_FINGERS]);
