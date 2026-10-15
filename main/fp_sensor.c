/*
 * GROW ("R30x / R503 / ZhianTec") protocol driver — used by the FPM383F
 * (57600 8N1). The module also answers the FPM383 "F1 1F E2 2E..." protocol,
 * but GROW maps 1:1 onto the capture / merge / store / search flow used by
 * fp_service.
 *
 * Packet: EF 01 | addr(4) | PID | LEN(2, BE, payload+2) | payload | SUM(2, BE)
 *         SUM = PID + LEN bytes + payload bytes. ACK PID = 0x07, payload[0] = code.
 *
 * Library IDs: fp_service uses 1..capacity-1 (page 0 is never used).
 */
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

#define PID_COMMAND   0x01
#define PID_ACK       0x07
#define ADDRESS       0xFFFFFFFFu

/* GROW instruction codes */
#define CMD_GEN_IMG        0x01
#define CMD_IMG2TZ         0x02
#define CMD_SEARCH         0x04
#define CMD_REG_MODEL      0x05
#define CMD_STORE          0x06
#define CMD_DELETE         0x0C
#define CMD_EMPTY          0x0D
#define CMD_READ_SYS_PARA  0x0F
#define CMD_VERIFY_PWD     0x13
#define CMD_TEMPLATE_NUM   0x1D
#define CMD_READ_INDEX     0x1F
#define CMD_AURA_LED       0x35

/* GROW confirmation codes */
#define G_OK               0x00
#define G_NO_FINGER        0x02
#define G_IMAGE_FAIL       0x03
#define G_IMAGE_MESSY      0x06
#define G_FEATURE_FAIL     0x07
#define G_NOT_FOUND        0x09
#define G_MERGE_FAIL       0x0A
#define G_BAD_LOCATION     0x0B
#define G_FLASH_ERR        0x18

#define MAX_PAYLOAD        64
#define TIMEOUT_SHORT_MS   600
#define TIMEOUT_LONG_MS    2500

static bool s_ready = false;
static uint16_t s_capacity = 50;

/* Pin/baud actually in use (auto-detected; may differ from board_config.h) */
static int s_tx_gpio = FP_UART_TX_GPIO;
static int s_rx_gpio = FP_UART_RX_GPIO;
static int s_baud    = FP_UART_BAUD;

/* ── Packet I/O ─────────────────────────────────────────────────────────── */

static void send_packet(const uint8_t *data, uint16_t len)
{
    uint8_t pkt[9 + MAX_PAYLOAD + 2];
    uint16_t plen = len + 2;
    pkt[0] = 0xEF; pkt[1] = 0x01;
    pkt[2] = ADDRESS >> 24; pkt[3] = ADDRESS >> 16; pkt[4] = ADDRESS >> 8; pkt[5] = ADDRESS;
    pkt[6] = PID_COMMAND;
    pkt[7] = plen >> 8; pkt[8] = plen & 0xFF;
    memcpy(&pkt[9], data, len);
    uint16_t sum = PID_COMMAND + (plen >> 8) + (plen & 0xFF);
    for (int i = 0; i < len; i++) sum += data[i];
    pkt[9 + len] = sum >> 8;
    pkt[10 + len] = sum & 0xFF;

    uart_flush_input(FP_UART_NUM);
    uart_write_bytes(FP_UART_NUM, pkt, 11 + len);
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

/* Receive an ACK packet; returns payload length (payload[0] = code) or -1. */
static int recv_ack(uint8_t *payload, int max_len, int timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        uint8_t b, prev = 0;
        for (;;) {                                   /* hunt for EF 01 */
            if (!read_exact(&b, 1, deadline)) return -1;
            if (prev == 0xEF && b == 0x01) break;
            prev = b;
        }
        uint8_t hdr[7];                              /* addr(4) pid len(2) */
        if (!read_exact(hdr, sizeof(hdr), deadline)) return -1;
        uint8_t pid = hdr[4];
        uint16_t plen = (hdr[5] << 8) | hdr[6];
        if (plen < 3 || plen > MAX_PAYLOAD + 2) return -1;
        uint8_t body[MAX_PAYLOAD + 2];
        if (!read_exact(body, plen, deadline)) return -1;
        if (pid == PID_COMMAND) continue;            /* our own frame echoed back — skip */
        if (pid != PID_ACK) return -1;

        uint16_t sum = pid + (plen >> 8) + (plen & 0xFF);
        for (int i = 0; i < plen - 2; i++) sum += body[i];
        if (sum != ((body[plen - 2] << 8) | body[plen - 1])) {
            ESP_LOGW(TAG, "checksum mismatch");
            return -1;
        }
        int n = plen - 2 < max_len ? plen - 2 : max_len;
        memcpy(payload, body, n);
        return n;
    }
}

/* Send a command; returns the GROW confirmation code (0xFF = no reply). */
static uint8_t transact(const uint8_t *cmd, uint16_t len, uint8_t *reply, int reply_len, int timeout_ms)
{
    uint8_t ack[MAX_PAYLOAD];
    send_packet(cmd, len);
    int n = recv_ack(ack, sizeof(ack), timeout_ms);
    if (n < 1) return 0xFF;
    if (reply && reply_len > 0) {
        memset(reply, 0, reply_len);
        memcpy(reply, &ack[1], (n - 1) < reply_len ? (n - 1) : reply_len);
    }
    return ack[0];
}

/* GROW confirmation code -> driver result code (fp_sensor.h) */
static uint8_t map_code(uint8_t g)
{
    switch (g) {
    case G_OK:            return FP_OK;
    case G_NO_FINGER:     return FP_ERR_NO_FINGER;
    case G_IMAGE_FAIL:
    case G_IMAGE_MESSY:
    case G_FEATURE_FAIL:  return FP_ERR_BAD_QUALITY;
    case G_NOT_FOUND:     return FP_ERR_NO_MATCH;
    case G_MERGE_FAIL:    return FP_ERR_MERGE_FAIL;
    case G_BAD_LOCATION:  return FP_ERR_INVALID_PARAM;
    case 0xFF:            return FP_ERR_COMM;
    default:              return FP_ERR_FAIL;
    }
}

/* ── Link setup / auto-detect ───────────────────────────────────────────── */

static void uart_apply(int tx, int rx, int baud)
{
    gpio_reset_pin(s_tx_gpio);
    gpio_reset_pin(s_rx_gpio);
    uart_set_baudrate(FP_UART_NUM, baud);
    uart_set_pin(FP_UART_NUM, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    gpio_pullup_en(rx);
    s_tx_gpio = tx;
    s_rx_gpio = rx;
    s_baud = baud;
}

static bool handshake(void)
{
    const uint8_t cmd[] = { CMD_VERIFY_PWD, 0, 0, 0, 0 };
    return transact(cmd, sizeof(cmd), NULL, 0, 300) == G_OK;
}

esp_err_t fp_sensor_init(void)
{
    s_ready = false;
    if (!uart_is_driver_installed(FP_UART_NUM)) {
        const uart_config_t cfg = {
            .baud_rate = FP_UART_BAUD, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        ESP_RETURN_ON_ERROR(uart_driver_install(FP_UART_NUM, 512, 0, 0, NULL, 0), TAG, "uart install");
        ESP_RETURN_ON_ERROR(uart_param_config(FP_UART_NUM, &cfg), TAG, "uart config");
        uart_apply(FP_UART_TX_GPIO, FP_UART_RX_GPIO, FP_UART_BAUD);
        vTaskDelay(pdMS_TO_TICKS(300));   /* module boot time */
    }

    bool found = handshake() || handshake();
    if (!found) {
        static const int bauds[] = { 57600, 115200, 9600, 19200, 38400 };
        const int pins[2][2] = { { FP_UART_TX_GPIO, FP_UART_RX_GPIO },
                                 { FP_UART_RX_GPIO, FP_UART_TX_GPIO } };
        for (int p = 0; p < 2 && !found; p++) {
            for (int b = 0; b < (int)(sizeof(bauds) / sizeof(bauds[0])) && !found; b++) {
                uart_apply(pins[p][0], pins[p][1], bauds[b]);
                vTaskDelay(pdMS_TO_TICKS(20));
                found = handshake();
                ESP_LOGI(TAG, "probe TX=%d RX=%d %6d baud: %s", pins[p][0], pins[p][1], bauds[b],
                         found ? "OK" : "no reply");
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

    uint8_t r[16];
    const uint8_t cmd[] = { CMD_READ_SYS_PARA };
    if (transact(cmd, sizeof(cmd), r, sizeof(r), TIMEOUT_SHORT_MS) == G_OK) {
        uint16_t cap = (r[4] << 8) | r[5];
        if (cap > 0 && cap <= 1000) s_capacity = cap;
        ESP_LOGI(TAG, "Sensor connected (TX=GPIO%d RX=GPIO%d %d baud), capacity %u, security %u",
                 s_tx_gpio, s_rx_gpio, s_baud, s_capacity, (r[6] << 8) | r[7]);
    } else {
        ESP_LOGI(TAG, "Sensor connected (TX=GPIO%d RX=GPIO%d %d baud), capacity unknown -> %u",
                 s_tx_gpio, s_rx_gpio, s_baud, s_capacity);
    }
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
    /* GROW has no "detect" command: GenImg returns NO_FINGER when nothing is there */
    const uint8_t cmd[] = { CMD_GEN_IMG };
    uint8_t g = transact(cmd, sizeof(cmd), NULL, 0, TIMEOUT_SHORT_MS);
    if (present) *present = (g == G_OK);
    return (g == G_OK || g == G_NO_FINGER) ? FP_OK : map_code(g);
}

uint8_t fp_sensor_capture(void)
{
    const uint8_t cmd[] = { CMD_GEN_IMG };
    return map_code(transact(cmd, sizeof(cmd), NULL, 0, TIMEOUT_LONG_MS));
}

uint8_t fp_sensor_generate(uint8_t ram_buf)
{
    /* GROW char buffers are 1 and 2; RegModel merges those two. Capture 0 goes
     * to buffer 1, later captures to buffer 2 (the last one wins). */
    const uint8_t cmd[] = { CMD_IMG2TZ, (uint8_t)(ram_buf == 0 ? 1 : 2) };
    return map_code(transact(cmd, sizeof(cmd), NULL, 0, TIMEOUT_LONG_MS));
}

uint8_t fp_sensor_merge(uint8_t count)
{
    const uint8_t cmd[] = { CMD_REG_MODEL };
    return map_code(transact(cmd, sizeof(cmd), NULL, 0, TIMEOUT_LONG_MS));
}

uint8_t fp_sensor_store(uint16_t id)
{
    const uint8_t cmd[] = { CMD_STORE, 1, (uint8_t)(id >> 8), (uint8_t)id };
    return map_code(transact(cmd, sizeof(cmd), NULL, 0, TIMEOUT_LONG_MS));
}

uint8_t fp_sensor_search(uint16_t *out_id)
{
    const uint8_t cmd[] = { CMD_SEARCH, 1, 0, 0, (uint8_t)(s_capacity >> 8), (uint8_t)s_capacity };
    uint8_t r[4];
    uint8_t g = transact(cmd, sizeof(cmd), r, sizeof(r), TIMEOUT_LONG_MS);
    if (g == G_OK && out_id) *out_id = (r[0] << 8) | r[1];
    if (g == G_OK) ESP_LOGD(TAG, "search: id %u score %u", (r[0] << 8) | r[1], (r[2] << 8) | r[3]);
    return map_code(g);
}

uint8_t fp_sensor_delete(uint16_t first, uint16_t last)
{
    uint16_t n = last >= first ? last - first + 1 : 1;
    if (first == 1 && last >= s_capacity) {            /* whole library */
        const uint8_t cmd[] = { CMD_EMPTY };
        return map_code(transact(cmd, sizeof(cmd), NULL, 0, TIMEOUT_LONG_MS));
    }
    const uint8_t cmd[] = { CMD_DELETE, (uint8_t)(first >> 8), (uint8_t)first, (uint8_t)(n >> 8), (uint8_t)n };
    return map_code(transact(cmd, sizeof(cmd), NULL, 0, TIMEOUT_LONG_MS));
}

uint8_t fp_sensor_template_count(uint16_t *out_count)
{
    const uint8_t cmd[] = { CMD_TEMPLATE_NUM };
    uint8_t r[2];
    uint8_t g = transact(cmd, sizeof(cmd), r, sizeof(r), TIMEOUT_SHORT_MS);
    if (out_count) *out_count = (g == G_OK) ? ((r[0] << 8) | r[1]) : 0;
    return map_code(g);
}

uint8_t fp_sensor_enrolled_bitmap(uint8_t *bitmap, int max_bytes, int *out_len)
{
    int len = 0;
    memset(bitmap, 0, max_bytes);
    for (int page = 0; page * 256 < s_capacity && len + 32 <= max_bytes; page++) {
        const uint8_t cmd[] = { CMD_READ_INDEX, (uint8_t)page };
        uint8_t g = transact(cmd, sizeof(cmd), bitmap + len, 32, TIMEOUT_SHORT_MS);
        if (g != G_OK) return map_code(g);
        len += 32;
    }
    if (out_len) *out_len = len;
    return FP_OK;
}

uint8_t fp_sensor_free_id(uint16_t from, uint16_t *out_id)
{
    uint8_t bm[128];
    int len = 0;
    uint8_t rc = fp_sensor_enrolled_bitmap(bm, sizeof(bm), &len);
    if (rc != FP_OK) return rc;
    for (int id = from; id < s_capacity && id < len * 8; id++) {
        if (!(bm[id / 8] & (1 << (id % 8)))) {
            if (out_id) *out_id = id;
            return FP_OK;
        }
    }
    return FP_ERR_LIBRARY_FULL;
}

uint8_t fp_sensor_led(fp_led_mode_t mode, fp_led_color_t color, uint8_t count)
{
    if (!s_ready) return FP_ERR_COMM;
    /* GROW AuraLedConfig: ctrl 1 breathing, 2 flashing, 3 on, 4 off, 5 fade in, 6 fade out;
     * colours 1 red, 2 blue, 3 purple, 4 green, 5 yellow, 6 cyan, 7 white */
    static const uint8_t ctrl_map[]  = { 0, 1, 2, 3, 4, 5, 6, 2 };     /* SLOW_BLINK -> flashing */
    static const uint8_t color_map[] = { 0, 4, 1, 5, 2, 6, 3, 7 };     /* our enum -> GROW */
    uint8_t speed = (mode == FP_LED_FAST_BLINK) ? 0x20 : 0x60;
    const uint8_t cmd[] = { CMD_AURA_LED, ctrl_map[mode], speed, color_map[color], count };
    return map_code(transact(cmd, sizeof(cmd), NULL, 0, TIMEOUT_SHORT_MS));
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
