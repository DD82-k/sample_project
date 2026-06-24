/*
 * INMP441 麦克风驱动 — ESP-IDF v5.1.2 原生 I2S 标准模式
 * 引脚: SCK=GPIO42, WS=GPIO40, SD=GPIO8, CHIPEN=GPIO38
 * 参数: 16000Hz, 16bit PCM, 单声道
 *
 * 注: INMP441 是 24-bit I2S, ESP32 用 16-bit 读只能拿高 16 位.
 *     低信号 (<256 in 24-bit) 会被截断为 0, 导致 ZCR 不准.
 *     但这由 alarm_detect 的能量门限 (RMS>500) 补偿, 安静帧直接跳过.
 */
#include "audio_mic.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "audio_mic";

#define MIC_SCK_PIN    GPIO_NUM_42
#define MIC_WS_PIN     GPIO_NUM_40
#define MIC_SD_PIN     GPIO_NUM_8
#define MIC_CHIPEN_PIN GPIO_NUM_38
#define MIC_PORT       I2S_NUM_0
#define MIC_RATE       16000

static i2s_chan_handle_t rx_chan = NULL;

esp_err_t audio_mic_init(void)
{
    gpio_config_t en = {.mode = GPIO_MODE_OUTPUT, .pin_bit_mask = BIT64(MIC_CHIPEN_PIN)};
    gpio_config(&en);
    gpio_set_level(MIC_CHIPEN_PIN, 1);
    ESP_LOGI(TAG, "CHIPEN GPIO38=HIGH");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_PORT, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK_PIN,
            .ws   = MIC_WS_PIN,
            .din  = MIC_SD_PIN,
            .dout = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

    ESP_LOGI(TAG, "I2S0 RX — 16kHz 16-bit mono");
    return ESP_OK;
}

int audio_mic_read(int16_t *buf, int max_samples)
{
    if (rx_chan == NULL) return 0;
    size_t bytes = 0;
    i2s_channel_read(rx_chan, buf, max_samples * sizeof(int16_t), &bytes, pdMS_TO_TICKS(100));
    return bytes / sizeof(int16_t);
}

void audio_mic_deinit(void)
{
    if (rx_chan) { i2s_channel_disable(rx_chan); i2s_del_channel(rx_chan); rx_chan = NULL; }
    gpio_set_level(MIC_CHIPEN_PIN, 0);
}
