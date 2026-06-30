#ifndef AUDIO_UPLOAD_H
#define AUDIO_UPLOAD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback for ASR text and optional emotion.
 *
 * text/emotion are valid only during the callback. Copy them if you need to
 * keep them. emotion can be NULL when the server has not returned it yet.
 */
typedef void (*asr_callback_t)(const char *text, const char *emotion, esp_err_t status);

/**
 * Start a Volcengine ASR bidirectional streaming WebSocket session.
 */
esp_err_t audio_stream_start(asr_callback_t callback);

/**
 * Feed one 16 kHz/16-bit/mono PCM frame into the active stream.
 */
esp_err_t audio_stream_feed(const int16_t *pcm, int sample_count);

/**
 * Send the final frame and let the stream drain the server's final result.
 */
esp_err_t audio_stream_finish(void);

/**
 * Returns true while a streaming ASR session exists.
 */
bool audio_stream_active(void);

/**
 * Returns true when a stream exists and can accept audio frames.
 */
bool audio_stream_accepting(void);

/**
 * Compatibility helper: stream an existing PCM buffer in 20 ms chunks.
 * New real-time code should use audio_stream_start/feed/finish.
 */
esp_err_t audio_upload_start(const int16_t *pcm, int sample_count,
                             asr_callback_t callback);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_UPLOAD_H */
