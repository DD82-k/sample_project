/*
 * INMP441 microphone driver, ESP-IDF v5.1.2 native I2S standard mode.
 * Pins: SCK=GPIO42, WS=GPIO40, SD=GPIO8, CHIPEN=GPIO38.
 *
 * INMP441 outputs 24-bit samples in a 32-bit I2S slot. Read 32-bit words and
 * down-convert to 16-bit signed PCM so VAD and cloud ASR receive valid PCM.
 */
#include "audio_mic.h"

#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
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
static int32_t *rx32_buf = NULL;
static int rx32_capacity = 0;
static int pcm_shift = 14;
static int diag_count = 0;

esp_err_t audio_mic_init(void)
{
    gpio_config_t en = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = BIT64(MIC_CHIPEN_PIN),
    };
    gpio_config(&en);
    gpio_set_level(MIC_CHIPEN_PIN, 1);
    ESP_LOGI(TAG, "CHIPEN GPIO38=HIGH");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_PORT, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK_PIN,
            .ws = MIC_WS_PIN,
            .din = MIC_SD_PIN,
            .dout = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    std_cfg.slot_cfg.ws_width = 32;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));

    ESP_LOGI(TAG, "I2S0 RX 16kHz 32-bit slot -> 16-bit mono PCM");
    return ESP_OK;
}

int audio_mic_read(int16_t *buf, int max_samples)
{
    if (rx_chan == NULL || buf == NULL || max_samples <= 0) {
        return 0;
    }

    if (!rx32_buf || rx32_capacity < max_samples) {
        free(rx32_buf);
        rx32_buf = heap_caps_malloc(max_samples * sizeof(int32_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!rx32_buf) {
            rx32_buf = malloc(max_samples * sizeof(int32_t));
        }
        if (!rx32_buf) {
            ESP_LOGE(TAG, "rx32 malloc failed");
            rx32_capacity = 0;
            return 0;
        }
        rx32_capacity = max_samples;
    }

    size_t bytes = 0;
    i2s_channel_read(rx_chan, rx32_buf, max_samples * sizeof(int32_t),
                     &bytes, pdMS_TO_TICKS(100));

    int samples = bytes / sizeof(int32_t);
    int peak_shift8 = 0;
    int peak_shift14 = 0;
    int peak_shift16 = 0;
    int nonzero_low8 = 0;

    for (int i = 0; i < samples; i++) {
        int32_t raw = rx32_buf[i];
        int s8 = (int)(raw >> 8);
        int s14 = (int)(raw >> 14);
        int s16 = (int)(raw >> 16);
        int a8 = s8 < 0 ? -s8 : s8;
        int a14 = s14 < 0 ? -s14 : s14;
        int a16 = s16 < 0 ? -s16 : s16;
        if (a8 > peak_shift8) {
            peak_shift8 = a8;
        }
        if (a14 > peak_shift14) {
            peak_shift14 = a14;
        }
        if (a16 > peak_shift16) {
            peak_shift16 = a16;
        }
        if ((raw & 0xff) != 0) {
            nonzero_low8++;
        }
    }

    for (int i = 0; i < samples; i++) {
        int32_t s = rx32_buf[i] >> pcm_shift;
        if (s > 32767) {
            s = 32767;
        } else if (s < -32768) {
            s = -32768;
        }
        buf[i] = (int16_t)s;
    }

    if (++diag_count >= 400) {
        diag_count = 0;
        ESP_LOGI(TAG, "raw32 diag: shift=%d low8_nonzero=%d/%d peak>>8=%d peak>>14=%d peak>>16=%d",
                 pcm_shift, nonzero_low8, samples, peak_shift8, peak_shift14, peak_shift16);
    }
    return samples;
}

void audio_mic_deinit(void)
{
    if (rx_chan) {
        i2s_channel_disable(rx_chan);
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
    }

    free(rx32_buf);
    rx32_buf = NULL;
    rx32_capacity = 0;
    gpio_set_level(MIC_CHIPEN_PIN, 0);
}
