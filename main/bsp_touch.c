#include "bsp_touch.h"
#include "bsp_i2c.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_touch_gt911.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BSP_TOUCH";

esp_err_t bsp_touch_init(esp_lcd_touch_handle_t *out_handle)
{
    ESP_LOGI(TAG, "Initializing GT911 touch (RST=%d, INT=%d)", TOUCH_RST_GPIO, TOUCH_INT_GPIO);

    /*
     * SAFETY: Do NOT set RST/INT to real GPIOs unless verified against the
     * board schematic.  The stock BSP uses NC (-1) for both.  Driving the
     * wrong GPIO as an output can damage power circuitry or the touch panel.
     */
    const esp_lcd_touch_config_t touch_cfg = {
        .x_max          = LCD_H_RES,
        .y_max          = LCD_V_RES,
        .rst_gpio_num   = TOUCH_RST_GPIO,
        .int_gpio_num   = TOUCH_INT_GPIO,
        .levels = {
            .reset     = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy  = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(bsp_i2c_get_handle(), &tp_io_cfg, &tp_io),
        TAG, "touch panel IO");

    esp_lcd_touch_handle_t handle = NULL;
    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_gt911(tp_io, &touch_cfg, &handle),
        TAG, "GT911 init");

    /* Read-only diagnostic: log firmware info (no writes!) */
    {
        uint8_t info[10];
        esp_lcd_panel_io_rx_param(tp_io, 0x8140, info, 10);
        ESP_LOGI(TAG, "GT911 ID=%c%c%c FW=0x%02X%02X res=%dx%d",
                 info[0], info[1], info[2], info[5], info[4],
                 info[6] | (info[7] << 8), info[8] | (info[9] << 8));
    }

    ESP_LOGI(TAG, "GT911 touch ready (%dx%d)", LCD_H_RES, LCD_V_RES);
    if (out_handle) *out_handle = handle;
    return ESP_OK;
}
