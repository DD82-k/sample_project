/*
 * P169H002-CTP Smartwatch — 主入口
 * ST7789 LCD (240x280) + CST816T touch + LVGL v9
 */
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_st7789.h"
#include "touch_cst816t.h"
#include "lvgl_port.h"
#include "ui/ui.h"
#include "wifi_mqtt.h"
#include "audio_mic.h"
#include "vad.h"
#include "alarm_detect.h"
#include "speech_prep.h"
#include "audio_upload.h"
#include "alarm_ui.h"
#include "asr_display.h"

extern lv_obj_t * uic_LabelContent;
extern lv_obj_t * ui_LabelTime;

/* ---- 清空按钮 ---- */
void Button_Clear(lv_event_t * e)
{
    asr_display_reset();
    lv_label_set_text(uic_LabelContent, "");
}

/* ---- 时钟 ---- */
static void rtc_update_cb(lv_timer_t *timer)
{
    time_t now; struct tm ti; char buf[16];
    time(&now); localtime_r(&now, &ti);
    strftime(buf, sizeof(buf), "%H:%M:%S", &ti);
    lv_label_set_text(ui_LabelTime, buf);
}

/* ---- 音频任务: VAD + 报警 + 降噪 + ASR 上传 ---- */
static void audio_task(void *arg)
{
    int16_t *buf = heap_caps_malloc(320 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *asr_buf = heap_caps_malloc(320 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *asr_pending = heap_caps_malloc(ASR_PENDING_FRAMES * MIC_FRAME_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int *asr_pending_counts = heap_caps_malloc(ASR_PENDING_FRAMES * sizeof(int), MALLOC_CAP_SPIRAM);
    if (!buf || !asr_buf || !asr_pending || !asr_pending_counts) {
        printf("audio: malloc failed\n"); vTaskDelete(NULL); return;
    }

    int voice_len = 0, asr_pending_count = 0, next_asr_start_retry = 0, last_audio_diag = -16000;
    bool asr_streaming = false, asr_start_pending = false, asr_preconnected = false;
    bool asr_wait_logged = false, asr_feed_full_logged = false;
    TickType_t asr_cooldown_until = 0;

    vad_init(220, 90);
    alarm_detect_init(0, 0);
    alarm_detect_set_callback(alarm_ui_on_alarm);
    speech_prep_init(3000.0f, 12.0f);

    while (1) {
        int n = audio_mic_read(buf, 320);
        if (n < 1) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        taskYIELD();

        /* 报警检测 (每 5 帧一次)*/
        static int alarm_div = 0;
        if (++alarm_div >= 5) { alarm_div = 0; alarm_detect_feed(buf, n); }
        vad_feed(buf, n);

        switch (vad_state()) {
        case VAD_STATE_IDLE:
            speech_prep_feed_idle(buf, n);
            break;

        case VAD_STATE_SPEAKING: {
            bool first = (voice_len == 0);
            voice_len += n;
            if ((voice_len % 3200) == n) printf("VAD: speaking (%d samples)\n", voice_len);

            if (first) {
                asr_display_begin_sentence();
                asr_pending_clear(&asr_pending_count);
                asr_start_pending = asr_streaming = asr_preconnected = false;
                asr_wait_logged = asr_feed_full_logged = false;
                next_asr_start_retry = 0;
                TickType_t now_tick = xTaskGetTickCount();
                if (!network_has_ip()) {
                    printf("  -> WiFi not ready, skipping ASR\n");
                } else if (now_tick < asr_cooldown_until) {
                    printf("  -> ASR cooling down, skipping this utterance\n");
                } else if (audio_stream_accepting()) {
                    asr_streaming = true;
                } else {
                    asr_start_pending = true;
                }
            }

            if (asr_streaming || asr_start_pending) {
                memcpy(asr_buf, buf, n * sizeof(int16_t));
                asr_pcm_auto_gain(asr_buf, n);
                if (voice_len <= n || voice_len - last_audio_diag >= 16000) {
                    log_asr_audio_stats(buf, asr_buf, n, voice_len, speech_prep_noise_rms());
                    last_audio_diag = voice_len;
                }
            }

            if (asr_start_pending) {
                asr_pending_push(asr_pending, asr_pending_counts, &asr_pending_count, asr_buf, n);
                if (voice_len >= next_asr_start_retry) {
                    next_asr_start_retry = voice_len + ASR_START_RETRY_SAMPLES;
                    if (!audio_stream_active()) {
                        if (audio_stream_start(asr_result_cb) == ESP_OK) {
                            asr_streaming = true; asr_start_pending = false;
                            asr_pending_feed(asr_pending, asr_pending_counts, &asr_pending_count);
                        } else {
                            asr_cooldown_until = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
                            asr_start_pending = false;
                            asr_pending_clear(&asr_pending_count);
                        }
                    }
                }
            } else if (asr_streaming) {
                esp_err_t fe = audio_stream_feed(asr_buf, n);
                if (fe != ESP_OK && fe != ESP_ERR_TIMEOUT) {
                    printf("  -> audio_stream_feed failed: %s\n", esp_err_to_name(fe));
                    if (fe == ESP_ERR_INVALID_STATE) asr_streaming = false;
                }
            }

            if (voice_len >= 128000) { /* 8s 强制截断 */
                printf("VAD: force done at %.1fs\n", (float)voice_len / 16000.0f);
                vad_reset();
                if (asr_streaming) { audio_stream_finish(); asr_streaming = false; }
                asr_start_pending = asr_preconnected = asr_wait_logged = asr_feed_full_logged = false;
                asr_pending_clear(&asr_pending_count);
                voice_len = 0; last_audio_diag = -16000; speech_prep_reset();
            }
            break;
        }

        case VAD_STATE_DONE: {
            printf("VAD: done, %d samples (%.1fs)\n", voice_len, (float)voice_len / 16000.0f);

            if (!asr_streaming && asr_start_pending && asr_pending_count > 0) {
                for (int w = 0; w < 3500 && audio_stream_active(); w += 50) vTaskDelay(pdMS_TO_TICKS(50));
                if (!audio_stream_active()) {
                    if (audio_stream_start(asr_result_cb) == ESP_OK) {
                        asr_streaming = true;
                        asr_pending_feed(asr_pending, asr_pending_counts, &asr_pending_count);
                    } else {
                        asr_cooldown_until = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
                    }
                }
            }
            if (asr_streaming) { audio_stream_finish(); }
            asr_streaming = asr_start_pending = asr_preconnected = asr_wait_logged = asr_feed_full_logged = false;
            asr_pending_clear(&asr_pending_count);
            voice_len = 0; last_audio_diag = -16000;
            speech_prep_reset(); vad_reset();
            break;
        }
        default: break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ---- 主函数 ---- */
void app_main(void)
{
    /* RTC 初始化 */
    {
        struct tm tm = {0}; char mon[4]; int d, y, h, min;
        sscanf(__DATE__, "%3s %d %d", mon, &d, &y);
        sscanf(__TIME__, "%d:%d", &h, &min);
        tm.tm_mday = d; tm.tm_year = y - 1900; tm.tm_hour = h; tm.tm_min = min; tm.tm_sec = 0;
        tm.tm_mon = (strcmp(mon,"Jan")==0)?0:(strcmp(mon,"Feb")==0)?1:(strcmp(mon,"Mar")==0)?2:
                     (strcmp(mon,"Apr")==0)?3:(strcmp(mon,"May")==0)?4:(strcmp(mon,"Jun")==0)?5:
                     (strcmp(mon,"Jul")==0)?6:(strcmp(mon,"Aug")==0)?7:(strcmp(mon,"Sep")==0)?8:
                     (strcmp(mon,"Oct")==0)?9:(strcmp(mon,"Nov")==0)?10:11;
        setenv("TZ", "CST-8", 1); tzset();
        struct timeval tv = { .tv_sec = mktime(&tm), .tv_usec = 0 };
        settimeofday(&tv, NULL);
        printf("RTC set\n");
    }

    printf("Start\n");

    esp_lcd_panel_handle_t lcd   = lcd_st7789_init();
    esp_lcd_touch_handle_t touch = touch_cst816t_init();
    lvgl_port_init(lcd, touch);
    ui_init();
    lv_timer_create(rtc_update_cb, 1000, NULL);
    alarm_ui_init();
    lvgl_port_start();

    /* WiFi AP + TCP Server */
    wifi_mqtt_start();
    // wifi_mqtt_start();  /* 稍后启用 */

    if (audio_mic_init() == ESP_OK) {
        xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 3, NULL, 1);
        printf("Audio task started\n");
    }
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
