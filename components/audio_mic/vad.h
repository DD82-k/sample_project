/*
 * VAD 人声检测 — 基于能量阈值状态机
 * 安静 RMS < 阈值 → 无人声
 * 连续触发帧超阈值 → 开始说话
 * 连续静音帧低于阈值 → 说话结束
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    VAD_STATE_IDLE,         /* 空闲, 环境安静 */
    VAD_STATE_SPEAKING,     /* 正在说话 */
    VAD_STATE_DONE          /* 刚说完, 等待读取结果后回到 IDLE */
} vad_state_t;

/* 初始化 VAD, start_thresh: 开始说话 RMS 阈值, idle_thresh: 静音 RMS 阈值 */
void vad_init(int start_thresh, int idle_thresh);

/* 喂一帧 PCM 数据, 样本数任意 */
void vad_feed(const int16_t *pcm, int samples);

/* 当前状态 */
vad_state_t vad_state(void);

/* 调用后 DONE → IDLE, 准备下一次检测 */
void vad_reset(void);
