#include "face_db.h"
#include "student_db.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

static const char *TAG = "FACE_DB";

#define DB_PATH   "/sd/faces.bin"
#define DB_TMP    "/sd/faces.tmp"
#define DB_MAGIC  "FDB1"
#define MAX_RECS  (STUDENT_MAX * FACE_DB_SAMPLES_PER_STUDENT)

typedef struct {
    uint16_t student_id;
    uint16_t reserved;
} rec_hdr_t;

static uint16_t s_feat_len;          /* 0 = unknown (empty DB) */
static int s_count;
static uint16_t *s_ids;              /* [MAX_RECS] */
static float *s_feats;               /* [MAX_RECS * s_feat_len], PSRAM */
static SemaphoreHandle_t s_lock;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static esp_err_t ensure_storage(uint16_t feat_len)
{
    if (s_feats && s_feat_len == feat_len) return ESP_OK;
    if (s_feats && s_count > 0) return ESP_ERR_INVALID_STATE;   /* length change with data */
    heap_caps_free(s_feats);
    s_feats = heap_caps_calloc((size_t)MAX_RECS * feat_len, sizeof(float), MALLOC_CAP_SPIRAM);
    if (!s_ids) s_ids = heap_caps_calloc(MAX_RECS, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_feats || !s_ids) return ESP_ERR_NO_MEM;
    s_feat_len = feat_len;
    return ESP_OK;
}

static esp_err_t save_locked(void)
{
    FILE *f = fopen(DB_TMP, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed: errno=%d", DB_TMP, errno);
        return ESP_FAIL;
    }
    uint16_t len = s_feat_len, res = 0;
    uint32_t count = s_count;
    bool ok = fwrite(DB_MAGIC, 1, 4, f) == 4 &&
              fwrite(&len, 2, 1, f) == 1 && fwrite(&res, 2, 1, f) == 1 &&
              fwrite(&count, 4, 1, f) == 1;
    for (int i = 0; ok && i < s_count; i++) {
        rec_hdr_t h = { .student_id = s_ids[i] };
        ok = fwrite(&h, sizeof(h), 1, f) == 1 &&
             fwrite(&s_feats[(size_t)i * len], sizeof(float), len, f) == len;
    }
    ok = (fflush(f) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        ESP_LOGE(TAG, "write failed");
        remove(DB_TMP);
        return ESP_FAIL;
    }
    remove(DB_PATH);
    if (rename(DB_TMP, DB_PATH) != 0) {
        ESP_LOGE(TAG, "rename failed: errno=%d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t face_db_load(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    LOCK();
    s_count = 0;

    FILE *f = fopen(DB_PATH, "rb");
    if (!f) {
        ESP_LOGI(TAG, "No %s yet", DB_PATH);
        UNLOCK();
        return ESP_OK;
    }

    char magic[4];
    uint16_t len = 0, res;
    uint32_t count = 0;
    esp_err_t ret = ESP_FAIL;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, DB_MAGIC, 4) != 0 ||
        fread(&len, 2, 1, f) != 1 || fread(&res, 2, 1, f) != 1 || fread(&count, 4, 1, f) != 1 ||
        len == 0 || len > 1024) {
        ESP_LOGE(TAG, "%s is corrupt — ignoring", DB_PATH);
        goto out;
    }
    if (ensure_storage(len) != ESP_OK) goto out;
    for (uint32_t i = 0; i < count && s_count < MAX_RECS; i++) {
        rec_hdr_t h;
        if (fread(&h, sizeof(h), 1, f) != 1 ||
            fread(&s_feats[(size_t)s_count * len], sizeof(float), len, f) != len) {
            ESP_LOGW(TAG, "truncated at record %lu", (unsigned long)i);
            break;
        }
        if (!student_db_get(h.student_id, NULL)) continue;   /* student was deleted */
        s_ids[s_count++] = h.student_id;
    }
    ret = ESP_OK;
    ESP_LOGI(TAG, "Loaded %d face samples (%d students, len %u)", s_count, face_db_student_count(), len);
out:
    fclose(f);
    UNLOCK();
    return ret;
}

int face_db_count_for(uint16_t student_id)
{
    if (!s_lock) return 0;
    int n = 0;
    LOCK();
    for (int i = 0; i < s_count; i++) if (s_ids[i] == student_id) n++;
    UNLOCK();
    return n;
}

int face_db_student_count(void)
{
    /* called with or without the lock; read-only scan */
    int n = 0;
    for (int i = 0; i < s_count; i++) {
        bool seen = false;
        for (int k = 0; k < i && !seen; k++) seen = (s_ids[k] == s_ids[i]);
        if (!seen) n++;
    }
    return n;
}

static void remove_locked(uint16_t student_id)
{
    int w = 0;
    for (int r = 0; r < s_count; r++) {
        if (s_ids[r] == student_id) continue;
        if (w != r) {
            s_ids[w] = s_ids[r];
            memcpy(&s_feats[(size_t)w * s_feat_len], &s_feats[(size_t)r * s_feat_len],
                   s_feat_len * sizeof(float));
        }
        w++;
    }
    s_count = w;
}

esp_err_t face_db_set_student(uint16_t student_id, const float *feats, int n, int feat_len)
{
    LOCK();
    esp_err_t ret = ensure_storage(feat_len);
    if (ret == ESP_OK) {
        remove_locked(student_id);
        for (int i = 0; i < n && s_count < MAX_RECS; i++) {
            s_ids[s_count] = student_id;
            memcpy(&s_feats[(size_t)s_count * feat_len], &feats[(size_t)i * feat_len],
                   feat_len * sizeof(float));
            s_count++;
        }
        ret = save_locked();
        ESP_LOGI(TAG, "Student #%u: %d face samples saved", student_id, n);
    } else {
        ESP_LOGE(TAG, "storage error: %s", esp_err_to_name(ret));
    }
    UNLOCK();
    return ret;
}

esp_err_t face_db_remove_student(uint16_t student_id)
{
    if (!s_lock) return ESP_OK;
    LOCK();
    int before = s_count;
    remove_locked(student_id);
    esp_err_t ret = (s_count != before) ? save_locked() : ESP_OK;
    UNLOCK();
    return ret;
}

esp_err_t face_db_clear(void)
{
    LOCK();
    s_count = 0;
    esp_err_t ret = s_feat_len ? save_locked() : ESP_OK;
    UNLOCK();
    return ret;
}

bool face_db_match(const float *feat, int feat_len, uint16_t exclude_id,
                   uint16_t *out_id, float *out_sim)
{
    bool found = false;
    float best = -2.0f;
    uint16_t best_id = 0;
    LOCK();
    if (s_count > 0 && feat_len == s_feat_len) {
        for (int r = 0; r < s_count; r++) {
            if (s_ids[r] == exclude_id) continue;
            const float *v = &s_feats[(size_t)r * feat_len];
            float sim = 0;
            for (int i = 0; i < feat_len; i++) sim += v[i] * feat[i];
            if (sim > best) {
                best = sim;
                best_id = s_ids[r];
                found = true;
            }
        }
    }
    UNLOCK();
    if (out_id) *out_id = best_id;
    if (out_sim) *out_sim = found ? best : 0;
    return found;
}
