/* asr_display.h — 对话记录显示 + ASR 回调 + 音频辅助 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* PCM 统计 */
typedef struct { float rms; int peak, min, max; } pcm_stats_t;
pcm_stats_t pcm_stats_calc(const int16_t *pcm, int samples);
void        asr_pcm_auto_gain(int16_t *pcm, int samples);
void        log_asr_audio_stats(const int16_t *raw, const int16_t *upload, int samples, int voice_len, float noise_rms);

/* 语音缓冲队列 (ASR 启动前暂存) */
void asr_pending_clear(int *frame_count);
void asr_pending_push(int16_t *pcm_frames, int *sample_counts, int *frame_count, const int16_t *pcm, int samples);
void asr_pending_feed(int16_t *pcm_frames, int *sample_counts, int *frame_count);

/* 对话显示 */
void asr_display_reset(void);                       /* 清空记录 */
void asr_display_begin_sentence(void);              /* 新一句话开始 */
void asr_display_set_text(const char *text);        /* 追加文字 */
void asr_display_set_result(const char *text, const char *emotion); /* ASR 结果 */
void asr_display_restore_current(void);              /* 报警解除后恢复对话 */

/* ASR 回调 (给 audio_upload 用) */
void asr_result_cb(const char *text, const char *emotion, esp_err_t status);

/* 网络检测 */
bool network_has_ip(void);

/* 常量 */
#define MIC_FRAME_SAMPLES        320
#define ASR_PENDING_FRAMES       20
#define ASR_START_RETRY_SAMPLES  1600
