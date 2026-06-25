/*
 * ASR audio — Volcengine WebSocket Speech Recognition
 *
 * Implements minimal WebSocket client + Volcengine binary protocol
 * using ESP-TLS and raw LWIP sockets.
 */
#include "audio_upload.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "audio_up";

/* ================================================================== */
/*  Credentials                                                        */
/* ================================================================== */
#define ASR_HOST        "openspeech.bytedance.com"
#define ASR_PORT        443
#define ASR_PATH        "/api/v1/asr"
#define VOLC_APP_ID     "1072685805"
#define VOLC_TOKEN      "vdoSwqj15Jrh33OVaOMmUIH3U7ffjVFw"
#define VOLC_CLUSTER    "volc_asr_v1"

#define TASK_STACK      16384
#define BUF_SIZE        8192

/* ================================================================== */
/*  WebSocket frame helpers                                            */
/* ================================================================== */

/* Encode a WebSocket frame. Returns bytes written.
 * We always mask (client → server must be masked). */
static int ws_encode(uint8_t *dst, int opcode, const uint8_t *data, int len, bool fin)
{
    int n = 0;
    dst[n++] = (fin ? 0x80 : 0x00) | (opcode & 0x0F);
    if (len < 126) {
        dst[n++] = 0x80 | len;  /* mask bit set */
    } else if (len < 65536) {
        dst[n++] = 0x80 | 126;
        dst[n++] = (len >> 8) & 0xFF;
        dst[n++] = len & 0xFF;
    } else {
        dst[n++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) dst[n++] = (len >> (i * 8)) & 0xFF;
    }
    /* Mask key (4 random bytes — we use a fixed key for simplicity) */
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    memcpy(dst + n, mask, 4); n += 4;
    /* Masked payload */
    for (int i = 0; i < len; i++) dst[n + i] = data[i] ^ mask[i % 4];
    return n + len;
}

/* Decode a WebSocket frame (server → client, unmasked).
 * Returns payload length, or negative on error.
 * *out_opcode receives the opcode. */
static int ws_decode(const uint8_t *src, int src_len, int *out_opcode)
{
    if (src_len < 2) return -1;
    *out_opcode = src[0] & 0x0F;
    int masked = (src[1] & 0x80) ? 1 : 0;
    int len = src[1] & 0x7F;
    int off = 2;
    if (len == 126) { if (src_len < 4) return -1; len = (src[2] << 8) | src[3]; off = 4; }
    else if (len == 127) { if (src_len < 10) return -1; len = 0; for (int i=0;i<8;i++) len = (len<<8)|src[2+i]; off = 10; }
    if (masked) off += 4;
    if (src_len < off + len) return -1;
    return len;  /* payload starts at src[off] */
}

/* ================================================================== */
/*  Volcengine binary protocol helpers                                  */
/* ================================================================== */
static int volc_frame(uint8_t *buf, int msg_type, int flags,
                      const uint8_t *payload, int plen)
{
    buf[0] = 0x11;
    buf[1] = (msg_type << 4) | flags;
    buf[2] = 0x10;  /* JSON serialization, no compression */
    buf[3] = 0x00;
    buf[4] = (plen >> 24) & 0xFF;
    buf[5] = (plen >> 16) & 0xFF;
    buf[6] = (plen >> 8) & 0xFF;
    buf[7] = plen & 0xFF;
    if (payload && plen > 0) memcpy(buf + 8, payload, plen);
    return 8 + plen;
}

/* ================================================================== */
/*  Background task                                                    */
/* ================================================================== */
typedef struct {
    int16_t        *pcm;
    int             sample_count;
    asr_callback_t  callback;
} upload_req_t;

static void upload_task(void *arg)
{
    upload_req_t *req = (upload_req_t *)arg;
    if (!req) vTaskDelete(NULL);

    float dur = (float)req->sample_count / 16000.0f;
    ESP_LOGI(TAG, "Starting for %.1f sec audio", dur);

    /* ---- Build JSON request ---- */
    cJSON *root   = cJSON_CreateObject();
    cJSON *app    = cJSON_AddObjectToObject(root, "app");
    cJSON *user   = cJSON_AddObjectToObject(root, "user");
    cJSON *audio  = cJSON_AddObjectToObject(root, "audio");
    cJSON *reqj   = cJSON_AddObjectToObject(root, "request");
    cJSON_AddStringToObject(app, "appid",   VOLC_APP_ID);
    cJSON_AddStringToObject(app, "token",   VOLC_TOKEN);
    cJSON_AddStringToObject(app, "cluster", VOLC_CLUSTER);
    cJSON_AddStringToObject(user, "uid", "esp32_s3");
    cJSON_AddStringToObject(audio, "format", "pcm");
    cJSON_AddNumberToObject(audio, "rate", 16000);
    cJSON_AddNumberToObject(audio, "bits", 16);
    cJSON_AddNumberToObject(audio, "channel", 1);
    cJSON_AddStringToObject(reqj, "model_name", "bigmodel");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) { ESP_LOGE(TAG, "JSON OOM"); goto fail; }
    ESP_LOGI(TAG, "JSON (%d): %s", strlen(json), json);

    /* ---- Build Volcengine binary frames (inner payloads) ---- */
    int json_len = strlen(json);
    uint8_t *volc_req_bin = malloc(8 + json_len);
    int volc_req_len = volc_frame(volc_req_bin, 1, 0, (uint8_t *)json, json_len);
    free(json);

    int pcm_bytes = req->sample_count * sizeof(int16_t);
    uint8_t *volc_aud_bin = malloc(8 + pcm_bytes);
    int volc_aud_len = volc_frame(volc_aud_bin, 2, 2, (uint8_t *)req->pcm, pcm_bytes);
    free(req->pcm);

    /* ---- WebSocket upgrade HTTP request ---- */
    char *ws_key = "dGhlIHNhbXBsZSBub25jZQ==";  /* RFC example base64 */
    char upgrade[1024];
    int up_len = snprintf(upgrade, sizeof(upgrade),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Authorization: Bearer; %s\r\n"
        "\r\n",
        ASR_PATH, ASR_HOST, ws_key, VOLC_TOKEN);

    /* ---- ESP-TLS connection ---- */
    esp_tls_t *tls = esp_tls_init();
    if (!tls) { ESP_LOGE(TAG, "tls init fail"); goto fail; }

    esp_tls_cfg_t tls_cfg = { .crt_bundle_attach = esp_crt_bundle_attach };
    int ret = esp_tls_conn_new_sync(ASR_HOST, strlen(ASR_HOST), ASR_PORT, &tls_cfg, tls);
    if (ret != 1) { ESP_LOGE(TAG, "TLS connect fail"); goto tls_fail; }
    ESP_LOGI(TAG, "TLS connected");

    /* Send HTTP upgrade request */
    esp_tls_conn_write(tls, upgrade, up_len);
    ESP_LOGI(TAG, "Upgrade sent");

    /* Read HTTP response */
    char resp[1024] = {0};
    int nr = esp_tls_conn_read(tls, resp, sizeof(resp) - 1);
    if (nr <= 0) { ESP_LOGE(TAG, "No response"); goto tls_fail; }
    resp[nr] = 0;
    ESP_LOGI(TAG, "HTTP: %.*s", nr > 200 ? 200 : nr, resp);

    /* Check for 101 Switching Protocols */
    if (!strstr(resp, "101")) {
        ESP_LOGE(TAG, "WebSocket upgrade rejected");
        goto tls_fail;
    }
    ESP_LOGI(TAG, "WebSocket upgraded");

    /* ---- Send Volcengine frames over WebSocket ---- */
    /* Frame 1: Full client request (binary WebSocket frame) */
    {
        uint8_t ws_frame[4096];
        int ws_len = ws_encode(ws_frame, 2, volc_req_bin, volc_req_len, true);
        esp_tls_conn_write(tls, ws_frame, ws_len);
        ESP_LOGI(TAG, "Full request sent (%d bytes)", ws_len);
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    /* Frame 2: Audio data (binary WebSocket frame) */
    {
        uint8_t ws_frame[65536];
        int ws_len = ws_encode(ws_frame, 2, volc_aud_bin, volc_aud_len, true);
        esp_tls_conn_write(tls, ws_frame, ws_len);
        ESP_LOGI(TAG, "Audio sent (%d bytes)", ws_len);
    }

    /* ---- Read response ---- */
    char *result = NULL;
    uint8_t raw[8192];
    int total_read = 0;
    int timeout_ms = 30000;
    bool got_result = false;

    while (timeout_ms > 0 && !got_result) {
        nr = esp_tls_conn_read(tls, (char *)raw + total_read, sizeof(raw) - total_read - 1);
        if (nr > 0) {
            total_read += nr;
            raw[total_read] = 0;

            /* Try to extract JSON result from the WebSocket + Volcengine frames */
            char *json_start = strstr((char *)raw, "{\"result\"");
            if (!json_start) json_start = strstr((char *)raw, "\"result\"");
            if (json_start) {
                /* Find matching closing brace */
                int depth = 0;
                char *p = json_start;
                while (*p) {
                    if (*p == '{') depth++;
                    else if (*p == '}') { depth--; if (depth == 0) break; }
                    p++;
                }
                if (depth == 0 && p > json_start) {
                    int jlen = p - json_start + 1;
                    char *jstr = strndup(json_start, jlen);
                    ESP_LOGI(TAG, "JSON: %s", jstr);
                    cJSON *r = cJSON_Parse(jstr);
                    if (r) {
                        cJSON *res = cJSON_GetObjectItem(r, "result");
                        if (res) {
                            cJSON *text = cJSON_GetObjectItem(res, "text");
                            if (cJSON_IsString(text) && text->valuestring[0])
                                result = strdup(text->valuestring);
                        }
                        cJSON_Delete(r);
                    }
                    free(jstr);
                    if (result) { got_result = true; break; }
                }
            }
        } else if (nr < 0) {
            break;
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
            timeout_ms -= 50;
        }
    }

    if (result) {
        ESP_LOGI(TAG, "Result: %s", result);
        if (req->callback) req->callback(result, ESP_OK);
        free(result);
    } else {
        ESP_LOGE(TAG, "No result, raw=%s", (char *)raw);
        if (req->callback) req->callback(NULL, ESP_FAIL);
    }

tls_fail:
    esp_tls_conn_destroy(tls);
    free(volc_req_bin);
    free(volc_aud_bin);
    free(req);
    vTaskDelete(NULL);
    return;

fail:
    if (req->callback) req->callback(NULL, ESP_FAIL);
    free(req->pcm);
    free(req);
    vTaskDelete(NULL);
}

esp_err_t audio_upload_start(const int16_t *pcm, int sample_count,
                             asr_callback_t callback)
{
    if (!pcm || sample_count < 160 || !callback) return ESP_ERR_INVALID_ARG;
    upload_req_t *req = calloc(1, sizeof(upload_req_t));
    if (!req) return ESP_ERR_NO_MEM;
    req->pcm = malloc(sample_count * sizeof(int16_t));
    if (!req->pcm) { free(req); return ESP_ERR_NO_MEM; }
    memcpy(req->pcm, pcm, sample_count * sizeof(int16_t));
    req->sample_count = sample_count;
    req->callback = callback;
    BaseType_t r = xTaskCreate(upload_task, "asr_ws", TASK_STACK, req, 5, NULL);
    if (r != pdPASS) { free(req->pcm); free(req); return ESP_FAIL; }
    return ESP_OK;
}
