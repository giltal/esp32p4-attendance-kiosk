/*
 * TEMPORARY pin diagnostic: jumper between two GPIOs, test plain-GPIO and
 * UART loopback in both directions. Enable with PIN_DIAG in main.c.
 */
#include "pin_diag.h"
#include "board_config.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "PIN_DIAG";

static void gpio_loop(int out, int in)
{
    gpio_reset_pin(out);
    gpio_reset_pin(in);
    gpio_set_direction(out, GPIO_MODE_OUTPUT);
    gpio_set_direction(in, GPIO_MODE_INPUT);

    gpio_set_pull_mode(in, GPIO_PULLDOWN_ONLY);
    gpio_set_level(out, 1); vTaskDelay(pdMS_TO_TICKS(5));
    int hi = gpio_get_level(in);

    gpio_set_pull_mode(in, GPIO_PULLUP_ONLY);
    gpio_set_level(out, 0); vTaskDelay(pdMS_TO_TICKS(5));
    int lo = gpio_get_level(in);

    ESP_LOGI(TAG, "GPIO%d -> GPIO%d plain GPIO: drive 1 read %d, drive 0 read %d  => %s",
             out, in, hi, lo, (hi == 1 && lo == 0) ? "PASS" : "FAIL");
    gpio_reset_pin(out);
    gpio_reset_pin(in);
}

static void uart_loop(int tx, int rx, bool lp_release, const char *label)
{
    const uart_config_t cfg = {
        .baud_rate = 57600, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(FP_UART_NUM, 256, 0, 0, NULL, 0);
    uart_param_config(FP_UART_NUM, &cfg);
    gpio_reset_pin(tx);
    gpio_reset_pin(rx);
    if (lp_release) {
        if (rtc_gpio_is_valid_gpio(tx)) rtc_gpio_deinit(tx);
        if (rtc_gpio_is_valid_gpio(rx)) rtc_gpio_deinit(rx);
    }
    uart_set_pin(FP_UART_NUM, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    gpio_pullup_en(rx);
    vTaskDelay(pdMS_TO_TICKS(20));
    uart_flush_input(FP_UART_NUM);

    const char msg[] = "LOOPBACK-123";
    uart_write_bytes(FP_UART_NUM, msg, sizeof(msg) - 1);
    char buf[32] = {0};
    int n = uart_read_bytes(FP_UART_NUM, buf, sizeof(buf) - 1, pdMS_TO_TICKS(300));
    bool ok = (n == (int)sizeof(msg) - 1) && memcmp(buf, msg, n) == 0;
    ESP_LOGI(TAG, "UART TX=GPIO%d RX=GPIO%d %s: got %d bytes '%.*s'  => %s",
             tx, rx, label, n, n > 0 ? n : 0, buf, ok ? "PASS" : "FAIL");
    if (!ok) gpio_dump_io_configuration(stdout, (1ULL << tx) | (1ULL << rx));

    uart_driver_delete(FP_UART_NUM);
    gpio_reset_pin(tx);
    gpio_reset_pin(rx);
}

/* Drive a single pin 0/1 and read the pad back (input+output enabled). */
void pin_diag_selftest(const int *pins, int count)
{
    ESP_LOGW(TAG, "===== Single-pin drive/readback test =====");
    for (int i = 0; i < count; i++) {
        int p = pins[i];
        gpio_reset_pin(p);
        gpio_set_pull_mode(p, GPIO_FLOATING);
        gpio_set_direction(p, GPIO_MODE_INPUT_OUTPUT);
        gpio_set_level(p, 0); vTaskDelay(pdMS_TO_TICKS(5));
        int r0 = gpio_get_level(p);
        gpio_set_level(p, 1); vTaskDelay(pdMS_TO_TICKS(5));
        int r1 = gpio_get_level(p);
        gpio_set_direction(p, GPIO_MODE_INPUT);
        gpio_set_pull_mode(p, GPIO_PULLDOWN_ONLY); vTaskDelay(pdMS_TO_TICKS(5));
        int pd = gpio_get_level(p);
        gpio_set_pull_mode(p, GPIO_PULLUP_ONLY); vTaskDelay(pdMS_TO_TICKS(5));
        int pu = gpio_get_level(p);
        ESP_LOGI(TAG, "GPIO%-2d drive0->%d drive1->%d | input pulldown->%d pullup->%d  => %s",
                 p, r0, r1, pd, pu,
                 (r0 == 0 && r1 == 1) ? (pd == 0 && pu == 1 ? "OK, free" : "OK, externally pulled")
                                      : "CANNOT DRIVE (tied to something?)");
        gpio_reset_pin(p);
    }
}

/* ── Sensor line probe: TX pad self-readback + edge capture on RX ─────────── */

#include "esp_timer.h"
#include "esp_rom_gpio.h"
#include "soc/uart_periph.h"

static volatile int s_edges;
static volatile int64_t s_last_us, s_min_us;

static void IRAM_ATTR edge_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (s_edges > 0) {
        int64_t d = now - s_last_us;
        if (d < s_min_us) s_min_us = d;
    }
    s_last_us = now;
    s_edges++;
}

/* ID809 TEST_CONNECTION frame (26 bytes, checksum = byte sum = 0x0100) */
static const uint8_t VFY_PWD_PKT[26] = {
    0xAA, 0x55, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x00, 0x01
};

void pin_diag_sensor_lines(int tx, int rx)
{
    const uart_config_t cfg = {
        .baud_rate = 57600, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_LOGW(TAG, "===== Sensor line probe TX=GPIO%d RX=GPIO%d =====", tx, rx);
    ESP_LOGI(TAG, "idle levels: GPIO%d=%d GPIO%d=%d", tx, gpio_get_level(tx), rx, gpio_get_level(rx));

    uart_driver_install(FP_UART_NUM, 256, 0, 0, NULL, 0);
    uart_param_config(FP_UART_NUM, &cfg);

    /* 1. TX pad self-readback: route UART RX from the TX pin itself */
    gpio_reset_pin(tx);
    uart_set_pin(FP_UART_NUM, tx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    gpio_input_enable(tx);
    esp_rom_gpio_connect_in_signal(tx, UART_PERIPH_SIGNAL(FP_UART_NUM, SOC_UART_RX_PIN_IDX), false);
    vTaskDelay(pdMS_TO_TICKS(20));
    uart_flush_input(FP_UART_NUM);
    uart_write_bytes(FP_UART_NUM, (const char *)VFY_PWD_PKT, sizeof(VFY_PWD_PKT));
    uint8_t buf[64];
    int n = uart_read_bytes(FP_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(300));
    bool self_ok = n >= (int)sizeof(VFY_PWD_PKT) && memcmp(buf, VFY_PWD_PKT, sizeof(VFY_PWD_PKT)) == 0;
    ESP_LOGI(TAG, "TX pad GPIO%d self-readback: %d bytes => %s", tx, n,
             self_ok ? "PASS (UART TX reaches the pin)" : "FAIL (UART TX not reaching the pin)");

    /* 2. Normal pins, count edges on RX after sending the handshake */
    gpio_reset_pin(tx);
    gpio_reset_pin(rx);
    uart_set_pin(FP_UART_NUM, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    gpio_install_isr_service(0);
    gpio_set_intr_type(rx, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(rx, edge_isr, NULL);

    /* All R503 baud rates (9600*N, N=1..12) at the default and zero address */
    static const uint32_t addrs[] = { 0 };
    int total_after = 0;
    for (int a = 0; a < 1; a++) {
        uint8_t pkt[sizeof(VFY_PWD_PKT)];
        memcpy(pkt, VFY_PWD_PKT, sizeof(pkt));
        for (int nb = 1; nb <= 12; nb++) {
            int baud = 9600 * nb;
            uart_set_baudrate(FP_UART_NUM, baud);
            vTaskDelay(pdMS_TO_TICKS(30));
            uart_flush_input(FP_UART_NUM);
            s_edges = 0; s_min_us = INT64_MAX;
            uart_write_bytes(FP_UART_NUM, (const char *)pkt, sizeof(pkt));
            uart_wait_tx_done(FP_UART_NUM, pdMS_TO_TICKS(100));
            int before = s_edges;
            vTaskDelay(pdMS_TO_TICKS(250));
            n = uart_read_bytes(FP_UART_NUM, buf, sizeof(buf), 0);
            int after = s_edges - before;
            total_after += after;
            if (after > 0 || n > 0) {
                char hex[3 * 20 + 1] = {0};
                for (int i = 0; i < n && i < 20; i++) sprintf(hex + i * 3, "%02X ", buf[i]);
                int64_t mn = s_min_us;
                ESP_LOGW(TAG, "REPLY! addr=%08lX @%6d: %d edges, min pulse %lld us (~%lld baud), %d bytes: %s",
                         (unsigned long)addrs[a], baud, after, mn, mn > 0 ? 1000000 / mn : -1, n, hex);
            }
        }
    }
    ESP_LOGI(TAG, "Scanned 12 bauds: total RX edges after TX = %d%s", total_after,
             total_after ? "" : " (sensor never transmitted)");
    gpio_isr_handler_remove(rx);
    gpio_set_intr_type(rx, GPIO_INTR_DISABLE);
    uart_driver_delete(FP_UART_NUM);
    gpio_reset_pin(tx);
    gpio_reset_pin(rx);
    ESP_LOGW(TAG, "===== Sensor line probe done =====");
}

/* Log every level change on a pin (e.g. sensor WAKEUP/touch line). */
static void watch_task(void *arg)
{
    int pin = (int)(intptr_t)arg;
    int last = -1;
    for (;;) {
        int v = gpio_get_level(pin);
        if (v != last) {
            ESP_LOGI(TAG, "WATCH GPIO%d = %d %s", pin, v, v == 0 ? "(LOW - finger?)" : "(HIGH)");
            last = v;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void pin_diag_watch(int pin)
{
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    xTaskCreate(watch_task, "pin_watch", 3072, (void *)(intptr_t)pin, 2, NULL);
}

void pin_diag_run(int a, int b)
{
    ESP_LOGW(TAG, "===== Pin diagnostic GPIO%d <-> GPIO%d (jumper required) =====", a, b);
    gpio_loop(a, b);
    gpio_loop(b, a);
    uart_loop(a, b, false, "(normal)");
    uart_loop(b, a, false, "(normal)");
    uart_loop(a, b, true, "(LP-IO released)");
    uart_loop(b, a, true, "(LP-IO released)");
    ESP_LOGW(TAG, "===== Pin diagnostic done =====");
}
