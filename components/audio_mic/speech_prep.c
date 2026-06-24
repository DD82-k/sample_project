/*
 * 语音预处理 — DC 去除 + 预加重 + 谱减法降噪 + 归一化
 *
 * 全部时域处理, 计算量极小, 适合 ESP32 实时运行
 */
#include "speech_prep.h"
#include <math.h>
#include <string.h>

/* 预加重系数: 典型 0.9~0.97, 越高高频提升越多 */
#define PRE_EMPH_COEFF  0.95f

/* DC 追踪低通截止频率系数 (α=0.995 → fc≈12Hz @16kHz) */
#define DC_ALPHA        0.995f

/* 噪声估计平滑系数 (EMA) */
#define NOISE_EMA_ALPHA 0.05f

static float target_rms     = 2000.0f;  /* 目标 RMS */
static float reduction_gain = 0.3f;     /* 降噪强度 (0~1, 越小降越多) */

/* 状态 */
static float prev_in     = 0.0f;   /* 预加重: x[n-1] */
static float dc_est      = 0.0f;   /* DC 估计 */
static float dc_out      = 0.0f;   /* DC 滤波器 y[n-1] */
static float noise_rms   = 10.0f;  /* 估计噪声 RMS */
static float gain        = 1.0f;   /* 当前归一化增益 */

void speech_prep_init(float tgt_rms, float reduction_db)
{
    target_rms = tgt_rms;
    /* reduction_db: 降噪 dB, 映射到 gain 0~1
     * 10dB → gain=0.1, 20dB → gain=0.01 */
    reduction_gain = powf(10.0f, -reduction_db / 20.0f);
    if (reduction_gain < 0.01f) reduction_gain = 0.01f;
    if (reduction_gain > 1.0f)  reduction_gain = 1.0f;
    speech_prep_reset();
}

void speech_prep_feed_idle(const int16_t *pcm, int samples)
{
    /* 更新噪声 RMS 估计 (EMA) */
    int64_t sum = 0;
    for (int i = 0; i < samples; i++) {
        sum += (int32_t)pcm[i] * pcm[i];
    }
    float rms = sqrtf((float)sum / (float)samples);

    /* 只往下估计 —— 安静期 RMS 总是 ≤ 当前值 */
    if (rms < noise_rms * 2.0f) {  /* 排除突发大值 */
        noise_rms = NOISE_EMA_ALPHA * rms + (1.0f - NOISE_EMA_ALPHA) * noise_rms;
    }
}

void speech_prep_process(int16_t *pcm, int samples)
{
    float frame_rms = 0.0f;

    for (int i = 0; i < samples; i++) {
        float x = (float)pcm[i];

        /* ---- 1. DC 去除: 1阶 HP, fc≈12Hz ---- */
        float dc_free = (x - prev_in) + DC_ALPHA * dc_out;
        prev_in = x;
        dc_out = dc_free;

        /* ---- 2. 预加重: y[n] = x[n] - coeff * x[n-1] ---- */
        float emph = dc_free - PRE_EMPH_COEFF * dc_est;
        dc_est = dc_free;

        /* ---- 3. 谱减法 (时域近似): 减去噪声估计 ---- */
        float noise_floor = noise_rms * reduction_gain;
        float sign = (emph >= 0.0f) ? 1.0f : -1.0f;
        float mag = fabsf(emph);
        float clean = sign * (mag > noise_floor ? mag - noise_floor : 0.0f);

        /* 累加 RMS */
        frame_rms += clean * clean;

        pcm[i] = (int16_t)clean;
    }

    /* ---- 4. AGC 归一化: 计算帧增益, 平滑更新 ---- */
    frame_rms = sqrtf(frame_rms / (float)samples);
    if (frame_rms > noise_rms) {
        float target_gain = target_rms / frame_rms;
        /* 增益平滑过渡 (10% 更新率, 避免音量突变) */
        gain = 0.1f * target_gain + 0.9f * gain;
        /* 限制增益范围 0.3~3.0 (防止过度放大噪声或削波) */
        if (gain < 0.3f) gain = 0.3f;
        if (gain > 3.0f) gain = 3.0f;
    }

    /* 应用增益并限幅 */
    for (int i = 0; i < samples; i++) {
        float y = (float)pcm[i] * gain;
        if (y > 32767.0f)  y = 32767.0f;
        if (y < -32768.0f) y = -32768.0f;
        pcm[i] = (int16_t)y;
    }
}

void speech_prep_reset(void)
{
    prev_in  = 0.0f;
    dc_est   = 0.0f;
    dc_out   = 0.0f;
    gain     = 1.0f;
    /* noise_rms 不重置 —— 保持历史估计 */
}

float speech_prep_noise_rms(void)
{
    return noise_rms;
}
