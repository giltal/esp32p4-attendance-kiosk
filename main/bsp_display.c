#include "bsp_display.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_jd9165.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BSP_DISPLAY";

/* ── Vendor-specific init commands for this panel (JC1060M070N) ───────── */
static const jd9165_lcd_init_cmd_t s_panel_init_cmds[] = {
    {0x30, (uint8_t[]){0x00}, 1, 0},
    {0xF7, (uint8_t[]){0x49, 0x61, 0x02, 0x00}, 4, 0},
    {0x30, (uint8_t[]){0x01}, 1, 0},
    {0x04, (uint8_t[]){0x0C}, 1, 0},
    {0x05, (uint8_t[]){0x00}, 1, 0},
    {0x06, (uint8_t[]){0x00}, 1, 0},
    {0x0B, (uint8_t[]){0x11}, 1, 0},
    {0x17, (uint8_t[]){0x00}, 1, 0},
    {0x20, (uint8_t[]){0x04}, 1, 0},
    {0x1F, (uint8_t[]){0x05}, 1, 0},
    {0x23, (uint8_t[]){0x00}, 1, 0},
    {0x25, (uint8_t[]){0x19}, 1, 0},
    {0x28, (uint8_t[]){0x18}, 1, 0},
    {0x29, (uint8_t[]){0x04}, 1, 0},
    {0x2A, (uint8_t[]){0x01}, 1, 0},
    {0x2B, (uint8_t[]){0x04}, 1, 0},
    {0x2C, (uint8_t[]){0x01}, 1, 0},

    {0x30, (uint8_t[]){0x02}, 1, 0},
    {0x01, (uint8_t[]){0x22}, 1, 0},
    {0x03, (uint8_t[]){0x12}, 1, 0},
    {0x04, (uint8_t[]){0x00}, 1, 0},
    {0x05, (uint8_t[]){0x64}, 1, 0},
    {0x0A, (uint8_t[]){0x08}, 1, 0},
    {0x0B, (uint8_t[]){0x0A, 0x1A, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x08, 0x1F, 0x1D}, 11, 0},
    {0x0C, (uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0D, (uint8_t[]){0x16, 0x1B, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x09, 0x1E, 0x1C}, 11, 0},
    {0x0E, (uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0F, (uint8_t[]){0x16, 0x1B, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1C, 0x1E, 0x09, 0x07}, 11, 0},
    {0x10, (uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x11, (uint8_t[]){0x0A, 0x1A, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1D, 0x1F, 0x08, 0x06}, 11, 0},
    {0x12, (uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x14, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0x18, (uint8_t[]){0x99}, 1, 0},

    {0x30, (uint8_t[]){0x06}, 1, 0},
    {0x12, (uint8_t[]){0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29}, 14, 0},
    {0x13, (uint8_t[]){0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29}, 14, 0},

    {0x30, (uint8_t[]){0x0A}, 1, 0},
    {0x02, (uint8_t[]){0x4F}, 1, 0},
    {0x0B, (uint8_t[]){0x40}, 1, 0},
    {0x12, (uint8_t[]){0x3E}, 1, 0},
    {0x13, (uint8_t[]){0x78}, 1, 0},

    {0x30, (uint8_t[]){0x0D}, 1, 0},
    {0x0D, (uint8_t[]){0x04}, 1, 0},
    {0x10, (uint8_t[]){0x0C}, 1, 0},
    {0x11, (uint8_t[]){0x0C}, 1, 0},
    {0x12, (uint8_t[]){0x0C}, 1, 0},
    {0x13, (uint8_t[]){0x0C}, 1, 0},

    {0x30, (uint8_t[]){0x00}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 50},
};

esp_err_t bsp_display_init(esp_lcd_panel_handle_t *out_panel,
                           esp_lcd_dsi_bus_handle_t *out_dsi_bus,
                           esp_lcd_panel_io_handle_t *out_io)
{
    /* 1. Power the MIPI-DSI PHY via internal LDO */
    ESP_LOGI(TAG, "Powering MIPI-DSI PHY LDO (ch%d, %dmV)",
             LCD_MIPI_DSI_PHY_LDO_CHAN, LCD_MIPI_DSI_PHY_LDO_MV);
    esp_ldo_channel_handle_t ldo_mipi = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = LCD_MIPI_DSI_PHY_LDO_CHAN,
        .voltage_mv = LCD_MIPI_DSI_PHY_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi), TAG, "LDO");

    /* Give the display panel time to power up before DSI communication */
    ESP_LOGI(TAG, "Waiting for panel power stabilization...");
    vTaskDelay(pdMS_TO_TICKS(200));

    /* 2. Create MIPI-DSI bus */
    ESP_LOGI(TAG, "Creating MIPI-DSI bus");
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 550,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus), TAG, "DSI bus");

    /* 3. Create DBI panel IO */
    ESP_LOGI(TAG, "Creating panel IO (DBI)");
    esp_lcd_panel_io_handle_t dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = JD9165_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &dbi_io), TAG, "DBI IO");

    /* 4. Create JD9165 panel with custom init sequence & DPI timing */
    ESP_LOGI(TAG, "Installing JD9165 panel driver (%dx%d)", LCD_H_RES, LCD_V_RES);

    const esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src       = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = LCD_DPI_CLK_MHZ,
        .virtual_channel   = 0,
        .pixel_format      = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs           = 1,
        .video_timing = {
            .h_size            = LCD_H_RES,
            .v_size            = LCD_V_RES,
            .hsync_back_porch  = LCD_HSYNC_BACK_PORCH,
            .hsync_pulse_width = LCD_HSYNC_PULSE_WIDTH,
            .hsync_front_porch = LCD_HSYNC_FRONT_PORCH,
            .vsync_back_porch  = LCD_VSYNC_BACK_PORCH,
            .vsync_pulse_width = LCD_VSYNC_PULSE_WIDTH,
            .vsync_front_porch = LCD_VSYNC_FRONT_PORCH,
        },
        .flags.use_dma2d = true,
    };

    jd9165_vendor_config_t vendor_cfg = {
        .init_cmds      = s_panel_init_cmds,
        .init_cmds_size = sizeof(s_panel_init_cmds) / sizeof(s_panel_init_cmds[0]),
        .mipi_config = {
            .dsi_bus    = dsi_bus,
            .dpi_config = &dpi_cfg,
        },
    };

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num  = LCD_RST_GPIO,
        .rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel  = LCD_BIT_PER_PIXEL,
        .vendor_config   = &vendor_cfg,
    };

    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9165(dbi_io, &panel_cfg, &panel), TAG, "panel");

    /* Hardware reset via GPIO27 (also affects touch subsystem) */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    vTaskDelay(pdMS_TO_TICKS(120));  /* Extra settle time after reset */
    ESP_LOGI(TAG, "Display hardware reset done (GPIO%d)", LCD_RST_GPIO);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "disp on");

    /* 5. Turn on backlight */
    if (LCD_BK_LIGHT_GPIO >= 0) {
        gpio_config_t bk_cfg = {
            .pin_bit_mask = 1ULL << LCD_BK_LIGHT_GPIO,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&bk_cfg), TAG, "BK GPIO config");
        gpio_set_level(LCD_BK_LIGHT_GPIO, LCD_BK_LIGHT_ON_LEVEL);
        ESP_LOGI(TAG, "Backlight ON (GPIO%d)", LCD_BK_LIGHT_GPIO);
    }

    ESP_LOGI(TAG, "Display initialized (%dx%d @ %.1f MHz)", LCD_H_RES, LCD_V_RES, LCD_DPI_CLK_MHZ);

    if (out_panel)   *out_panel   = panel;
    if (out_dsi_bus) *out_dsi_bus = dsi_bus;
    if (out_io)      *out_io      = dbi_io;
    return ESP_OK;
}
