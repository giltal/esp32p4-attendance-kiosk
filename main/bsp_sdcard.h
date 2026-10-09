#pragma once
#include "esp_err.h"

esp_err_t bsp_sdcard_init(void);
const char *bsp_sdcard_get_mount_point(void);
