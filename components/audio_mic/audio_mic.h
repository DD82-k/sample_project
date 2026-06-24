/*
 * INMP441 I2S 麦克风驱动 — ESP-IDF 原生 I2S
 * 引脚: SCK=GPIO42, WS=GPIO40, SD=GPIO8, CHIPEN=GPIO38
 * 参数: 16000Hz, 16bit PCM, 单声道
 */
#pragma once
#include "esp_err.h"
#include <stdint.h>

esp_err_t audio_mic_init(void);
int audio_mic_read(int16_t *buf, int max_samples);
void audio_mic_deinit(void);
