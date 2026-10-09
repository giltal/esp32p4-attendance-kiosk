#include "bsp_audio.h"
#include "bsp_i2c.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include <math.h>

static const char *TAG = "BSP_AUDIO";
static i2s_chan_handle_t s_i2s_tx = NULL;
static i2s_chan_handle_t s_i2s_rx = NULL;
static esp_codec_dev_handle_t s_codec_dev = NULL;

esp_err_t bsp_audio_init(void)
{
    /* 1. Configure power amplifier GPIO (off initially) */
    gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << AUDIO_PA_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pa_cfg);
    gpio_set_level(AUDIO_PA_GPIO, 0);

    /* 2. Create I2S channel */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, &s_i2s_rx), TAG, "I2S channel");

    /* 3. Configure I2S standard mode (Philips) */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_GPIO,
            .bclk = I2S_BCLK_GPIO,
            .ws   = I2S_WS_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din  = I2S_DIN_GPIO,
            .invert_flags = { false, false, false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = AUDIO_MCLK_MULTIPLE;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg), TAG, "I2S TX init");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_rx, &std_cfg), TAG, "I2S RX init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "I2S TX enable");

    /* 4. Set up ES8311 codec via esp_codec_dev */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = AUDIO_I2S_NUM,
        .rx_handle = s_i2s_rx,
        .tx_handle = s_i2s_tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM,
        .addr = AUDIO_CODEC_I2C_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if    = ctrl_if,
        .gpio_if    = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin     = AUDIO_PA_GPIO,
        .use_mclk   = true,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&codec_cfg);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type  = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if  = codec_if,
        .data_if   = data_if,
    };
    s_codec_dev = esp_codec_dev_new(&dev_cfg);

    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate     = AUDIO_SAMPLE_RATE,
        .channel         = 2,
        .bits_per_sample = 16,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_codec_dev, &sample_info), TAG, "codec open");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_codec_dev, 60), TAG, "codec vol");

    /* 5. Enable speaker amplifier */
    gpio_set_level(AUDIO_PA_GPIO, 1);

    ESP_LOGI(TAG, "Audio ready (ES8311, %d Hz, PA=%d)", AUDIO_SAMPLE_RATE, AUDIO_PA_GPIO);
    return ESP_OK;
}

esp_err_t bsp_audio_set_volume(int volume_pct)
{
    if (!s_codec_dev) return ESP_ERR_INVALID_STATE;
    if (volume_pct < 0) volume_pct = 0;
    if (volume_pct > 100) volume_pct = 100;
    return esp_codec_dev_set_out_vol(s_codec_dev, volume_pct);
}

esp_err_t bsp_audio_play_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    if (!s_codec_dev) return ESP_ERR_INVALID_STATE;

    const int total_samples = (AUDIO_SAMPLE_RATE * duration_ms) / 1000;
    const int buf_samples = 256;
    int16_t buf[buf_samples * 2]; /* stereo */

    for (int offset = 0; offset < total_samples; offset += buf_samples) {
        int count = (offset + buf_samples > total_samples) ? (total_samples - offset) : buf_samples;
        for (int i = 0; i < count; i++) {
            double t = (double)(offset + i) / AUDIO_SAMPLE_RATE;
            int16_t sample = (int16_t)(16000.0 * sin(2.0 * M_PI * freq_hz * t));
            buf[i * 2]     = sample;
            buf[i * 2 + 1] = sample;
        }
        esp_codec_dev_write(s_codec_dev, buf, count * 4);
    }
    return ESP_OK;
}
