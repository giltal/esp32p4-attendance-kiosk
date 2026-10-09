#include "fp_service.h"
#include "fp_sensor.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "FP_SERVICE";

typedef enum {
    REQ_ENROLL,
    REQ_CANCEL,
    REQ_DELETE_SLOT,
    REQ_WIPE,
    REQ_CLEANUP,
    REQ_STATUS,
} req_type_t;

typedef struct {
    req_type_t type;
    uint16_t   student_id;
    int        arg;          /* finger index / slot */
} fp_req_t;

#define ENROLL_CAPTURES    3       /* ID809 merges up to 3 captures per template */
#define FINGER_WAIT_MS     30000   /* enrollment: wait for finger */
#define LIFT_WAIT_MS       10000
#define CAPTURE_RETRIES    3
#define POLL_MS            100
#define RECONNECT_MS       10000
#define MAX_COMM_FAILS     5

static QueueHandle_t s_queue;
static fp_event_cb_t s_cb;
static volatile bool s_scanning = true;
static bool s_sensor_ok = false;

/* ── Helpers ────────────────────────────────────────────────────────────── */

static void emit(const fp_event_t *e)
{
    if (s_cb) s_cb(e);
}

static void led_idle(void)
{
    if (s_scanning) fp_sensor_led(FP_LED_BREATHING, FP_COLOR_BLUE, 0);
    else            fp_sensor_led(FP_LED_OFF, FP_COLOR_BLUE, 0);
}

static void led_error(void)
{
    fp_sensor_led(FP_LED_FAST_BLINK, FP_COLOR_RED, 3);
}

static void beep_ok(bool in)
{
    if (in) { bsp_audio_play_tone(880, 90);  bsp_audio_play_tone(1320, 140); }
    else    { bsp_audio_play_tone(1320, 90); bsp_audio_play_tone(880, 140); }
}

static void beep_err(void)
{
    bsp_audio_play_tone(300, 350);
}

static void emit_status(void)
{
    fp_event_t e = { .type = FP_EVT_SENSOR_STATUS, .ok = s_sensor_ok,
                     .capacity = fp_sensor_capacity() };
    if (s_sensor_ok) fp_sensor_template_count(&e.templates);
    emit(&e);
}

static void connect_sensor(void)
{
    s_sensor_ok = (fp_sensor_init() == ESP_OK);
    if (s_sensor_ok) led_idle();
    emit_status();
}

static bool finger_present(void)
{
    bool present = false;
    fp_sensor_finger_present(&present);
    return present;
}

/* Check the queue for a cancel request while inside a blocking flow. */
static bool cancel_requested(void)
{
    fp_req_t req;
    while (xQueueReceive(s_queue, &req, 0) == pdTRUE) {
        if (req.type == REQ_CANCEL) return true;
        ESP_LOGW(TAG, "Request %d dropped — enrollment in progress", req.type);
    }
    return false;
}

/* Wait until a finger is on the sensor. */
static fp_enroll_result_t wait_finger(int timeout_ms)
{
    for (int t = 0; t < timeout_ms; t += POLL_MS) {
        if (cancel_requested()) return FP_ENROLL_CANCELLED;
        bool present = false;
        if (fp_sensor_finger_present(&present) == FP_ERR_COMM) return FP_ENROLL_SENSOR_ERR;
        if (present) return FP_ENROLL_OK;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    return FP_ENROLL_TIMEOUT;
}

/* Wait until the finger is lifted. */
static void wait_lift(int timeout_ms)
{
    for (int t = 0; t < timeout_ms && finger_present(); t += POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

/* Capture a finger into RAM buffer `ram`, retrying on poor images.
 * Emits ENROLL_STEP 1 with count = capture number (1..ENROLL_CAPTURES). */
static fp_enroll_result_t capture(uint8_t ram)
{
    for (int attempt = 0; attempt < CAPTURE_RETRIES; attempt++) {
        fp_event_t e = { .type = FP_EVT_ENROLL_STEP, .step = 1, .count = ram + 1,
                         .ok = (attempt == 0) };
        emit(&e);   /* ok=false → "try again" */

        fp_enroll_result_t r = wait_finger(FINGER_WAIT_MS);
        if (r != FP_ENROLL_OK) return r;

        uint8_t rc = fp_sensor_capture();
        if (rc == FP_OK) rc = fp_sensor_generate(ram);
        if (rc == FP_OK) return FP_ENROLL_OK;
        if (rc == FP_ERR_COMM) return FP_ENROLL_SENSOR_ERR;

        ESP_LOGW(TAG, "capture %d: %s — retry", ram + 1, fp_sensor_err_str(rc));
        led_error();
        beep_err();
        wait_lift(LIFT_WAIT_MS);
        fp_sensor_led(FP_LED_BREATHING, FP_COLOR_MAGENTA, 0);
    }
    return FP_ENROLL_MISMATCH;
}

/* First library ID that is empty on the sensor and unused in the DB. */
static int find_free_slot(void)
{
    uint16_t from = 1;
    while (from <= fp_sensor_capacity()) {
        uint16_t id;
        if (fp_sensor_free_id(from, &id) != FP_OK) return -1;
        if (!student_db_slot_in_use(id)) return id;
        from = id + 1;
    }
    return -1;
}

/* ── Enrollment ─────────────────────────────────────────────────────────── */

static fp_enroll_result_t do_enroll(uint16_t student_id, int finger_idx, student_t *dup)
{
    int slot = find_free_slot();
    if (slot < 0) return FP_ENROLL_FULL;

    fp_sensor_led(FP_LED_BREATHING, FP_COLOR_MAGENTA, 0);

    for (int i = 0; i < ENROLL_CAPTURES; i++) {
        fp_enroll_result_t r = capture(i);
        if (r != FP_ENROLL_OK) return r;

        if (i == 0) {
            /* Reject a finger that is already enrolled */
            uint16_t found;
            if (fp_sensor_search(&found) == FP_OK) {
                if (student_db_find_by_slot(found, dup)) return FP_ENROLL_DUPLICATE;
                ESP_LOGW(TAG, "Removing orphan template at ID %u", found);
                fp_sensor_delete(found, found);
            }
        }

        fp_sensor_led(FP_LED_ON, FP_COLOR_GREEN, 0);
        bsp_audio_play_tone(1000, 80);
        if (i < ENROLL_CAPTURES - 1) {
            fp_event_t e = { .type = FP_EVT_ENROLL_STEP, .step = 2, .count = i + 1, .ok = true };
            emit(&e);
            wait_lift(LIFT_WAIT_MS);
            fp_sensor_led(FP_LED_BREATHING, FP_COLOR_MAGENTA, 0);
        }
    }

    fp_event_t e = { .type = FP_EVT_ENROLL_STEP, .step = 4, .ok = true };
    emit(&e);

    uint8_t rc = fp_sensor_merge(ENROLL_CAPTURES);
    if (rc == FP_ERR_MERGE_FAIL) return FP_ENROLL_MISMATCH;
    if (rc != FP_OK) return FP_ENROLL_SENSOR_ERR;

    rc = fp_sensor_store(slot);
    if (rc != FP_OK) {
        ESP_LOGE(TAG, "store ID %d: %s", slot, fp_sensor_err_str(rc));
        return FP_ENROLL_SENSOR_ERR;
    }

    int old_slot = STUDENT_NO_SLOT;
    if (student_db_set_finger(student_id, finger_idx, slot, &old_slot) != ESP_OK) {
        ESP_LOGW(TAG, "Student #%u vanished during enrollment — discarding template", student_id);
        fp_sensor_delete(slot, slot);
        return FP_ENROLL_SENSOR_ERR;
    }
    if (old_slot > 0 && old_slot != slot) {
        fp_sensor_delete(old_slot, old_slot);   /* replaced an older enrollment */
    }
    ESP_LOGI(TAG, "Enrolled student #%u finger %d -> ID %d", student_id, finger_idx, slot);
    return FP_ENROLL_OK;
}

static void handle_enroll(uint16_t student_id, int finger_idx)
{
    fp_event_t e = { .type = FP_EVT_ENROLL_DONE };

    if (!s_sensor_ok) {
        e.enroll_result = FP_ENROLL_SENSOR_ERR;
    } else {
        e.enroll_result = do_enroll(student_id, finger_idx, &e.student);
    }
    e.ok = (e.enroll_result == FP_ENROLL_OK);
    if (e.ok) {
        student_db_get(student_id, &e.student);
        fp_sensor_led(FP_LED_ON, FP_COLOR_GREEN, 0);
        beep_ok(true);
    } else if (e.enroll_result != FP_ENROLL_CANCELLED) {
        led_error();
        beep_err();
    }
    emit(&e);
    vTaskDelay(pdMS_TO_TICKS(800));
    led_idle();
    emit_status();
}

/* ── Maintenance ops ────────────────────────────────────────────────────── */

static void handle_cleanup(void)
{
    int removed = 0;
    uint8_t bitmap[64] = {0};
    int len = 0;
    uint8_t rc = fp_sensor_enrolled_bitmap(bitmap, sizeof(bitmap), &len);
    if (rc == FP_OK) {
        for (int id = 1; id < len * 8 && id <= fp_sensor_capacity(); id++) {
            if ((bitmap[id / 8] & (1 << (id % 8))) && !student_db_slot_in_use(id)) {
                if (fp_sensor_delete(id, id) == FP_OK) removed++;
            }
        }
    } else if (rc != FP_ERR_LIBRARY_EMPTY) {
        ESP_LOGW(TAG, "enrolled list: %s", fp_sensor_err_str(rc));
    }
    ESP_LOGI(TAG, "Orphan cleanup removed %d templates", removed);
    fp_event_t e = { .type = FP_EVT_OP_DONE, .step = FP_OP_CLEANUP, .ok = true, .count = removed };
    emit(&e);
}

static void handle_request(const fp_req_t *req)
{
    fp_event_t e = { .type = FP_EVT_OP_DONE };
    uint8_t rc;

    switch (req->type) {
    case REQ_ENROLL:
        handle_enroll(req->student_id, req->arg);
        return;
    case REQ_CANCEL:
        if (s_sensor_ok) led_idle();   /* nothing in progress — just sync the LED */
        return;
    case REQ_STATUS:
        connect_sensor();
        return;
    case REQ_DELETE_SLOT:
        e.step = FP_OP_DELETE;
        rc = s_sensor_ok ? fp_sensor_delete(req->arg, req->arg) : FP_ERR_COMM;
        e.ok = (rc == FP_OK || rc == FP_ERR_TMPL_EMPTY);
        e.count = e.ok ? 1 : 0;
        emit(&e);
        break;
    case REQ_WIPE:
        e.step = FP_OP_WIPE;
        rc = s_sensor_ok ? fp_sensor_delete(1, fp_sensor_capacity()) : FP_ERR_COMM;
        e.ok = (rc == FP_OK || rc == FP_ERR_LIBRARY_EMPTY);
        if (e.ok) student_db_clear_all_fingers();
        emit(&e);
        break;
    case REQ_CLEANUP:
        if (s_sensor_ok) handle_cleanup();
        break;
    }
    emit_status();
}

/* ── Attendance scanning ────────────────────────────────────────────────── */

static int s_comm_fails = 0;

static void scan_once(void)
{
    bool present = false;
    uint8_t rc = fp_sensor_finger_present(&present);
    if (rc == FP_ERR_COMM) {
        if (++s_comm_fails >= MAX_COMM_FAILS) {
            ESP_LOGE(TAG, "Sensor stopped responding");
            s_sensor_ok = false;
            emit_status();
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        return;
    }
    s_comm_fails = 0;
    if (!present) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        return;
    }

    fp_event_t e = {0};
    uint16_t id = 0;

    rc = fp_sensor_capture();
    if (rc == FP_OK) rc = fp_sensor_generate(0);
    if (rc != FP_OK) {
        ESP_LOGW(TAG, "capture: %s", fp_sensor_err_str(rc));
        e.type = FP_EVT_SCAN_BAD_IMAGE;
    } else if ((rc = fp_sensor_search(&id)) == FP_OK && student_db_find_by_slot(id, &e.student)) {
        e.type = FP_EVT_SCAN_MATCH;
        e.ok = true;
        e.att = attendance_register(&e.student);
        ESP_LOGI(TAG, "Match ID %u -> student #%u", id, e.student.id);
    } else {
        if (rc == FP_OK) ESP_LOGW(TAG, "ID %u matched but belongs to no student", id);
        e.type = FP_EVT_SCAN_UNKNOWN;
    }

    if (e.type != FP_EVT_SCAN_MATCH)  led_error();
    else if (e.att.repeat)            fp_sensor_led(FP_LED_ON, FP_COLOR_CYAN, 0);
    else if (e.att.dir == ATT_IN)     fp_sensor_led(FP_LED_ON, FP_COLOR_GREEN, 0);
    else                              fp_sensor_led(FP_LED_ON, FP_COLOR_YELLOW, 0);
    emit(&e);

    if (e.type == FP_EVT_SCAN_MATCH && !e.att.repeat) beep_ok(e.att.dir == ATT_IN);
    else if (e.type == FP_EVT_SCAN_MATCH)             bsp_audio_play_tone(1000, 80);
    else                                              beep_err();

    wait_lift(3000);
    led_idle();
}

/* ── Task ───────────────────────────────────────────────────────────────── */

static void fp_task(void *arg)
{
    connect_sensor();
    TickType_t last_reconnect = xTaskGetTickCount();

    for (;;) {
        fp_req_t req;
        TickType_t wait = (s_scanning && s_sensor_ok) ? 0 : pdMS_TO_TICKS(200);
        if (xQueueReceive(s_queue, &req, wait) == pdTRUE) {
            handle_request(&req);
            continue;
        }

        if (!s_sensor_ok) {
            if (xTaskGetTickCount() - last_reconnect >= pdMS_TO_TICKS(RECONNECT_MS)) {
                last_reconnect = xTaskGetTickCount();
                connect_sensor();
            }
            continue;
        }

        if (s_scanning) scan_once();
    }
}

esp_err_t fp_service_start(fp_event_cb_t cb)
{
    s_cb = cb;
    s_queue = xQueueCreate(8, sizeof(fp_req_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    BaseType_t ok = xTaskCreatePinnedToCore(fp_task, "fp_task", 8192, NULL, 4, NULL, 1);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static void post(req_type_t type, uint16_t id, int arg)
{
    fp_req_t req = { .type = type, .student_id = id, .arg = arg };
    if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Request queue full — request %d dropped", type);
    }
}

void fp_service_set_scanning(bool enabled)
{
    s_scanning = enabled;
    post(REQ_CANCEL, 0, 0);   /* wakes the task so the LED state updates promptly */
}

void fp_service_enroll(uint16_t student_id, int finger_idx) { post(REQ_ENROLL, student_id, finger_idx); }
void fp_service_cancel(void)                                { post(REQ_CANCEL, 0, 0); }
void fp_service_delete_slot(int slot)                       { if (slot > 0) post(REQ_DELETE_SLOT, 0, slot); }
void fp_service_wipe_all(void)                              { post(REQ_WIPE, 0, 0); }
void fp_service_cleanup_orphans(void)                       { post(REQ_CLEANUP, 0, 0); }
void fp_service_refresh_status(void)                        { post(REQ_STATUS, 0, 0); }

bool fp_service_sensor_ok(void)
{
    return s_sensor_ok;
}
