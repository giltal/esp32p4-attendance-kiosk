#include "bsp_rtc.h"
#include "bsp_i2c.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "BSP_RTC";
static i2c_master_dev_handle_t s_rtc_dev = NULL;

/* RX8025T register offsets */
#define RX8025T_REG_SEC     0x00
#define RX8025T_REG_MIN     0x01
#define RX8025T_REG_HOUR    0x02
#define RX8025T_REG_WEEK    0x03
#define RX8025T_REG_DAY     0x04
#define RX8025T_REG_MONTH   0x05
#define RX8025T_REG_YEAR    0x06
#define RX8025T_REG_FLAG    0x0E
#define RX8025T_REG_CTRL    0x0F

static uint8_t bcd_to_dec(uint8_t bcd)  { return (bcd >> 4) * 10 + (bcd & 0x0F); }
static uint8_t dec_to_bcd(uint8_t dec)  { return ((dec / 10) << 4) | (dec % 10); }

static esp_err_t rtc_read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_rtc_dev, &reg, 1, buf, len, 100);
}

static esp_err_t rtc_write_regs(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[16];
    buf[0] = reg;
    if (len > sizeof(buf) - 1) return ESP_ERR_INVALID_SIZE;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(s_rtc_dev, buf, len + 1, 100);
}

esp_err_t bsp_rtc_init(void)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = RTC_I2C_ADDR,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };

    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(bsp_i2c_get_handle(), &dev_cfg, &s_rtc_dev),
        TAG, "add RTC device");

    /* Read flag register to clear any pending flags */
    uint8_t flag = 0;
    esp_err_t ret = rtc_read_regs(RX8025T_REG_FLAG, &flag, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "RX8025T not responding — RTC unavailable");
        return ret;
    }

    /* Clear VLF (voltage low flag) if set — and set time from compile timestamp */
    if (flag & 0x02) {
        ESP_LOGW(TAG, "RX8025T VLF set — battery was lost, setting time from build timestamp");
        flag &= ~0x02;
        rtc_write_regs(RX8025T_REG_FLAG, &flag, 1);

        /* Parse __DATE__ "Mmm dd yyyy" and __TIME__ "hh:mm:ss" */
        struct tm build_time = {0};
        static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
        char mon_str[4] = {0};
        int day, year, hour, min, sec;
        sscanf(__DATE__, "%3s %d %d", mon_str, &day, &year);
        sscanf(__TIME__, "%d:%d:%d", &hour, &min, &sec);
        const char *p = strstr(months, mon_str);
        build_time.tm_mon  = p ? (int)(p - months) / 3 : 0;
        build_time.tm_mday = day;
        build_time.tm_year = year - 1900;
        build_time.tm_hour = hour;
        build_time.tm_min  = min;
        build_time.tm_sec  = sec;
        bsp_rtc_set_time(&build_time);
    }

    /* Read and display current time */
    struct tm t;
    bsp_rtc_get_time(&t);

    /* If time is clearly invalid (e.g. month out of range), set from build timestamp */
    if (t.tm_mon < 0 || t.tm_mon > 11 || t.tm_mday < 1 || t.tm_mday > 31 ||
        t.tm_hour > 23 || t.tm_min > 59 || t.tm_sec > 59) {
        ESP_LOGW(TAG, "RTC time invalid — setting from build timestamp");
        struct tm build_time = {0};
        static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
        char mon_str[4] = {0};
        int day, year, hour, min, sec;
        sscanf(__DATE__, "%3s %d %d", mon_str, &day, &year);
        sscanf(__TIME__, "%d:%d:%d", &hour, &min, &sec);
        const char *p = strstr(months, mon_str);
        build_time.tm_mon  = p ? (int)(p - months) / 3 : 0;
        build_time.tm_mday = day;
        build_time.tm_year = year - 1900;
        build_time.tm_hour = hour;
        build_time.tm_min  = min;
        build_time.tm_sec  = sec;
        bsp_rtc_set_time(&build_time);
        bsp_rtc_get_time(&t);
    }

    ESP_LOGI(TAG, "RTC ready: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    return ESP_OK;
}

esp_err_t bsp_rtc_get_time(struct tm *out_time)
{
    if (!s_rtc_dev || !out_time) return ESP_ERR_INVALID_STATE;

    uint8_t regs[7];
    ESP_RETURN_ON_ERROR(rtc_read_regs(RX8025T_REG_SEC, regs, 7), TAG, "read time");

    out_time->tm_sec  = bcd_to_dec(regs[0] & 0x7F);
    out_time->tm_min  = bcd_to_dec(regs[1] & 0x7F);
    out_time->tm_hour = bcd_to_dec(regs[2] & 0x3F);
    /* regs[3] is weekday bitmask (bit0=Sun, bit1=Mon, ..., bit6=Sat) */
    {
        uint8_t wday_mask = regs[3];
        int w = 0;
        while (wday_mask > 1 && w < 6) { wday_mask >>= 1; w++; }
        out_time->tm_wday = w;
    }
    out_time->tm_mday = bcd_to_dec(regs[4] & 0x3F);
    out_time->tm_mon  = bcd_to_dec(regs[5] & 0x1F) - 1; /* struct tm is 0-based */
    out_time->tm_year = bcd_to_dec(regs[6]) + 100;       /* years since 1900 → 2000+xx */
    out_time->tm_isdst = -1;
    return ESP_OK;
}

esp_err_t bsp_rtc_set_time(const struct tm *time)
{
    if (!s_rtc_dev || !time) return ESP_ERR_INVALID_STATE;

    /* Compute weekday bitmask from tm_wday (0=Sunday) */
    uint8_t wday_mask = 1 << time->tm_wday;

    uint8_t regs[7] = {
        dec_to_bcd(time->tm_sec),
        dec_to_bcd(time->tm_min),
        dec_to_bcd(time->tm_hour),
        wday_mask,
        dec_to_bcd(time->tm_mday),
        dec_to_bcd(time->tm_mon + 1),
        dec_to_bcd(time->tm_year - 100),
    };

    ESP_RETURN_ON_ERROR(rtc_write_regs(RX8025T_REG_SEC, regs, 7), TAG, "write time");
    ESP_LOGI(TAG, "RTC set: %04d-%02d-%02d %02d:%02d:%02d",
             time->tm_year + 1900, time->tm_mon + 1, time->tm_mday,
             time->tm_hour, time->tm_min, time->tm_sec);
    return ESP_OK;
}
