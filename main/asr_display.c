/* asr_display.c — 对话记录管理 + ASR 回调 + 音频辅助函数 */
#include "asr_display.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "ui/ui.h"
#include "audio_upload.h"
#include "speech_prep.h"
#include "esp_netif.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- 对话缓冲区 ---- */
static char saved_text[4096] = {0};
static size_t asr_line_start = 0;
static bool asr_line_active = false;
static volatile bool asr_user_active = false;

extern lv_obj_t *ui_Panel1;

/* ---- PCM 统计 ---- */
pcm_stats_t pcm_stats_calc(const int16_t *pcm, int samples)
{
    pcm_stats_t st = { .rms = 0, .peak = 0, .min = 32767, .max = -32768 };
    int64_t sum = 0;
    for (int i = 0; i < samples; i++) {
        int v = pcm[i], a = v < 0 ? -v : v;
        if (a > st.peak) st.peak = a;
        if (v < st.min) st.min = v;
        if (v > st.max) st.max = v;
        sum += (int64_t)v * v;
    }
    if (samples > 0) st.rms = sqrtf((float)sum / (float)samples);
    return st;
}

void asr_pcm_auto_gain(int16_t *pcm, int samples)
{
    pcm_stats_t st = pcm_stats_calc(pcm, samples);
    if (st.rms < 1.0f || st.peak < 8) return;
    float gain = 2000.0f / st.rms;
    if (gain < 1.0f) gain = 1.0f; else if (gain > 6.0f) gain = 6.0f;
    if ((float)st.peak * gain > 22000.0f) gain = 22000.0f / (float)st.peak;
    for (int i = 0; i < samples; i++) {
        float y = (float)pcm[i] * gain;
        if (y > 32767) y = 32767; else if (y < -32768) y = -32768;
        pcm[i] = (int16_t)y;
    }
}

void log_asr_audio_stats(const int16_t *raw, const int16_t *upload, int samples, int voice_len, float noise_rms)
{
    pcm_stats_t r = pcm_stats_calc(raw, samples);
    pcm_stats_t u = pcm_stats_calc(upload, samples);
    printf("ASR audio[%d]: raw rms=%.1f peak=%d, upload rms=%.1f peak=%d, noise=%.1f\n",
           voice_len, r.rms, r.peak, u.rms, u.peak, noise_rms);
}

/* ---- 语音缓冲队列 ---- */
void asr_pending_clear(int *frame_count) { *frame_count = 0; }

void asr_pending_push(int16_t *pcm_frames, int *sample_counts, int *frame_count,
                      const int16_t *pcm, int samples)
{
    if (!pcm_frames || !sample_counts || !frame_count || *frame_count >= ASR_PENDING_FRAMES) return;
    int idx = *frame_count;
    memcpy(pcm_frames + idx * MIC_FRAME_SAMPLES, pcm, samples * sizeof(int16_t));
    sample_counts[idx] = samples;
    *frame_count = idx + 1;
}

void asr_pending_feed(int16_t *pcm_frames, int *sample_counts, int *frame_count)
{
    for (int i = 0; i < *frame_count; i++) {
        esp_err_t err = audio_stream_feed(pcm_frames + i * MIC_FRAME_SAMPLES, sample_counts[i]);
        if (err != ESP_OK) break;
    }
    asr_pending_clear(frame_count);
}

/* ---- 网络检测 ---- */
bool network_has_ip(void)
{
    esp_netif_t *netif = NULL;
    while ((netif = esp_netif_next(netif)) != NULL) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) return true;
    }
    return false;
}

/* ---- 情绪 emoji ---- */
static const char *emotion_emoji(const char *emotion)
{
    if (!emotion || !emotion[0]) return "";
    if (strcmp(emotion, "angry") == 0)   return "\xF0\x9F\x98\xA0";  /* 😠 */
    if (strcmp(emotion, "happy") == 0)   return "\xF0\x9F\x98\x83";  /* 😃 */
    if (strcmp(emotion, "neutral") == 0) return "\xF0\x9F\x98\x90";  /* 😐 */
    if (strcmp(emotion, "sad") == 0)     return "\xF0\x9F\x98\xA2";  /* 😢 */
    if (strcmp(emotion, "surprise") == 0) return "\xF0\x9F\x98\xAE"; /* 😮 */
    return "";
}

/* ---- 对话显示 ---- */
void asr_display_reset(void) {
    saved_text[0] = '\0'; asr_line_start = 0; asr_line_active = false;
}

void asr_display_begin_sentence(void) { asr_line_active = false; }

void asr_display_set_text(const char *text)
{
    static char candidate[4096];
    size_t start = 0;
    if (!text || !text[0]) return;

    if (asr_line_active) {
        size_t n = asr_line_start;
        if (n >= sizeof(candidate)) n = sizeof(candidate) - 1;
        memcpy(candidate, saved_text, n);
        candidate[n] = '\0';
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
    strncpy(saved_text, candidate, sizeof(saved_text) - 1);
    saved_text[sizeof(saved_text) - 1] = '\0';
    asr_line_start = start;
    asr_line_active = true;
    lv_label_set_text(uic_LabelContent, saved_text);
    if (ui_Panel1) lv_obj_scroll_to_y(ui_Panel1, LV_COORD_MAX, LV_ANIM_OFF);
}

void asr_display_set_result(const char *text, const char *emotion)
{
    const char *e = emotion_emoji(emotion);
    char line[384];
    if (e[0]) snprintf(line, sizeof(line), "%s [%s]", text, e);
    else      snprintf(line, sizeof(line), "%s", text);
    asr_display_set_text(line);
}

void asr_display_restore_current(void)
{
    lv_label_set_text(uic_LabelContent, saved_text);
}

/* ---- ASR 回调 ---- */
void asr_result_cb(const char *text, const char *emotion, esp_err_t status)
{
    if (status != ESP_OK && !asr_user_active) return;
    lvgl_port_lock();
    if (status == ESP_OK && text) {
        asr_display_set_result(text, emotion);
    } else {
        printf("  -> ASR failed, status=%d\n", status);
    }
    lvgl_port_unlock();
}
