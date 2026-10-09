#pragma once
#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

esp_err_t bsp_touch_init(esp_lcd_touch_handle_t *out_handle);
