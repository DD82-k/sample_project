/*
 * Simple energy VAD for 16 kHz mono PCM.
 *
 * Thresholds are real RMS values, not mean-square values. The state machine
 * enters SPEAKING after several consecutive loud frames and enters DONE after
 * enough quiet frames or after a maximum utterance length.
 */
#include "vad.h"

#include <math.h>

#define START_FRAMES       4     /* 80 ms @ 20 ms/frame */
#define IDLE_FRAMES        35    /* 700 ms @ 20 ms/frame */
#define MAX_SPEECH_FRAMES  400   /* 8 s safety cap */

static int start_thr = 220;
static int idle_thr = 90;
static int active_cnt = 0;
static int silence_cnt = 0;
static int speech_frames = 0;
static vad_state_t cur_state = VAD_STATE_IDLE;

void vad_init(int start_thresh, int idle_thresh)
{
    start_thr = start_thresh;
    idle_thr = idle_thresh;
    active_cnt = 0;
    silence_cnt = 0;
    speech_frames = 0;
    cur_state = VAD_STATE_IDLE;
}

void vad_feed(const int16_t *pcm, int samples)
{
    if (!pcm || samples <= 0 || cur_state == VAD_STATE_DONE) {
        return;
    }

    int64_t sum = 0;
    for (int i = 0; i < samples; i++) {
        sum += (int32_t)pcm[i] * pcm[i];
    }
    int rms = (int)sqrtf((float)sum / (float)samples);

    if (cur_state == VAD_STATE_IDLE) {
        if (rms > start_thr) {
            active_cnt++;
        } else {
            active_cnt = 0;
        }

        if (active_cnt >= START_FRAMES) {
            cur_state = VAD_STATE_SPEAKING;
            active_cnt = 0;
            silence_cnt = 0;
            speech_frames = 0;
        }
        return;
    }

    if (cur_state == VAD_STATE_SPEAKING) {
        speech_frames++;
        if (rms < idle_thr) {
            silence_cnt++;
        } else if (rms > start_thr) {
            silence_cnt = 0;
        }

        if (silence_cnt >= IDLE_FRAMES || speech_frames >= MAX_SPEECH_FRAMES) {
            cur_state = VAD_STATE_DONE;
            silence_cnt = 0;
        }
    }
}

vad_state_t vad_state(void)
{
    return cur_state;
}

void vad_reset(void)
{
    cur_state = VAD_STATE_IDLE;
    active_cnt = 0;
    silence_cnt = 0;
    speech_frames = 0;
}
