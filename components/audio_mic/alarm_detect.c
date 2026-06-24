/*
 * DSP 报警检测 — 多频点 Goertzel 频谱扫描 + 峰值/均值比 (PAR)
 *
 * 原理:
 *   报警音是窄带纯音 → 频谱能量集中在单一频点 → PAR 极高 (>8)
 *   人声/噪声是宽带信号 → 频谱能量分散 → PAR 低 (<5)
 *
 * 33 个 Goertzel 频点覆盖 2000~4000Hz (每 62.5Hz 一个),
 * 保证任意频率的纯音都不会漏检 (无频谱泄漏零点).
 *
 * 检测条件 (同时满足):
 *   1. RMS > 能量门限
 *   2. PAR > 5.0      (纯音 vs 宽带)
 *   3. alarm_ratio > 0.08  (频段能量集中度)
 *   4. 连续 6 帧 = 120ms
 */
#include "alarm_detect.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ---- 可调阈值 ---- */
#define ENERGY_GATE_RMS     18      /* RMS < 18 跳过 */
#define PAR_THRESH          8.0f    /* 峰值/均值比 (57 bin, 语音低频拉低PAR) */
#define PEAK_RATIO_THRESH   0.40f   /* 次高峰/最高峰 < 0.4 */
#define TRIGGER_FRAMES      6       /* 连续 6 帧触发 */
#define RELEASE_FRAMES      45      /* 连续 45 帧解除 */
#define STARTUP_MUTE        50      /* 上电静音帧数 */

/* ---- Goertzel 频点 ---- */
#define SAMPLE_RATE         16000
#define FRAME_SAMPLES       320
#define FREQ_START          500     /* Hz — 包含语音低频, 压低语音 PAR */
#define FREQ_END            4000    /* Hz */
#define FREQ_STEP           62.5f   /* Hz (DFT 分辨率偏移半格, 避免零点) */
#define NUM_BINS            57      /* (4000-500)/62.5 + 1 */

/* ---- 全局状态 ---- */
static int    trigger_cnt    = 0;
static int    release_cnt    = 0;
static bool   alarm_active   = false;
static int    startup_cnt    = 0;
static float  last_rms       = 0.0f;
static float  last_par       = 0.0f;
static float  last_peak_ratio = 0.0f;
static alarm_cb_t  user_cb   = NULL;

/* Goertzel 谐振器状态 — 用 double 避免大数相减精度丢失 */
static double g_s1[NUM_BINS];    /* s[n-1] */
static double g_s2[NUM_BINS];    /* s[n-2] */
static float  g_coeff[NUM_BINS]; /* 2*cos(2πk/N) */
static bool   g_inited = false;

/* ---- 初始化 Goertzel 系数 ---- */
static void goertzel_init(void)
{
    if (g_inited) return;
    int N = FRAME_SAMPLES;
    for (int b = 0; b < NUM_BINS; b++) {
        float freq = FREQ_START + (float)b * FREQ_STEP;
        float k = freq * (float)N / (float)SAMPLE_RATE;
        float omega = 2.0f * 3.14159265f * k / (float)N;
        g_coeff[b] = 2.0f * cosf(omega);
        g_s1[b] = 0.0;
        g_s2[b] = 0.0;
    }
    g_inited = true;
}

/* ---- 单帧多频点 Goertzel (一次遍历, double 精度) ---- */
static void goertzel_scan(const int16_t *pcm, int N,
                          float *par_out, float *peak_ratio_out)
{
    /* 重置谐振器 */
    for (int b = 0; b < NUM_BINS; b++) {
        g_s1[b] = 0.0;
        g_s2[b] = 0.0;
    }

    /* 一次遍历, 同时更新所有谐振器 */
    for (int i = 0; i < N; i++) {
        double x = (double)pcm[i];
        for (int b = 0; b < NUM_BINS; b++) {
            double s0 = x + (double)g_coeff[b] * g_s1[b] - g_s2[b];
            g_s2[b] = g_s1[b];
            g_s1[b] = s0;
        }
    }

    /* 计算各频点功率, 找最高峰和次高峰 (double 精度) */
    double max_pow = 0.0, second_pow = 0.0;
    double sum_pow = 0.0;
    for (int b = 0; b < NUM_BINS; b++) {
        double p = g_s1[b] * g_s1[b] + g_s2[b] * g_s2[b]
                 - (double)g_coeff[b] * g_s1[b] * g_s2[b];
        if (p < 0.0) p = 0.0;
        if (p > max_pow) {
            second_pow = max_pow;
            max_pow = p;
        } else if (p > second_pow) {
            second_pow = p;
        }
        sum_pow += p;
    }

    /* PAR = 峰值 / 均值 */
    double avg_pow = sum_pow / (double)NUM_BINS;
    *par_out = (avg_pow > 0.0) ? (float)(max_pow / avg_pow) : 1.0f;

    /* peak_ratio = 次高峰/最高峰: 纯音→0, 宽带噪声→0.7~1.0 */
    *peak_ratio_out = (max_pow > 0.0) ? (float)(second_pow / max_pow) : 1.0f;
}

/* ---- API ---- */
void alarm_detect_init(int zcr_thresh, int hi_energy_ratio)
{
    (void)zcr_thresh;
    (void)hi_energy_ratio;
    goertzel_init();
    trigger_cnt  = 0;
    release_cnt  = 0;
    alarm_active = false;
    startup_cnt  = 0;
    last_rms     = 0.0f;
    last_par     = 0.0f;
}

void alarm_detect_feed(const int16_t *pcm, int samples)
{
    /* 启动静音 */
    if (startup_cnt < STARTUP_MUTE) {
        startup_cnt++;
        return;
    }

    /* 能量门限 */
    int64_t sum = 0;
    for (int i = 0; i < samples; i++) sum += (int32_t)pcm[i] * pcm[i];
    last_rms = sqrtf((float)sum / (float)samples);
    if (last_rms < ENERGY_GATE_RMS) {
        if (alarm_active) {
            release_cnt++;
            if (release_cnt >= RELEASE_FRAMES) {
                alarm_active = false;
                if (user_cb) user_cb(ALARM_UNKNOWN, false);
            }
        }
        return;
    }

    /* 多频点扫描 */
    float par, peak_ratio;
    goertzel_scan(pcm, samples, &par, &peak_ratio);
    last_par = par;
    last_peak_ratio = peak_ratio;

    /* 判断: 高 PAR (纯音) + 次峰远低于主峰 (单频) */
    bool is_alarm = (par > PAR_THRESH) && (peak_ratio < PEAK_RATIO_THRESH);

    if (is_alarm) {
        trigger_cnt++;
        release_cnt = 0;
        if (!alarm_active && trigger_cnt >= TRIGGER_FRAMES) {
            alarm_active = true;
            if (user_cb) user_cb(ALARM_UNKNOWN, true);
        }
    } else {
        release_cnt++;
        trigger_cnt = 0;
        if (alarm_active && release_cnt >= RELEASE_FRAMES) {
            alarm_active = false;
            if (user_cb) user_cb(ALARM_UNKNOWN, false);
        }
    }
}

alarm_type_t alarm_detect_type(void)
{
    return alarm_active ? ALARM_UNKNOWN : ALARM_NONE;
}

void alarm_detect_set_callback(alarm_cb_t cb)
{
    user_cb = cb;
}

void alarm_detect_debug(float *zcr, float *hi_ratio, float *variance)
{
    if (zcr)       *zcr       = last_par;      /* PAR */
    if (hi_ratio)  *hi_ratio  = last_peak_ratio;   /* alarm_ratio */
    if (variance)  *variance  = 0.0f;
}

float alarm_detect_rms(void)
{
    return last_rms;
}
