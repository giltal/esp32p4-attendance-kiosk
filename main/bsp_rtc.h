#pragma once
#include "esp_err.h"
#include <time.h>

esp_err_t bsp_rtc_init(void);
esp_err_t bsp_rtc_get_time(struct tm *out_time);
esp_err_t bsp_rtc_set_time(const struct tm *time);
