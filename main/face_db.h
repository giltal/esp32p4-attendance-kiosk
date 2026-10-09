#pragma once

/*
 * Face signature store, persisted to /sd/faces.bin (binary, atomic tmp+rename):
 *   header: "FDB1", uint16 feat_len, uint16 reserved, uint32 count
 *   record: uint16 student_id, uint16 reserved, float feat[feat_len]
 * Several signatures (samples) per student. No images are stored.
 * Thread-safe.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define FACE_DB_SAMPLES_PER_STUDENT  5

esp_err_t face_db_load(void);

/* Number of samples stored for a student (0 = no face enrolled). */
int face_db_count_for(uint16_t student_id);

/* Number of students with at least one sample. */
int face_db_student_count(void);

/* Replace all samples of a student. Saves to SD. */
esp_err_t face_db_set_student(uint16_t student_id, const float *feats, int n, int feat_len);

esp_err_t face_db_remove_student(uint16_t student_id);
esp_err_t face_db_clear(void);

/* Best match over all samples. Returns false if the DB is empty or the
 * signature length differs. out_sim is the best similarity (may be low). */
bool face_db_match(const float *feat, int feat_len, uint16_t exclude_id,
                   uint16_t *out_id, float *out_sim);
