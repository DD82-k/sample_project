/*
 * ASR audio upload module
 *
 * Buffers PCM audio, encodes as WAV, uploads to Volcengine AI Gateway
 * (OpenAI-compatible /v1/audio/transcriptions), and returns the
 * transcribed text via callback.
 */
#include "audio_upload.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "audio_up";

/* ================================================================== */
/*  Volcengine ASR configuration                                       */
/* ================================================================== */
#define ASR_BASE_URL    "https://ai-gateway.vei.volces.com/v1"
#define ASR_API_KEY     "sk-e03e012d80c349938be158b7acb8f75csoeh2gwnh4bpp1bd"
#define ASR_MODEL       "doubao-seed-1.6"

#define RESPONSE_BUF_SIZE   4096
#define TASK_STACK_SIZE     8192

/* ================================================================== */
/*  WAV header (44 bytes for 16-bit mono PCM)                          */
/* ================================================================== */
#pragma pack(push, 1)
typedef struct {
    char     riff[4];        /* "RIFF" */
    uint32_t file_size;      /* total - 8 */
    char     wave[4];        /* "WAVE" */
    char     fmt[4];         /* "fmt " */
    uint32_t fmt_len;        /* 16 */
    uint16_t audio_fmt;      /* 1 = PCM */
    uint16_t channels;       /* 1 */
    uint32_t sample_rate;    /* 16000 */
    uint32_t byte_rate;      /* sample_rate * channels * bits_per_sample/8 */
    uint16_t block_align;    /* channels * bits_per_sample/8 */
    uint16_t bits_per_sample;/* 16 */
    char     data[4];        /* "data" */
    uint32_t data_size;      /* total PCM bytes */
} wav_header_t;
#pragma pack(pop)

static void wav_header_fill(wav_header_t *hdr, int sample_count)
{
    uint32_t data_bytes = sample_count * sizeof(int16_t);
    memset(hdr, 0, sizeof(*hdr));
    memcpy(hdr->riff, "RIFF", 4);
    hdr->file_size    = data_bytes + 36;
    memcpy(hdr->wave, "WAVE", 4);
    memcpy(hdr->fmt, "fmt ", 4);
    hdr->fmt_len       = 16;
    hdr->audio_fmt     = 1;
    hdr->channels      = 1;
    hdr->sample_rate   = 16000;
    hdr->byte_rate     = 16000 * 2;
    hdr->block_align   = 2;
    hdr->bits_per_sample = 16;
    memcpy(hdr->data, "data", 4);
    hdr->data_size     = data_bytes;
}

/* ================================================================== */
/*  HTTP event handler                                                 */
/* ================================================================== */
typedef struct {
    asr_callback_t callback;
    char          *buf;
    size_t         len;
    size_t         cap;
    esp_err_t      http_err;
} upload_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    upload_ctx_t *ctx = (upload_ctx_t *)evt->user_data;
    if (!ctx) return ESP_FAIL;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (!ctx->buf) {
            ctx->cap = RESPONSE_BUF_SIZE;
            ctx->buf = calloc(1, ctx->cap);
            if (!ctx->buf) return ESP_ERR_NO_MEM;
        }
        if (ctx->len + evt->data_len < ctx->cap) {
            memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
            ctx->len += evt->data_len;
        }
        break;

    case HTTP_EVENT_ON_FINISH:
        if (ctx->buf) {
            ctx->buf[ctx->len] = '\0';
            ESP_LOGD(TAG, "ASR response: %s", ctx->buf);

            cJSON *root = cJSON_Parse(ctx->buf);
            if (root) {
                cJSON *text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "ASR result: %s", text->valuestring);
                    if (ctx->callback)
                        ctx->callback(text->valuestring, ESP_OK);
                    cJSON_Delete(root);
                    goto cleanup;
                }
                cJSON_Delete(root);
            }
            ESP_LOGE(TAG, "Failed to parse ASR response");
            if (ctx->callback) ctx->callback(NULL, ESP_FAIL);
        }
    cleanup:
        free(ctx->buf); ctx->buf = NULL; ctx->len = 0;
        break;

    case HTTP_EVENT_ERROR:
        if (ctx->buf) { free(ctx->buf); ctx->buf = NULL; ctx->len = 0; }
        if (ctx->callback) ctx->callback(NULL, ESP_ERR_HTTP_CONNECT);
        break;

    default:
        break;
    }
    return ESP_OK;
}

/* ================================================================== */
/*  Background task: encode WAV → multipart upload → parse response    */
/* ================================================================== */
typedef struct {
    int16_t        *pcm;
    int             sample_count;
    asr_callback_t  callback;
} upload_req_t;

static void upload_task(void *arg)
{
    upload_req_t *req = (upload_req_t *)arg;
    if (!req) { vTaskDelete(NULL); return; }

    int sample_count = req->sample_count;
    int pcm_bytes = sample_count * sizeof(int16_t);
    int wav_size  = sizeof(wav_header_t) + pcm_bytes;

    /* ---- Build WAV in heap ---- */
    uint8_t *wav = heap_caps_malloc(wav_size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!wav) {
        ESP_LOGE(TAG, "WAV malloc failed (%d bytes)", wav_size);
        if (req->callback) req->callback(NULL, ESP_ERR_NO_MEM);
        free(req->pcm); free(req);
        vTaskDelete(NULL);
        return;
    }

    wav_header_t *hdr = (wav_header_t *)wav;
    wav_header_fill(hdr, sample_count);
    memcpy(wav + sizeof(wav_header_t), req->pcm, pcm_bytes);
    free(req->pcm);

    ESP_LOGI(TAG, "Uploading %d bytes WAV (%d samples, %.1f sec)",
             wav_size, sample_count, (float)sample_count / 16000.0f);

    /* ---- Build multipart form-data body ---- */
    const char *boundary = "----ESP32ASRBoundary";
    char url[128];
    snprintf(url, sizeof(url), "%s/audio/transcriptions", ASR_BASE_URL);

    /* Calculate total body size */
    int head_len = snprintf(NULL, 0,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        boundary, ASR_MODEL, boundary);
    char tail_fmt[256];
    int tail_len = snprintf(tail_fmt, sizeof(tail_fmt),
        "\r\n--%s\r\n"
        "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n"
        "json\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
        "zh\r\n"
        "--%s--\r\n",
        boundary, boundary, boundary);

    int total_len = head_len + wav_size + tail_len;

    /* ---- HTTP POST (multipart/form-data) ---- */
    upload_ctx_t ctx = {
        .callback = req->callback,
        .buf = NULL, .len = 0, .cap = 0, .http_err = ESP_OK,
    };

    char content_type[128];
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=%s", boundary);

    esp_http_client_config_t cfg = {
        .url                         = url,
        .method                      = HTTP_METHOD_POST,
        .event_handler               = http_event_handler,
        .user_data                   = &ctx,
        .timeout_ms                  = 60000,
        .buffer_size                 = 2048,
        .crt_bundle_attach           = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        if (req->callback) req->callback(NULL, ESP_FAIL);
        free(wav); free(req);
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_set_header(client, "Content-Type", content_type);
    char auth[256];
    snprintf(auth, sizeof(auth), "Bearer %s", ASR_API_KEY);
    esp_http_client_set_header(client, "Authorization", auth);

    /* We'll use set_post_field to set the raw body, but the WAV data
       is binary so we need to construct the full body in memory. */
    uint8_t *body = heap_caps_malloc(total_len, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!body) {
        ESP_LOGE(TAG, "Body malloc failed (%d bytes)", total_len);
        esp_http_client_cleanup(client);
        if (req->callback) req->callback(NULL, ESP_ERR_NO_MEM);
        free(wav); free(req);
        vTaskDelete(NULL);
        return;
    }

    int off = 0;
    /* Write head */
    off += snprintf((char *)body + off, total_len - off,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        boundary, ASR_MODEL, boundary);
    /* Write WAV binary */
    memcpy(body + off, wav, wav_size);
    off += wav_size;
    /* Write tail */
    off += snprintf((char *)body + off, total_len - off,
        "\r\n--%s\r\n"
        "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n"
        "json\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
        "zh\r\n"
        "--%s--\r\n",
        boundary, boundary, boundary);

    esp_http_client_set_post_field(client, (char *)body, off);

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP failed: %s (status=%d)",
                 esp_err_to_name(err),
                 esp_http_client_get_status_code(client));
        if (ctx.buf == NULL) {
            if (req->callback) req->callback(NULL, err);
        }
    }

    esp_http_client_cleanup(client);
    free(body);
    free(wav);
    free(req);
    vTaskDelete(NULL);
}

/* ================================================================== */
/*  Public API                                                          */
/* ================================================================== */
esp_err_t audio_upload_start(const int16_t *pcm, int sample_count,
                             asr_callback_t callback)
{
    if (!pcm || sample_count < 160 || !callback) {
        return ESP_ERR_INVALID_ARG;
    }

    upload_req_t *req = calloc(1, sizeof(upload_req_t));
    if (!req) return ESP_ERR_NO_MEM;

    req->pcm = heap_caps_malloc(sample_count * sizeof(int16_t),
                                MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!req->pcm) { free(req); return ESP_ERR_NO_MEM; }

    memcpy(req->pcm, pcm, sample_count * sizeof(int16_t));
    req->sample_count = sample_count;
    req->callback     = callback;

    BaseType_t ret = xTaskCreate(upload_task, "asr_upload",
                                 TASK_STACK_SIZE, req, 5, NULL);
    if (ret != pdPASS) {
        free(req->pcm); free(req);
        return ESP_FAIL;
    }
    return ESP_OK;
}
