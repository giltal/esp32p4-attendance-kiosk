#pragma once
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_mipi_dsi.h"

esp_err_t bsp_display_init(esp_lcd_panel_handle_t *out_panel,
                           esp_lcd_dsi_bus_handle_t *out_dsi_bus,
                           esp_lcd_panel_io_handle_t *out_io);
