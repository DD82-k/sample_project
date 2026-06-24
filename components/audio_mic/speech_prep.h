/*
 * 语音预处理 — DC 去除 + 预加重 + 噪声估计 + 谱减法降噪 + 归一化
 *
 * 处理链:
 *   raw PCM → DC removal → pre-emphasis → spectral subtraction → normalize → clean PCM
 *
 * 噪声估计:
 *   feed_idle() 在 VAD 安静期调用, 更新噪声 RMS 估计
 *   process() 在 VAD 说话期调用, 用估计的噪声做谱减法
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* 初始化, 设置目标 RMS 和降噪强度 */
void speech_prep_init(float target_rms, float reduction_db);

/* 喂入安静帧更新噪声估计 */
void speech_prep_feed_idle(const int16_t *pcm, int samples);

/* 处理语音帧: DC → 预加重 → 谱减法 → 归一化 */
void speech_prep_process(int16_t *pcm, int samples);

/* 重置状态 (新一轮语音开始前调用) */
void speech_prep_reset(void);

/* 获取当前噪声 RMS (调试用) */
float speech_prep_noise_rms(void);
