#include "bsp_i2c.h"
#include "board_config.h"
#include "esp_log.h"

static const char *TAG = "BSP_I2C";
static i2c_master_bus_handle_t s_i2c_bus = NULL;

esp_err_t bsp_i2c_init(void)
{
    if (s_i2c_bus) {
        return ESP_OK; /* already initialized */
    }

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port   = I2C_NUM,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "I2C bus ready (SDA=%d, SCL=%d, %d Hz)",
                 I2C_SDA_GPIO, I2C_SCL_GPIO, I2C_FREQ_HZ);
    }
    return ret;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    return s_i2c_bus;
}
