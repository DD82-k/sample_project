/*
 * VAD 实现 — 能量阈值状态机
 *
 *   安静 RMS < idle_thresh → 计数器归零, IDLE
 *   [IDLE] 连续 5 帧 RMS > start_thresh → SPEAKING (防误触发)
 *   [SPEAKING] 连续 15 帧 RMS < idle_thresh → DONE (防句中停顿)
 *
 * 参数经验值 (16kHz 单声道, 320 样本/帧 = 20ms):
 *   start_thresh = 2000  说话开始阈值
 *   idle_thresh  = 800   静音阈值
 */
#include "vad.h"
#include <stddef.h>
#include <string.h>

/* 可调参数 */
#define START_FRAMES  5    /* 连续触发帧数: 100ms */
#define IDLE_FRAMES   15   /* 连续静音帧数: 300ms */

static int  start_thr   = 2000;
static int  idle_thr    = 800;
static int  active_cnt  = 0;   /* 连续触发帧计数 */
static int  silence_cnt = 0;   /* 连续静音帧计数 */
static vad_state_t cur_state = VAD_STATE_IDLE;

void vad_init(int start_thresh, int idle_thresh)
{
    start_thr   = start_thresh;
    idle_thr    = idle_thresh;
    active_cnt  = 0;
    silence_cnt = 0;
    cur_state   = VAD_STATE_IDLE;
}

void vad_feed(const int16_t *pcm, int samples)
{
    /* 计算当前帧 RMS */
    int64_t sum = 0;
    for (int i = 0; i < samples; i++) sum += (int32_t)pcm[i] * pcm[i];
    int rms = (int)(sum / samples);

    if (rms > start_thr) {
        active_cnt++;
        silence_cnt = 0;
    } else if (rms < idle_thr) {
        silence_cnt++;
        active_cnt = 0;
    } else {
        /* 中间过渡区: 不改变计数 */
    }

    switch (cur_state) {
    case VAD_STATE_IDLE:
        if (active_cnt >= START_FRAMES) {
            cur_state = VAD_STATE_SPEAKING;
            active_cnt = 0;
        }
        break;

    case VAD_STATE_SPEAKING:
        if (silence_cnt >= IDLE_FRAMES) {
            cur_state = VAD_STATE_DONE;
            silence_cnt = 0;
        }
        break;

    case VAD_STATE_DONE:
        /* 等待外部调用 vad_reset() */
        break;
    }
}

vad_state_t vad_state(void)   { return cur_state; }

void vad_reset(void)
{
    cur_state   = VAD_STATE_IDLE;
    active_cnt  = 0;
    silence_cnt = 0;
}
