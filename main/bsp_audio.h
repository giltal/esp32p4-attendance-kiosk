#pragma once
#include "esp_err.h"

esp_err_t bsp_audio_init(void);
esp_err_t bsp_audio_set_volume(int volume_pct);
esp_err_t bsp_audio_play_tone(uint32_t freq_hz, uint32_t duration_ms);
