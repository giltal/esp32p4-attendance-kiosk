#include "student_db.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static const char *TAG = "STUDENT_DB";

#define DB_PATH "/sd/students.csv"
#define DB_TMP  "/sd/students.tmp"
#define UTF8_BOM "\xEF\xBB\xBF"

EXT_RAM_BSS_ATTR static student_t s_students[STUDENT_MAX];
static int s_count = 0;
static uint16_t s_next_id = 1;
static SemaphoreHandle_t s_lock;

#define LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(s_lock)

/* Commas/quotes/newlines would break the CSV — replace them with spaces. */
static void sanitize_name(char *dst, const char *src)
{
    strncpy(dst, src, STUDENT_NAME_MAX - 1);
    dst[STUDENT_NAME_MAX - 1] = '\0';
    for (char *p = dst; *p; p++) {
        if (*p == ',' || *p == '\n' || *p == '\r' || *p == '"') *p = ' ';
    }
}

static int index_of(uint16_t id)
{
    for (int i = 0; i < s_count; i++) {
        if (s_students[i].id == id) return i;
    }
    return -1;
}

static esp_err_t save_locked(void)
{
    FILE *f = fopen(DB_TMP, "w");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed: errno=%d", DB_TMP, errno);
        return ESP_FAIL;
    }
    fputs(UTF8_BOM "id,finger1,finger2,name\n", f);
    for (int i = 0; i < s_count; i++) {
        const student_t *s = &s_students[i];
        fprintf(f, "%u,%d,%d,%s\n", s->id, s->finger[0], s->finger[1], s->name);
    }
    int fl = fflush(f);
    int cl = fclose(f);
    if (fl != 0 || cl != 0) {
        ESP_LOGE(TAG, "write failed");
        remove(DB_TMP);
        return ESP_FAIL;
    }
    remove(DB_PATH);   /* FAT rename() won't overwrite */
    if (rename(DB_TMP, DB_PATH) != 0) {
        ESP_LOGE(TAG, "rename failed: errno=%d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t student_db_load(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();
    LOCK();
    s_count = 0;
    s_next_id = 1;

    FILE *f = fopen(DB_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "No %s yet — starting with an empty registry", DB_PATH);
        UNLOCK();
        return ESP_OK;
    }

    char line[160];
    while (fgets(line, sizeof(line), f) && s_count < STUDENT_MAX) {
        char *p = line;
        if (strncmp(p, UTF8_BOM, 3) == 0) p += 3;
        p[strcspn(p, "\r\n")] = '\0';
        if (*p < '0' || *p > '9') continue;   /* header / blank */

        student_t s = {0};
        char *end;
        s.id = strtoul(p, &end, 10);
        if (*end != ',') continue;
        s.finger[0] = strtol(end + 1, &end, 10);
        if (*end != ',') continue;
        s.finger[1] = strtol(end + 1, &end, 10);
        if (*end != ',') continue;
        sanitize_name(s.name, end + 1);
        if (s.id == 0 || index_of(s.id) >= 0) continue;

        s_students[s_count++] = s;
        if (s.id >= s_next_id) s_next_id = s.id + 1;
    }
    fclose(f);
    ESP_LOGI(TAG, "Loaded %d students", s_count);
    UNLOCK();
    return ESP_OK;
}

int student_db_count(void)
{
    return s_count;
}

bool student_db_get_at(int index, student_t *out)
{
    LOCK();
    bool ok = index >= 0 && index < s_count;
    if (ok) *out = s_students[index];
    UNLOCK();
    return ok;
}

bool student_db_get(uint16_t id, student_t *out)
{
    LOCK();
    int i = index_of(id);
    if (i >= 0 && out) *out = s_students[i];
    UNLOCK();
    return i >= 0;
}

bool student_db_find_by_slot(int slot, student_t *out)
{
    bool found = false;
    LOCK();
    for (int i = 0; i < s_count && !found; i++) {
        for (int k = 0; k < STUDENT_FINGERS; k++) {
            if (s_students[i].finger[k] == slot) {
                if (out) *out = s_students[i];
                found = true;
                break;
            }
        }
    }
    UNLOCK();
    return found;
}

bool student_db_slot_in_use(int slot)
{
    return student_db_find_by_slot(slot, NULL);
}

esp_err_t student_db_add(const char *name, uint16_t *out_id)
{
    LOCK();
    if (s_count >= STUDENT_MAX) {
        UNLOCK();
        return ESP_ERR_NO_MEM;
    }
    student_t *s = &s_students[s_count];
    memset(s, 0, sizeof(*s));
    s->id = s_next_id++;
    s->finger[0] = STUDENT_NO_SLOT;
    s->finger[1] = STUDENT_NO_SLOT;
    sanitize_name(s->name, name);
    s_count++;
    if (out_id) *out_id = s->id;
    ESP_LOGI(TAG, "Added #%u '%s'", s->id, s->name);
    esp_err_t ret = save_locked();
    UNLOCK();
    return ret;
}

esp_err_t student_db_rename(uint16_t id, const char *name)
{
    LOCK();
    int i = index_of(id);
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    if (i >= 0) {
        sanitize_name(s_students[i].name, name);
        ret = save_locked();
    }
    UNLOCK();
    return ret;
}

esp_err_t student_db_set_finger(uint16_t id, int finger_idx, int slot, int *out_old_slot)
{
    if (finger_idx < 0 || finger_idx >= STUDENT_FINGERS) return ESP_ERR_INVALID_ARG;
    LOCK();
    int i = index_of(id);
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    if (i >= 0) {
        if (out_old_slot) *out_old_slot = s_students[i].finger[finger_idx];
        s_students[i].finger[finger_idx] = slot;
        ret = save_locked();
    }
    UNLOCK();
    return ret;
}

esp_err_t student_db_clear_all_fingers(void)
{
    LOCK();
    for (int i = 0; i < s_count; i++) {
        for (int k = 0; k < STUDENT_FINGERS; k++) s_students[i].finger[k] = STUDENT_NO_SLOT;
    }
    esp_err_t ret = save_locked();
    UNLOCK();
    return ret;
}

esp_err_t student_db_remove(uint16_t id, int out_slots[STUDENT_FINGERS])
{
    LOCK();
    int i = index_of(id);
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    if (i >= 0) {
        for (int k = 0; k < STUDENT_FINGERS; k++) out_slots[k] = s_students[i].finger[k];
        ESP_LOGI(TAG, "Removing #%u '%s'", id, s_students[i].name);
        memmove(&s_students[i], &s_students[i + 1], (s_count - i - 1) * sizeof(student_t));
        s_count--;
        ret = save_locked();
    }
    UNLOCK();
    return ret;
}
