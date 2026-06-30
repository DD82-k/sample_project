/*
 * P169H002-CTP Smartwatch
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
#include "esp_sntp.h"
#include "touch_cst816t.h"
#include "lvgl_port.h"
#include "ui/ui.h"
#include "wifi_mqtt.h"
#include "audio_mic.h"
#include "vad.h"
#include "alarm_detect.h"
#include "speech_prep.h"
#include "audio_upload.h"
#include "esp_netif.h"

typedef struct {
    float rms;
    int peak;
    int min;
    int max;
} pcm_stats_t;

static pcm_stats_t pcm_stats_calc(const int16_t *pcm, int samples)
{
    pcm_stats_t st = {
        .rms = 0.0f,
        .peak = 0,
        .min = 32767,
        .max = -32768,
    };
    int64_t sum = 0;

    for (int i = 0; i < samples; i++) {
        int v = pcm[i];
        int a = v < 0 ? -v : v;
        if (a > st.peak) st.peak = a;
        if (v < st.min) st.min = v;
        if (v > st.max) st.max = v;
        sum += (int64_t)v * v;
    }

    if (samples > 0) {
        st.rms = sqrtf((float)sum / (float)samples);
    }
    return st;
}

static void log_asr_audio_stats(const int16_t *raw, const int16_t *upload, int samples, int voice_len)
{
    pcm_stats_t r = pcm_stats_calc(raw, samples);
    pcm_stats_t u = pcm_stats_calc(upload, samples);
    printf("ASR audio[%d]: raw rms=%.1f peak=%d min=%d max=%d, upload rms=%.1f peak=%d min=%d max=%d, noise=%.1f\n",
           voice_len, r.rms, r.peak, r.min, r.max,
           u.rms, u.peak, u.min, u.max, speech_prep_noise_rms());
}

static void asr_pcm_auto_gain(int16_t *pcm, int samples)
{
    pcm_stats_t st = pcm_stats_calc(pcm, samples);
    if (st.rms < 1.0f || st.peak < 8) {
        return;
    }

    float gain = 2000.0f / st.rms;
    if (gain < 1.0f) {
        gain = 1.0f;
    } else if (gain > 6.0f) {
        gain = 6.0f;
    }

    if ((float)st.peak * gain > 22000.0f) {
        gain = 22000.0f / (float)st.peak;
    }

    for (int i = 0; i < samples; i++) {
        float y = (float)pcm[i] * gain;
        if (y > 32767.0f) {
            y = 32767.0f;
        } else if (y < -32768.0f) {
            y = -32768.0f;
        }
        pcm[i] = (int16_t)y;
    }
}

#define MIC_FRAME_SAMPLES        320
#define ASR_PENDING_FRAMES       20
#define ASR_START_RETRY_SAMPLES  1600

static bool network_has_ip(void)
{
    esp_netif_t *netif = NULL;
    while ((netif = esp_netif_next(netif)) != NULL) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            return true;
        }
    }
    return false;
}

static void asr_pending_clear(int *frame_count)
{
    *frame_count = 0;
}

static void asr_pending_push(int16_t *pcm_frames, int *sample_counts,
                             int *frame_count, const int16_t *pcm, int samples)
{
    if (!pcm_frames || !sample_counts || !frame_count || !pcm || samples <= 0) {
        return;
    }

    if (*frame_count >= ASR_PENDING_FRAMES) {
        memmove(pcm_frames, pcm_frames + MIC_FRAME_SAMPLES,
                (ASR_PENDING_FRAMES - 1) * MIC_FRAME_SAMPLES * sizeof(int16_t));
        memmove(sample_counts, sample_counts + 1,
                (ASR_PENDING_FRAMES - 1) * sizeof(int));
        *frame_count = ASR_PENDING_FRAMES - 1;
    }

    int idx = *frame_count;
    memcpy(pcm_frames + idx * MIC_FRAME_SAMPLES, pcm, samples * sizeof(int16_t));
    sample_counts[idx] = samples;
    *frame_count = idx + 1;
}

static void asr_pending_feed(int16_t *pcm_frames, int *sample_counts, int *frame_count)
{
    for (int i = 0; i < *frame_count; i++) {
        esp_err_t feed_err = audio_stream_feed(pcm_frames + i * MIC_FRAME_SAMPLES,
                                               sample_counts[i]);
        if (feed_err != ESP_OK) {
            if (feed_err != ESP_ERR_TIMEOUT) {
                printf("  -> audio_stream_feed cached failed: %s\n", esp_err_to_name(feed_err));
            }
            break;
        }
    }
    asr_pending_clear(frame_count);
}

extern lv_obj_t * uic_LabelContent;
extern lv_obj_t * ui_LabelTime;

/* 报警 UI 标志 (audio task 写入, LVGL task 读取) */
static volatile bool alarm_ui_active = false;
static volatile bool asr_user_active = false;
static char saved_text[512] = {0};
static size_t asr_line_start = 0;
static bool asr_line_active = false;

#define ASR_DISPLAY_COLS_PER_LINE 24
#define ASR_DISPLAY_MAX_LINES     8

static int utf8_char_bytes(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xe0) == 0xc0) return 2;
    if ((c & 0xf0) == 0xe0) return 3;
    if ((c & 0xf8) == 0xf0) return 4;
    return 1;
}

static int display_estimated_lines(const char *s)
{
    int lines = 1;
    int cols = 0;

    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c == '\n') {
            lines++;
            cols = 0;
            s++;
            continue;
        }

        int char_cols = (c < 0x80) ? 1 : 2;
        int step = utf8_char_bytes(c);
        if (cols > 0 && cols + char_cols > ASR_DISPLAY_COLS_PER_LINE) {
            lines++;
            cols = 0;
        }
        cols += char_cols;
        s += step;
    }
    return lines;
}

static void asr_display_reset(void)
{
    saved_text[0] = '\0';
    asr_line_start = 0;
    asr_line_active = false;
}

static void asr_display_begin_sentence(void)
{
    asr_line_active = false;
}

static void asr_display_set_text(const char *text)
{
    char candidate[sizeof(saved_text)];
    size_t start = 0;

    if (!text || !text[0]) {
        return;
    }

    if (asr_line_active) {
        size_t prefix_len = asr_line_start;
        if (prefix_len >= sizeof(candidate)) {
            prefix_len = sizeof(candidate) - 1;
        }
        memcpy(candidate, saved_text, prefix_len);
        candidate[prefix_len] = '\0';
        strncat(candidate, text, sizeof(candidate) - strlen(candidate) - 1);
        start = asr_line_start;
    } else if (saved_text[0]) {
        start = strlen(saved_text) + 1;
        strncpy(candidate, saved_text, sizeof(candidate) - 1);
        candidate[sizeof(candidate) - 1] = '\0';
        strncat(candidate, "\n", sizeof(candidate) - strlen(candidate) - 1);
        strncat(candidate, text, sizeof(candidate) - strlen(candidate) - 1);
    } else {
        start = 0;
        snprintf(candidate, sizeof(candidate), "%s", text);
    }

    if (display_estimated_lines(candidate) > ASR_DISPLAY_MAX_LINES) {
        snprintf(candidate, sizeof(candidate), "%s", text);
        start = 0;
    }

    strncpy(saved_text, candidate, sizeof(saved_text) - 1);
    saved_text[sizeof(saved_text) - 1] = '\0';
    asr_line_start = start;
    asr_line_active = true;
    lv_label_set_text(uic_LabelContent, saved_text);
}

static const char *emotion_label_zh(const char *emotion)
{
    if (!emotion || !emotion[0]) return NULL;
    if (strcmp(emotion, "angry") == 0) return "生气";
    if (strcmp(emotion, "happy") == 0) return "开心";
    if (strcmp(emotion, "neutral") == 0) return "平静";
    if (strcmp(emotion, "sad") == 0) return "悲伤";
    if (strcmp(emotion, "surprise") == 0) return "惊讶";
    return emotion;
}

static void asr_display_set_result(const char *text, const char *emotion)
{
    const char *label = emotion_label_zh(emotion);
    char line[384];

    if (label && label[0]) {
        snprintf(line, sizeof(line), "%s [%s]", text, label);
        asr_display_set_text(line);
    } else {
        asr_display_set_text(text);
    }
}
static bool alarm_dismissed = false;   /* 双击消除后阻止重触发 */

void Button_Clear(lv_event_t * e)
{
    asr_display_reset();
    lv_label_set_text(uic_LabelContent, "");
}

/* ============= 报警覆盖层 ============= */
static lv_obj_t *alarm_overlay = NULL;
static lv_obj_t *alarm_box = NULL;
static lv_obj_t *alarm_label = NULL;
static lv_obj_t *alarm_dismiss_btn = NULL;

/* 双击检测: 记录上次点击时间 */
static uint32_t last_click_tick = 0;

static void alarm_dismiss_click_cb(lv_event_t *e)
{
    uint32_t now = lv_tick_get();
    /* 两次点击间隔 < 600ms 视为双击 */
    if (now - last_click_tick < 600) {
        /* 双击 → 消除报警, 不再重触发 */
        alarm_ui_active = false;
        alarm_dismissed = true;
    }
    last_click_tick = now;
}

static void alarm_overlay_create(void)
{
    lv_obj_t *scr = lv_scr_act();

    /* 全屏覆盖层 (LCD反转: 写0x00FFFF→显示红色) */
    alarm_overlay = lv_obj_create(scr);
    lv_obj_set_size(alarm_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(alarm_overlay, lv_color_hex(0x00FFFF), 0);
    lv_obj_set_style_bg_opa(alarm_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(alarm_overlay, 0, 0);
    lv_obj_set_style_radius(alarm_overlay, 0, 0);
    lv_obj_add_flag(alarm_overlay, LV_OBJ_FLAG_HIDDEN);

    /* 中间提示框 */
    alarm_box = lv_obj_create(alarm_overlay);
    lv_obj_set_size(alarm_box, lv_pct(90), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(alarm_box, 20, 0);
    lv_obj_set_style_bg_color(alarm_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(alarm_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(alarm_box, 0, 0);
    lv_obj_set_style_radius(alarm_box, 12, 0);
    lv_obj_center(alarm_box);

    alarm_label = lv_label_create(alarm_box);
    lv_label_set_text(alarm_label, "检测到警报声！");
    lv_obj_set_style_text_color(alarm_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(alarm_label, &ui_font_font3Alibaba, LV_PART_MAIN);
    lv_obj_center(alarm_label);

    /* 底部双击返回按钮 */
    alarm_dismiss_btn = lv_btn_create(alarm_overlay);
    lv_obj_set_size(alarm_dismiss_btn, lv_pct(80), 40);
    lv_obj_set_style_bg_color(alarm_dismiss_btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(alarm_dismiss_btn, LV_OPA_60, 0);
    lv_obj_set_style_radius(alarm_dismiss_btn, 8, 0);
    lv_obj_align(alarm_dismiss_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_add_event_cb(alarm_dismiss_btn, alarm_dismiss_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(alarm_dismiss_btn);
    lv_label_set_text(btn_label, "双击返回");
    lv_obj_set_style_text_color(btn_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(btn_label, &ui_font_font3Alibaba, LV_PART_MAIN);
    lv_obj_center(btn_label);
}

/* LVGL timer — 每 500ms 检查报警标志并刷新闪烁 */
static void alarm_ui_check_cb(lv_timer_t *timer)
{
    static bool last_state = false;

    if (alarm_ui_active != last_state) {
        last_state = alarm_ui_active;
        if (alarm_ui_active) {
            if (!alarm_overlay) alarm_overlay_create();
            lv_obj_clear_flag(alarm_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(alarm_overlay);
        } else {
            if (alarm_overlay) lv_obj_add_flag(alarm_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(uic_LabelContent, saved_text);
        }
    }

    if (alarm_ui_active && alarm_overlay) {
        static bool flash_state = false;
        flash_state = !flash_state;
        lv_color_t c = flash_state ? lv_color_hex(0x00FFFF) : lv_color_hex(0xFFFFFF);
        lv_obj_set_style_bg_color(alarm_overlay, c, 0);
    }
}

/* ============= Doubao ============= */
/* ============= 时钟 ============= */
static void rtc_update_cb(lv_timer_t *timer)
{
    time_t now;
    struct tm timeinfo;
    char buf[16];

    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
    lv_label_set_text(ui_LabelTime, buf);
}

/* ============= 报警事件回调 ============= */
static void on_alarm(alarm_type_t type, bool active)
{
    printf("ALARM: %s (type=%d)\n", active ? "ACTIVE" : "CLEAR", type);
    if (active && alarm_dismissed) return;
    if (!active) alarm_dismissed = false;
    alarm_ui_active = active;
}

/* ASR 回调 — 显示转写结果 */
static void asr_result_cb(const char *text, const char *emotion, esp_err_t status)
{
    if (status != ESP_OK && !asr_user_active) {
        return;
    }

    lvgl_port_lock();
    if (status == ESP_OK && text) {
        asr_display_set_result(text, emotion);
    } else {
        asr_display_reset();
        lv_label_set_text(uic_LabelContent, "语音识别失败");
    }
    lvgl_port_unlock();
}

/* ============= 麦克风采集 + VAD + 报警 + ASR ============= */
static void audio_task(void *arg)
{
    /* 帧缓冲 (单帧 320 样本 = 20ms @16kHz) */
    int16_t *buf = heap_caps_malloc(320 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) { printf("audio: malloc failed\n"); vTaskDelete(NULL); return; }
    int16_t *asr_buf = heap_caps_malloc(320 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!asr_buf) { free(buf); printf("audio: asr_buf malloc failed\n"); vTaskDelete(NULL); return; }
    int16_t *asr_pending = heap_caps_malloc(ASR_PENDING_FRAMES * MIC_FRAME_SAMPLES * sizeof(int16_t),
                                            MALLOC_CAP_SPIRAM);
    int *asr_pending_counts = heap_caps_malloc(ASR_PENDING_FRAMES * sizeof(int),
                                               MALLOC_CAP_SPIRAM);
    if (!asr_pending || !asr_pending_counts) {
        free(buf);
        free(asr_buf);
        free(asr_pending);
        free(asr_pending_counts);
        printf("audio: asr pending malloc failed\n");
        vTaskDelete(NULL);
        return;
    }

    int voice_len = 0;
    bool asr_streaming = false;
    bool asr_start_pending = false;
    bool asr_preconnected = false;
    bool asr_wait_logged = false;
    bool asr_feed_full_logged = false;
    int asr_pending_count = 0;
    int next_asr_start_retry = 0;
    int last_audio_diag = -16000;

    vad_init(220, 90);
    alarm_detect_init(0, 0);
    alarm_detect_set_callback(on_alarm);
    speech_prep_init(3000.0f, 12.0f);

    while (1) {
        int n = audio_mic_read(buf, 320);
        if (n < 1) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        static int alarm_div = 0;
        if (++alarm_div >= 5) {
            alarm_div = 0;
            alarm_detect_feed(buf, n);
        }
        vad_feed(buf, n);

        switch (vad_state()) {
        case VAD_STATE_IDLE:
            speech_prep_feed_idle(buf, n);
            if (!asr_preconnected && !audio_stream_active() && network_has_ip()) {
                esp_err_t asr_err = audio_stream_start(asr_result_cb);
                if (asr_err == ESP_OK) {
                    asr_preconnected = true;
                    printf("  -> ASR preconnect started\n");
                }
            } else if (asr_preconnected && !audio_stream_accepting()) {
                asr_preconnected = false;
            }
            break;

        case VAD_STATE_SPEAKING: {
            bool first_voice_frame = (voice_len == 0);
            voice_len += n;
            if ((voice_len % 3200) == n) printf("VAD: speaking (%d samples)\n", voice_len);

            /* 在屏幕上显示 "聆听中..." (仅在第一次显示) */
            if (first_voice_frame) {
                asr_user_active = true;
                asr_display_begin_sentence();
                lvgl_port_lock();
                lv_label_set_text(uic_LabelContent, "聆听中...");
                lvgl_port_unlock();
            }

            if (first_voice_frame) {
                asr_pending_clear(&asr_pending_count);
                asr_start_pending = false;
                asr_preconnected = false;
                asr_wait_logged = false;
                asr_feed_full_logged = false;
                next_asr_start_retry = 0;
                if (!network_has_ip()) {
                    printf("  -> WiFi not ready, skipping realtime ASR\n");
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
                    log_asr_audio_stats(buf, asr_buf, n, voice_len);
                    last_audio_diag = voice_len;
                }
            }

            if (asr_start_pending) {
                asr_pending_push(asr_pending, asr_pending_counts,
                                 &asr_pending_count, asr_buf, n);
                if (voice_len >= next_asr_start_retry) {
                    next_asr_start_retry = voice_len + ASR_START_RETRY_SAMPLES;
                    if (audio_stream_active()) {
                        if (!asr_wait_logged) {
                            printf("  -> ASR stream closing, caching new speech\n");
                            asr_wait_logged = true;
                        }
                    } else {
                        esp_err_t asr_err = audio_stream_start(asr_result_cb);
                        if (asr_err == ESP_OK) {
                            asr_streaming = true;
                            asr_start_pending = false;
                            asr_pending_feed(asr_pending, asr_pending_counts, &asr_pending_count);
                        } else {
                            printf("  -> audio_stream_start failed: %s\n", esp_err_to_name(asr_err));
                            lvgl_port_lock();
                            lv_label_set_text(uic_LabelContent, "语音识别启动失败");
                            lvgl_port_unlock();
                            asr_start_pending = false;
                            asr_pending_clear(&asr_pending_count);
                        }
                    }
                }
            } else if (asr_streaming) {
                esp_err_t feed_err = audio_stream_feed(asr_buf, n);
                if (feed_err != ESP_OK) {
                    if (feed_err == ESP_ERR_TIMEOUT) {
                        if (!asr_feed_full_logged) {
                            printf("  -> ASR upload queue full, dropping realtime frames\n");
                            asr_feed_full_logged = true;
                        }
                    } else {
                        printf("  -> audio_stream_feed failed: %s\n", esp_err_to_name(feed_err));
                        if (feed_err == ESP_ERR_INVALID_STATE) {
                            asr_streaming = false;
                        }
                    }
                }
            }
            if (voice_len >= 128000) {
                printf("VAD: force done at %.1f sec\n", (float)voice_len / 16000.0f);
                vad_reset();
                if (asr_streaming) {
                    audio_stream_finish();
                    asr_streaming = false;
                }
                asr_start_pending = false;
                asr_preconnected = false;
                asr_wait_logged = false;
                asr_feed_full_logged = false;
                asr_pending_clear(&asr_pending_count);
                voice_len = 0;
                asr_user_active = false;
                last_audio_diag = -16000;
                speech_prep_reset();
            }
            break;
        }

        case VAD_STATE_DONE: {
            printf("VAD: done, total %d samples (%.1f sec)\n",
                   voice_len, (float)voice_len / 16000.0f);

            if (alarm_detect_type() != ALARM_NONE) {
                printf("  -> alarm also active, overlay shown\n");
            }

            if (!asr_streaming && asr_start_pending && asr_pending_count > 0) {
                for (int wait_ms = 0; wait_ms < 3500 && audio_stream_active(); wait_ms += 50) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                if (!audio_stream_active()) {
                    esp_err_t asr_err = audio_stream_start(asr_result_cb);
                    if (asr_err == ESP_OK) {
                        asr_streaming = true;
                        asr_pending_feed(asr_pending, asr_pending_counts, &asr_pending_count);
                    } else {
                        printf("  -> audio_stream_start cached failed: %s\n", esp_err_to_name(asr_err));
                    }
                } else {
                    printf("  -> ASR stream still busy, dropped cached speech\n");
                }
            }

            if (asr_streaming) {
                if (voice_len >= 1600) {  /* 至少 0.1s 语音才有意义 */
                    lvgl_port_lock();
                    lv_label_set_text(uic_LabelContent, "识别中...");
                    lvgl_port_unlock();

                    esp_err_t finish_err = audio_stream_finish();
                    if (finish_err != ESP_OK) {
                        printf("  -> audio_stream_finish failed: %s\n", esp_err_to_name(finish_err));
                    }
                } else {
                    audio_stream_finish();
                }
            }
            asr_streaming = false;
            asr_start_pending = false;
            asr_preconnected = false;
            asr_wait_logged = false;
            asr_feed_full_logged = false;
            asr_pending_clear(&asr_pending_count);
            voice_len = 0;
            asr_user_active = false;
            last_audio_diag = -16000;
            speech_prep_reset();
            vad_reset();
            break;
        }

        default: break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ============= 主函数 ============= */
void app_main(void)
{
    /* Set RTC using compile-time date/time as initial seed */
    {
        struct tm tm = {0};
        char mon[4]; int d, y, h, min;
        sscanf(__DATE__, "%3s %d %d", mon, &d, &y);
        sscanf(__TIME__, "%d:%d", &h, &min);
        tm.tm_mday = d;
        tm.tm_mon  = (strcmp(mon,"Jan")==0)?0:(strcmp(mon,"Feb")==0)?1:(strcmp(mon,"Mar")==0)?2:
                      (strcmp(mon,"Apr")==0)?3:(strcmp(mon,"May")==0)?4:(strcmp(mon,"Jun")==0)?5:
                      (strcmp(mon,"Jul")==0)?6:(strcmp(mon,"Aug")==0)?7:(strcmp(mon,"Sep")==0)?8:
                      (strcmp(mon,"Oct")==0)?9:(strcmp(mon,"Nov")==0)?10:11;
        tm.tm_year = y - 1900;
        tm.tm_hour = h;
        tm.tm_min  = min;
        tm.tm_sec  = 0;
        setenv("TZ", "CST-8", 1); tzset();
        struct timeval tv = { .tv_sec = mktime(&tm), .tv_usec = 0 };
        printf("RTC set: epoch=%lld, UTC=", (long long)tv.tv_sec);
        { time_t t = tv.tv_sec; struct tm g; gmtime_r(&t, &g); char b[32]; strftime(b,32,"%Y-%m-%dT%H:%M:%SZ",&g); printf("%s\n", b); }
        settimeofday(&tv, NULL);
    }

    printf("Start\n");

    esp_lcd_panel_handle_t lcd   = lcd_st7789_init();
    esp_lcd_touch_handle_t touch = touch_cst816t_init();
    lvgl_port_init(lcd, touch);
    ui_init();

    /* Start RTC display update — runs every 1000ms inside lv_timer_handler */
    lv_timer_create(rtc_update_cb, 1000, NULL);
    /* Alarm UI check — 500ms interval */
    lv_timer_create(alarm_ui_check_cb, 500, NULL);

    lvgl_port_start();

    /* Start WiFi + OneNet MQTT in background */
    wifi_mqtt_start();

    /* Start microphone audio capture */
    if (audio_mic_init() == ESP_OK) {
        xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 3, NULL, 1);
        printf("Audio task started\n");
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
