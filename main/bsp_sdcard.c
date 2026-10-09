#include "bsp_sdcard.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

static const char *TAG = "BSP_SDCARD";

esp_err_t bsp_sdcard_init(void)
{
    /* 1. Power-cycle the SD card via GPIO36 (active HIGH MOSFET) */
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = 1ULL << SD_POWER_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pwr_cfg);
    gpio_set_level(SD_POWER_GPIO, 0);          /* power off */
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(SD_POWER_GPIO, 1);          /* power on  */
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Mounting SD card at %s (SDMMC 4-bit)", SD_MOUNT_POINT);

    /* 2. Mount with SDMMC driver — Slot 0 (fixed IO MUX pins on ESP32-P4) */
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 10,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot          = SDMMC_HOST_SLOT_0;
    host.max_freq_khz  = SDMMC_FREQ_HIGHSPEED;

    /* On-chip LDO for SDMMC I/O voltage (required on ESP32-P4) */
    sd_pwr_ctrl_ldo_config_t ldo_config = { .ldo_chan_id = 4 };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init SDMMC LDO: %s", esp_err_to_name(ret));
        return ret;
    }
    host.pwr_ctrl_handle = pwr_ctrl_handle;

    /* Slot 0 uses fixed IO MUX pins — no need to specify GPIOs */
    sdmmc_slot_config_t slot_cfg = {
        .cd    = SDMMC_SLOT_NO_CD,
        .wp    = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = 0,
    };

    sdmmc_card_t *card = NULL;
    ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_cfg,
                                            &mount_cfg, &card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem (not FAT?)");
        } else {
            ESP_LOGE(TAG, "SD card init failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    ESP_LOGI(TAG, "SD card mounted — %s", SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, card);
    return ESP_OK;
}

const char *bsp_sdcard_get_mount_point(void)
{
    return SD_MOUNT_POINT;
}
