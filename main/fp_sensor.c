#include "fp_sensor.h"
#include "board_config.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "FP_SENSOR";

/* Frame types */
#define FRAME_RSP   0       /* response:      AA 55 */
#define FRAME_DATA  1       /* data response: A5 5A */

/* Commands (subset of DFRobot_ID809) */
#define CMD_TEST_CONNECTION      0x0001
#define CMD_DEVICE_INFO          0x0004
#define CMD_GET_IMAGE            0x0020
#define CMD_FINGER_DETECT        0x0021
#define CMD_SLED_CTRL            0x0024
#define CMD_STORE_CHAR           0x0040
#define CMD_DEL_CHAR             0x0044
#define CMD_GET_EMPTY_ID         0x0045
#define CMD_GET_ENROLL_COUNT     0x0048
#define CMD_GET_ENROLLED_ID_LIST 0x0049
#define CMD_GENERATE             0x0060
#define CMD_MERGE                0x0061
#define CMD_SEARCH               0x0063

#define FRAME_LEN        26
#define FRAME_DATA_LEN   16     /* command data field (zero-padded) */
#define RSP_DATA_LEN     14     /* response data field */
#define MAX_DATA_PKT     256

#define TIMEOUT_SHORT_MS 500
#define TIMEOUT_LONG_MS  2500   /* capture / search / merge / store / delete */

static bool s_ready = false;
static uint16_t s_capacity = 80;

/* Pin/baud actually in use (auto-detected; may differ from board_config.h) */
static int s_tx_gpio = FP_UART_TX_GPIO;
static int s_rx_gpio = FP_UART_RX_GPIO;
static int s_baud    = FP_UART_BAUD;

/* ── Frame I/O ──────────────────────────────────────────────────────────── */

static void send_cmd(uint16_t cmd, const uint8_t *data, uint16_t len)
{
    uint8_t f[FRAME_LEN] = {0};
    f[0] = 0xAA; f[1] = 0x55;          /* prefix */
    f[2] = 0;    f[3] = 0;             /* SID, DID */
    f[4] = cmd & 0xFF; f[5] = cmd >> 8;
    f[6] = len & 0xFF; f[7] = len >> 8;
    if (len) memcpy(&f[8], data, len > FRAME_DATA_LEN ? FRAME_DATA_LEN : len);

    uint16_t cks = 0;                  /* sum of every byte before the checksum */
    for (int i = 0; i < 8 + FRAME_DATA_LEN; i++) cks += f[i];
    f[24] = cks & 0xFF; f[25] = cks >> 8;

    uart_flush_input(FP_UART_NUM);
    vTaskDelay(pdMS_TO_TICKS(5));      /* the module wants a short gap between frames */
    uart_write_bytes(FP_UART_NUM, f, sizeof(f));
}

static bool read_exact(uint8_t *buf, int len, TickType_t deadline)
{
    int got = 0;
    while (got < len) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) return false;
        int n = uart_read_bytes(FP_UART_NUM, buf + got, len - got, deadline - now);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

/*
 * Receive one response (AA 55) or data response (A5 5A) frame.
 * Returns FRAME_RSP / FRAME_DATA, or -1 on timeout / bad frame.
 * *ret = module result code; data = payload (data_len bytes).
 */
static int recv_frame(uint8_t *ret, uint8_t *data, int max, int *data_len, int timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    uint8_t b, prev = 0;
    int type = -1;

    while (type < 0) {
        if (!read_exact(&b, 1, deadline)) return -1;
        if (prev == 0xAA && b == 0x55)      type = FRAME_RSP;
        else if (prev == 0xA5 && b == 0x5A) type = FRAME_DATA;
        prev = b;
    }

    uint8_t h[8];   /* SID DID RCM(2) LEN(2) RET(2) */
    if (!read_exact(h, sizeof(h), deadline)) return -1;
    uint16_t len = h[4] | (h[5] << 8);
    if (len < 2) return -1;

    /* Response frames have a fixed 14-byte data field; data frames carry LEN-2 bytes */
    int field = (type == FRAME_RSP) ? RSP_DATA_LEN : len - 2;
    int payload = len - 2;
    if (field > MAX_DATA_PKT || payload > field) return -1;

    uint8_t body[MAX_DATA_PKT + 2];
    if (!read_exact(body, field + 2, deadline)) return -1;

    uint16_t cks = (type == FRAME_RSP) ? (0xAA + 0x55) : (0xA5 + 0x5A);
    for (int i = 0; i < 8; i++) cks += h[i];
    for (int i = 0; i < payload; i++) cks += body[i];
    uint16_t rx_cks = body[field] | (body[field + 1] << 8);
    if (cks != rx_cks) {
        ESP_LOGW(TAG, "checksum mismatch (got %04X, want %04X)", rx_cks, cks);
        return -1;
    }

    *ret = h[6];
    int n = payload < max ? payload : max;
    if (data && n > 0) memcpy(data, body, n);
    if (data_len) *data_len = n;
    return type;
}

/* Send a command and return the module's result code; optional reply data. */
static uint8_t transact(uint16_t cmd, const uint8_t *data, uint16_t len,
                        uint8_t *reply, int reply_max, int timeout_ms)
{
    uint8_t ret;
    uint8_t buf[RSP_DATA_LEN];
    int n = 0;
    send_cmd(cmd, data, len);
    if (recv_frame(&ret, buf, sizeof(buf), &n, timeout_ms) != FRAME_RSP) return FP_ERR_COMM;
    if (reply && reply_max > 0) {
        memset(reply, 0, reply_max);
        memcpy(reply, buf, n < reply_max ? n : reply_max);
    }
    return ret;
}

/* Commands answered by a response (with the length) followed by a data frame. */
static uint8_t transact_data(uint16_t cmd, uint8_t *out, int max, int *out_len)
{
    uint8_t ret;
    uint8_t buf[RSP_DATA_LEN];
    send_cmd(cmd, NULL, 0);
    if (recv_frame(&ret, buf, sizeof(buf), NULL, TIMEOUT_SHORT_MS) != FRAME_RSP) return FP_ERR_COMM;
    if (ret != FP_OK) return ret;
    if (recv_frame(&ret, out, max, out_len, TIMEOUT_SHORT_MS) != FRAME_DATA) return FP_ERR_COMM;
    return ret;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static uint16_t get16(const uint8_t *p)  { return p[0] | (p[1] << 8); }

/* ── Link setup / auto-detect ───────────────────────────────────────────── */

static void uart_apply(int tx, int rx, int baud)
{
    /* Detach the previous pins first so a swapped TX can't keep driving the new RX */
    gpio_reset_pin(s_tx_gpio);
    gpio_reset_pin(s_rx_gpio);
    uart_set_baudrate(FP_UART_NUM, baud);
    uart_set_pin(FP_UART_NUM, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    gpio_pullup_en(rx);   /* keep RX idle-high when unplugged */
    s_tx_gpio = tx;
    s_rx_gpio = rx;
    s_baud = baud;
}

static bool test_connection(void)
{
    return transact(CMD_TEST_CONNECTION, NULL, 0, NULL, 0, 300) == FP_OK;
}

static void read_device_info(void)
{
    uint8_t info[64] = {0};
    int n = 0;
    if (transact_data(CMD_DEVICE_INFO, info, sizeof(info) - 1, &n) == FP_OK && n > 0) {
        info[n] = '\0';
        int last = (int)strnlen((char *)info, n) - 1;
        s_capacity = (last >= 0 && info[last] == '3') ? 200 : 80;   /* per DFRobot library */
        ESP_LOGI(TAG, "Device: '%s' -> capacity %u", (char *)info, s_capacity);
    } else {
        ESP_LOGW(TAG, "Device info unavailable — assuming capacity %u", s_capacity);
    }
}

esp_err_t fp_sensor_init(void)
{
    s_ready = false;
    if (!uart_is_driver_installed(FP_UART_NUM)) {
        const uart_config_t cfg = {
            .baud_rate  = FP_UART_BAUD,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        ESP_RETURN_ON_ERROR(uart_driver_install(FP_UART_NUM, 1024, 0, 0, NULL, 0),
                            TAG, "uart install");
        ESP_RETURN_ON_ERROR(uart_param_config(FP_UART_NUM, &cfg), TAG, "uart config");
        uart_apply(FP_UART_TX_GPIO, FP_UART_RX_GPIO, FP_UART_BAUD);
        vTaskDelay(pdMS_TO_TICKS(300));   /* module boot time */
    }

    /* Fast path: last known-good (or configured) settings */
    bool found = test_connection() || test_connection();

    /* Auto-detect: both wire orders x supported baud rates */
    if (!found) {
        static const int bauds[] = { 115200, 57600, 9600, 19200, 38400 };
        const int pins[2][2] = { { FP_UART_TX_GPIO, FP_UART_RX_GPIO },
                                 { FP_UART_RX_GPIO, FP_UART_TX_GPIO } };
        for (int p = 0; p < 2 && !found; p++) {
            for (int b = 0; b < (int)(sizeof(bauds) / sizeof(bauds[0])) && !found; b++) {
                uart_apply(pins[p][0], pins[p][1], bauds[b]);
                vTaskDelay(pdMS_TO_TICKS(20));
                found = test_connection();
                ESP_LOGI(TAG, "probe TX=%d RX=%d %6d baud: %s",
                         pins[p][0], pins[p][1], bauds[b], found ? "OK" : "no reply");
            }
        }
        if (!found) {
            uart_apply(FP_UART_TX_GPIO, FP_UART_RX_GPIO, FP_UART_BAUD);
            ESP_LOGE(TAG, "Sensor not found on GPIO%d/GPIO%d — check power/wiring",
                     FP_UART_TX_GPIO, FP_UART_RX_GPIO);
            return ESP_FAIL;
        }
        if (s_tx_gpio != FP_UART_TX_GPIO) {
            ESP_LOGW(TAG, "Sensor wires are swapped — using TX=GPIO%d RX=GPIO%d", s_tx_gpio, s_rx_gpio);
        }
    }

    ESP_LOGI(TAG, "Sensor connected (TX=GPIO%d RX=GPIO%d %d baud)", s_tx_gpio, s_rx_gpio, s_baud);
    read_device_info();
    s_ready = true;
    return ESP_OK;
}

uint16_t fp_sensor_capacity(void)
{
    return s_capacity;
}

/* ── Commands ───────────────────────────────────────────────────────────── */

uint8_t fp_sensor_finger_present(bool *present)
{
    uint8_t r[2];
    uint8_t rc = transact(CMD_FINGER_DETECT, NULL, 0, r, sizeof(r), TIMEOUT_SHORT_MS);
    if (present) *present = (rc == FP_OK && r[0] != 0);
    return rc;
}

uint8_t fp_sensor_capture(void)
{
    return transact(CMD_GET_IMAGE, NULL, 0, NULL, 0, TIMEOUT_LONG_MS);
}

uint8_t fp_sensor_generate(uint8_t ram_buf)
{
    uint8_t d[2];
    put16(d, ram_buf);
    return transact(CMD_GENERATE, d, sizeof(d), NULL, 0, TIMEOUT_LONG_MS);
}

uint8_t fp_sensor_merge(uint8_t count)
{
    uint8_t d[3] = { 0, 0, count };   /* result into RAM buffer 0 */
    return transact(CMD_MERGE, d, sizeof(d), NULL, 0, TIMEOUT_LONG_MS);
}

uint8_t fp_sensor_store(uint16_t id)
{
    uint8_t d[4];
    put16(&d[0], id);
    put16(&d[2], 0);    /* from RAM buffer 0 */
    return transact(CMD_STORE_CHAR, d, sizeof(d), NULL, 0, TIMEOUT_LONG_MS);
}

uint8_t fp_sensor_search(uint16_t *out_id)
{
    uint8_t d[6], r[2];
    put16(&d[0], 0);            /* RAM buffer 0 */
    put16(&d[2], 1);            /* first ID */
    put16(&d[4], s_capacity);   /* last ID */
    uint8_t rc = transact(CMD_SEARCH, d, sizeof(d), r, sizeof(r), TIMEOUT_LONG_MS);
    if (rc == FP_OK && out_id) *out_id = get16(r);
    return rc;
}

uint8_t fp_sensor_delete(uint16_t first, uint16_t last)
{
    uint8_t d[4];
    put16(&d[0], first);
    put16(&d[2], last);
    return transact(CMD_DEL_CHAR, d, sizeof(d), NULL, 0, TIMEOUT_LONG_MS);
}

uint8_t fp_sensor_template_count(uint16_t *out_count)
{
    uint8_t d[4], r[2];
    put16(&d[0], 1);
    put16(&d[2], s_capacity);
    uint8_t rc = transact(CMD_GET_ENROLL_COUNT, d, sizeof(d), r, sizeof(r), TIMEOUT_SHORT_MS);
    if (rc == FP_ERR_LIBRARY_EMPTY) {   /* "no templates" is reported as an error */
        r[0] = r[1] = 0;
        rc = FP_OK;
    }
    if (out_count) *out_count = (rc == FP_OK) ? get16(r) : 0;
    return rc;
}

uint8_t fp_sensor_free_id(uint16_t from, uint16_t *out_id)
{
    uint8_t d[4], r[2];
    put16(&d[0], from);
    put16(&d[2], s_capacity);
    uint8_t rc = transact(CMD_GET_EMPTY_ID, d, sizeof(d), r, sizeof(r), TIMEOUT_SHORT_MS);
    if (rc == FP_OK && out_id) *out_id = get16(r);
    return rc;
}

uint8_t fp_sensor_enrolled_bitmap(uint8_t *bitmap, int max_bytes, int *out_len)
{
    return transact_data(CMD_GET_ENROLLED_ID_LIST, bitmap, max_bytes, out_len);
}

uint8_t fp_sensor_led(fp_led_mode_t mode, fp_led_color_t color, uint8_t count)
{
    if (!s_ready) return FP_ERR_COMM;
    uint8_t d[4];
    if (s_capacity == 80) {
        d[0] = mode;
        d[1] = d[2] = color;
    } else {
        /* 200-template variant uses different codes (per DFRobot library) */
        static const uint8_t mode_map[]  = { 0, 2, 4, 1, 0, 3, 3, 4 };
        static const uint8_t color_map[] = { 0, 0x84, 0x82, 0x86, 0x81, 0x85, 0x83, 0x87 };
        d[0] = mode_map[mode];
        d[1] = d[2] = color_map[color];
    }
    d[3] = count;
    return transact(CMD_SLED_CTRL, d, sizeof(d), NULL, 0, TIMEOUT_SHORT_MS);
}

const char *fp_sensor_err_str(uint8_t code)
{
    switch (code) {
    case FP_OK:                  return "OK";
    case FP_ERR_FAIL:            return "command failed";
    case FP_ERR_NO_MATCH:        return "no match";
    case FP_ERR_TMPL_EMPTY:      return "ID empty";
    case FP_ERR_LIBRARY_EMPTY:   return "library empty";
    case FP_ERR_LIBRARY_FULL:    return "library full";
    case FP_ERR_DUPLICATE:       return "fingerprint already enrolled";
    case FP_ERR_BAD_QUALITY:     return "poor image quality";
    case FP_ERR_MERGE_FAIL:      return "captures did not match";
    case FP_ERR_INVALID_PARAM:   return "invalid parameter";
    case FP_ERR_CAPTURE_TIMEOUT: return "capture timeout";
    case FP_ERR_NO_FINGER:       return "no finger";
    case FP_ERR_COMM:            return "no response";
    default:                     return "error";
    }
}
