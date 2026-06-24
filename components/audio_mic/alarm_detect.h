/*
 * 简易 DSP 报警音检测 — ZCR + 高频能量 + 周期脉冲
 *
 * 原理:
 *   人声频谱 200~4000Hz, ZCR 低, 能量分散
 *   火警/列车提示音 2kHz~5kHz, 高频窄带脉冲, ZCR 高且有周期性
 *
 * 检测逻辑:
 *   ZCR > 阈值 + 高频能量占比 > 阈值 + 连续 N 帧触发 = 报警
 *
 * 与 VAD 并行, 互不干扰: VAD 检测人声, alarm_detect 检测报警
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    ALARM_NONE = 0,
    ALARM_FIRE,       /* 火警警报 */
    ALARM_TRAIN,      /* 列车到站提示 */
    ALARM_UNKNOWN,    /* 未知高频警报 */
} alarm_type_t;

/* 初始化, 可传入自定义阈值为 0 则使用默认值 */
void alarm_detect_init(int zcr_thresh, int hi_energy_ratio);

/* 喂一帧 PCM, 计算 ZCR + 高频能量, 更新检测状态 */
void alarm_detect_feed(const int16_t *pcm, int samples);

/* 当前报警类型 (无报警返回 ALARM_NONE) */
alarm_type_t alarm_detect_type(void);

/* 报警事件回调 (触发时调一次, 解除时调一次) */
typedef void (*alarm_cb_t)(alarm_type_t type, bool active);
void alarm_detect_set_callback(alarm_cb_t cb);
void alarm_detect_debug(float *zcr, float *hi_ratio, float *variance);
float alarm_detect_rms(void);
