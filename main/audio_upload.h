#ifndef AUDIO_UPLOAD_H
#define AUDIO_UPLOAD_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback for ASR (speech-to-text) result
 * @param text   Transcribed text (UTF-8), or NULL on error.
 *               Freed after callback — strdup() if you keep it.
 * @param status ESP_OK on success
 */
typedef void (*asr_callback_t)(const char *text, esp_err_t status);

/**
 * @brief Upload PCM audio to Volcengine ASR endpoint and get transcription
 *
 * Encodes 16-bit mono PCM data as a WAV file, uploads via multipart/form-data
 * to the OpenAI-compatible /v1/audio/transcriptions endpoint.
 * Runs in a background task — `callback` is invoked from task context.
 * Access LVGL via lvgl_port_lock()/unlock() inside the callback.
 *
 * @param pcm          PCM sample buffer (16-bit, 16000 Hz, mono)
 * @param sample_count Number of samples in the buffer
 * @param callback     Function called with the transcribed text
 * @return ESP_OK if the request was queued
 */
esp_err_t audio_upload_start(const int16_t *pcm, int sample_count,
                             asr_callback_t callback);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_UPLOAD_H */
