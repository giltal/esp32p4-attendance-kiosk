#pragma once

/*
 * Driver for ID809-based capacitive fingerprint modules (DFRobot SEN0348 and
 * clones: round 21 mm sensor, breathing RGB ring, 80 templates).
 * Protocol taken from DFRobot's official library (github.com/DFRobot/DFRobot_ID809):
 * 115200 8N1, fixed 26-byte command/response frames, prefix AA 55, little-endian.
 *
 * Template IDs are 1..capacity. All functions are blocking and NOT thread-safe —
 * call them from a single task (fp_service owns the sensor).
 */

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* Result codes (module error codes, plus FP_ERR_COMM for driver-level failures) */
#define FP_OK                   0x00
#define FP_ERR_FAIL             0x01
#define FP_ERR_NO_MATCH         0x11   /* 1:N search found nothing */
#define FP_ERR_TMPL_EMPTY       0x12
#define FP_ERR_LIBRARY_EMPTY    0x14
#define FP_ERR_LIBRARY_FULL     0x15   /* no free ID */
#define FP_ERR_DUPLICATE        0x18
#define FP_ERR_BAD_QUALITY      0x19
#define FP_ERR_MERGE_FAIL       0x1A
#define FP_ERR_INVALID_PARAM    0x22
#define FP_ERR_CAPTURE_TIMEOUT  0x23
#define FP_ERR_NO_FINGER        0x28
#define FP_ERR_COMM             0xFF   /* no / bad reply */

typedef enum {
    FP_LED_BREATHING = 1,
    FP_LED_FAST_BLINK,
    FP_LED_ON,
    FP_LED_OFF,
    FP_LED_FADE_IN,
    FP_LED_FADE_OUT,
    FP_LED_SLOW_BLINK,
} fp_led_mode_t;

typedef enum {
    FP_COLOR_GREEN = 1,
    FP_COLOR_RED,
    FP_COLOR_YELLOW,
    FP_COLOR_BLUE,
    FP_COLOR_CYAN,
    FP_COLOR_MAGENTA,
    FP_COLOR_WHITE,
} fp_led_color_t;

/* Initialise UART and handshake. Auto-detects swapped TX/RX and baud rate. */
esp_err_t fp_sensor_init(void);

/* Template library size reported by the module (80 or 200). */
uint16_t fp_sensor_capacity(void);

/* Is a finger on the sensor right now? */
uint8_t fp_sensor_finger_present(bool *present);

/* Capture an image of the finger currently on the sensor. */
uint8_t fp_sensor_capture(void);

/* Turn the captured image into a template in RAM buffer 0..2. */
uint8_t fp_sensor_generate(uint8_t ram_buf);

/* Merge templates in RAM buffers 0..count-1 into one (left in buffer 0). */
uint8_t fp_sensor_merge(uint8_t count);

/* Store the template in RAM buffer 0 at library ID. */
uint8_t fp_sensor_store(uint16_t id);

/* 1:N search of RAM buffer 0 against the whole library. FP_ERR_NO_MATCH if none. */
uint8_t fp_sensor_search(uint16_t *out_id);

/* Delete templates first..last (inclusive). */
uint8_t fp_sensor_delete(uint16_t first, uint16_t last);

/* Number of stored templates. */
uint8_t fp_sensor_template_count(uint16_t *out_count);

/* First free ID >= from. FP_ERR_LIBRARY_FULL if none. */
uint8_t fp_sensor_free_id(uint16_t from, uint16_t *out_id);

/* Occupancy bitmap: bit n of byte n/8 set = ID n enrolled. Returns bytes via out_len. */
uint8_t fp_sensor_enrolled_bitmap(uint8_t *bitmap, int max_bytes, int *out_len);

/* Ring LED. count = number of blinks (0 = forever) for breathing/blink modes. */
uint8_t fp_sensor_led(fp_led_mode_t mode, fp_led_color_t color, uint8_t count);

/* Human-readable text for a result code (English, for logs). */
const char *fp_sensor_err_str(uint8_t code);
