/*
 * Volcengine BigModel ASR bidirectional streaming WebSocket client.
 *
 * Endpoint:
 *   wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async
 *
 * The microphone task feeds 16 kHz/16-bit/mono PCM frames in real time.
 * This module owns the TLS/WebSocket session and forwards interim/final
 * recognition text through asr_callback_t.
 */
#include "audio_upload.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/ssl.h"
#include "lwip/dns.h"

static const char *TAG = "asr_ws";

#define ASR_HOST              "openspeech.bytedance.com"
#define ASR_PORT              443
#define ASR_PATH              "/api/v3/sauc/bigmodel_async"
#define ASR_RESOURCE_ID       "volc.bigasr.sauc.duration"

/*
 * For the v3 SAUC endpoint, the keys are sent as X-Api-* headers.
 * The Secret Key provided by the user is used as X-Api-App-Key.
 */
#define VOLC_APP_ID           "4578134049"
#define VOLC_ACCESS_TOKEN     "HiOrKIdnMD5nqovZpGFqiyW_yU-cpA1X"
#define VOLC_SECRET_KEY       "wyExExewgoCRROaCbNkcBaPMkfN_oPSQ"

#define AUDIO_RATE_HZ         16000
#define AUDIO_BITS            16
#define AUDIO_CHANNELS        1
#define AUDIO_FRAME_SAMPLES   320
#define AUDIO_QUEUE_DEPTH     240
#define AUDIO_CONNECT_BACKLOG_KEEP 120
#define ASR_TASK_STACK        (24 * 1024)
#define WS_RX_MAX             8192

#define PROTO_VER             0x01
#define HEADER_SIZE_WORDS     0x01
#define SERIAL_NONE           0x00
#define SERIAL_JSON           0x01
#define COMPRESS_NONE         0x00

#define MSG_FULL_CLIENT_REQ   0x01
#define MSG_AUDIO_ONLY_REQ    0x02
#define MSG_FULL_SERVER_RESP  0x09
#define MSG_SERVER_ACK        0x0b
#define MSG_ERROR_RESP        0x0f

#define FLAG_NO_SEQ           0x00
#define FLAG_POS_SEQ          0x01
#define FLAG_LAST_NEG_SEQ     0x03

typedef struct {
    int samples;
    bool final;
    int16_t pcm[AUDIO_FRAME_SAMPLES];
} audio_msg_t;

typedef struct {
    QueueHandle_t queue;
    TaskHandle_t task;
    asr_callback_t cb;
    volatile bool finishing;
    volatile bool closed;
    int32_t seq;
} asr_stream_t;

static asr_stream_t *s_stream;
static SemaphoreHandle_t s_lock;

static void put_be32(uint8_t *p, int32_t v)
{
    uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)(u >> 24);
    p[1] = (uint8_t)(u >> 16);
    p[2] = (uint8_t)(u >> 8);
    p[3] = (uint8_t)u;
}

static int32_t get_be32(const uint8_t *p)
{
    uint32_t u = ((uint32_t)p[0] << 24) |
                 ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8) |
                 (uint32_t)p[3];
    return (int32_t)u;
}

static bool is_want_io(ssize_t ret)
{
    return ret == MBEDTLS_ERR_SSL_WANT_READ ||
           ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
           ret == -EAGAIN ||
           ret == -EWOULDBLOCK;
}

static void asr_configure_dns(void)
{
    ip_addr_t dns0;
    ip_addr_t dns1;

    IP_ADDR4(&dns0, 223, 5, 5, 5);
    IP_ADDR4(&dns1, 114, 114, 114, 114);
    dns_setserver(0, &dns0);
    dns_setserver(1, &dns1);
    ESP_LOGI(TAG, "DNS set: 223.5.5.5, 114.114.114.114");
}

static esp_err_t write_all(esp_tls_t *tls, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t written = esp_tls_conn_write(tls, data + off, len - off);
        if (written > 0) {
            off += written;
            vTaskDelay(1);
            continue;
        }
        if (is_want_io(written)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        ESP_LOGE(TAG, "tls write failed: %d", (int)written);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t read_exact(esp_tls_t *tls, uint8_t *buf, size_t len, int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t off = 0;

    while (off < len) {
        ssize_t got = esp_tls_conn_read(tls, buf + off, len - off);
        if (got > 0) {
            off += got;
            continue;
        }
        if (got == 0) {
            return ESP_FAIL;
        }
        if (is_want_io(got)) {
            if (esp_timer_get_time() >= deadline) {
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        ESP_LOGE(TAG, "tls read failed: %d", (int)got);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static char *base64_dup(const uint8_t *src, size_t src_len)
{
    size_t olen = 0;
    size_t cap = ((src_len + 2) / 3) * 4 + 1;
    char *out = calloc(1, cap);
    if (!out) {
        return NULL;
    }
    if (mbedtls_base64_encode((unsigned char *)out, cap, &olen, src, src_len) != 0) {
        free(out);
        return NULL;
    }
    out[olen] = '\0';
    return out;
}

static char *make_ws_key(void)
{
    uint8_t raw[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = esp_random();
        memcpy(raw + i, &r, 4);
    }
    return base64_dup(raw, sizeof(raw));
}

static esp_err_t ws_send_frame(esp_tls_t *tls, uint8_t opcode,
                               const uint8_t *payload, size_t payload_len)
{
    size_t hdr_len = 2;
    if (payload_len >= 126 && payload_len <= 0xffff) {
        hdr_len += 2;
    } else if (payload_len > 0xffff) {
        hdr_len += 8;
    }
    hdr_len += 4; /* client masking key */

    uint8_t *frame = heap_caps_malloc(hdr_len + payload_len, MALLOC_CAP_SPIRAM);
    if (!frame) {
        frame = malloc(hdr_len + payload_len);
    }
    if (!frame) {
        return ESP_ERR_NO_MEM;
    }

    size_t p = 0;
    frame[p++] = 0x80 | (opcode & 0x0f);
    if (payload_len < 126) {
        frame[p++] = 0x80 | (uint8_t)payload_len;
    } else if (payload_len <= 0xffff) {
        frame[p++] = 0x80 | 126;
        frame[p++] = (uint8_t)(payload_len >> 8);
        frame[p++] = (uint8_t)payload_len;
    } else {
        frame[p++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) {
            frame[p++] = (uint8_t)(payload_len >> (i * 8));
        }
    }

    uint8_t mask[4];
    uint32_t r = esp_random();
    memcpy(mask, &r, sizeof(mask));
    memcpy(frame + p, mask, sizeof(mask));
    p += sizeof(mask);

    for (size_t i = 0; i < payload_len; i++) {
        frame[p + i] = payload[i] ^ mask[i & 3];
    }

    esp_err_t err = write_all(tls, frame, hdr_len + payload_len);
    free(frame);
    return err;
}

static esp_err_t ws_recv_frame(esp_tls_t *tls, uint8_t *opcode,
                               uint8_t **payload, size_t *payload_len,
                               int timeout_ms)
{
    uint8_t hdr[2];
    esp_err_t err = read_exact(tls, hdr, sizeof(hdr), timeout_ms);
    if (err != ESP_OK) {
        return err;
    }

    *opcode = hdr[0] & 0x0f;
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t len = hdr[1] & 0x7f;

    if (len == 126) {
        uint8_t ext[2];
        ESP_RETURN_ON_ERROR(read_exact(tls, ext, sizeof(ext), timeout_ms), TAG, "ws ext16");
        len = ((uint16_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        ESP_RETURN_ON_ERROR(read_exact(tls, ext, sizeof(ext), timeout_ms), TAG, "ws ext64");
        len = 0;
        for (int i = 0; i < 8; i++) {
            len = (len << 8) | ext[i];
        }
    }

    if (len > WS_RX_MAX) {
        ESP_LOGE(TAG, "ws frame too large: %u", (unsigned)len);
        return ESP_ERR_NO_MEM;
    }

    uint8_t mask[4] = {0};
    if (masked) {
        ESP_RETURN_ON_ERROR(read_exact(tls, mask, sizeof(mask), timeout_ms), TAG, "ws mask");
    }

    uint8_t *buf = calloc(1, (size_t)len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    if (len > 0) {
        err = read_exact(tls, buf, (size_t)len, timeout_ms);
        if (err != ESP_OK) {
            free(buf);
            return err;
        }
    }

    if (masked) {
        for (size_t i = 0; i < (size_t)len; i++) {
            buf[i] ^= mask[i & 3];
        }
    }

    *payload = buf;
    *payload_len = (size_t)len;
    return ESP_OK;
}

static esp_err_t ws_connect(esp_tls_t **out_tls)
{
    asr_configure_dns();
    ESP_LOGI(TAG, "connecting to %s:%d", ASR_HOST, ASR_PORT);

    char *key = make_ws_key();
    if (!key) {
        return ESP_ERR_NO_MEM;
    }

    char connect_id[40];
    snprintf(connect_id, sizeof(connect_id), "esp32s3-%08lx", (unsigned long)esp_random());

    char req[1536];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "X-Api-App-Key: %s\r\n"
        "X-Api-Access-Key: %s\r\n"
        "X-Api-Secret-Key: %s\r\n"
        "X-Api-App-Id: %s\r\n"
        "X-Api-Resource-Id: %s\r\n"
        "X-Api-Connect-Id: %s\r\n"
        "Authorization: Bearer; %s\r\n"
        "\r\n",
        ASR_PATH, ASR_HOST, key,
        VOLC_SECRET_KEY,
        VOLC_ACCESS_TOKEN,
        VOLC_SECRET_KEY,
        VOLC_APP_ID,
        ASR_RESOURCE_ID,
        connect_id,
        VOLC_ACCESS_TOKEN);
    free(key);

    if (req_len <= 0 || req_len >= (int)sizeof(req)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        return ESP_ERR_NO_MEM;
    }

    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 4000,
        .non_block = true,
    };

    int ret = esp_tls_conn_new_sync(ASR_HOST, strlen(ASR_HOST), ASR_PORT, &cfg, tls);
    if (ret != 1) {
        ESP_LOGE(TAG, "TLS connect failed: %d", ret);
        esp_tls_conn_destroy(tls);
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(write_all(tls, (const uint8_t *)req, req_len), TAG, "ws upgrade write");

    char resp[768] = {0};
    size_t used = 0;
    while (used + 1 < sizeof(resp) && !strstr(resp, "\r\n\r\n")) {
        ssize_t got = esp_tls_conn_read(tls, resp + used, sizeof(resp) - used - 1);
        if (got > 0) {
            used += got;
            resp[used] = '\0';
            continue;
        }
        if (is_want_io(got)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        ESP_LOGE(TAG, "upgrade read failed: %d", (int)got);
        esp_tls_conn_destroy(tls);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "upgrade response: %.120s", resp);
    if (!strstr(resp, " 101 ")) {
        ESP_LOGE(TAG, "WebSocket upgrade rejected");
        esp_tls_conn_destroy(tls);
        return ESP_FAIL;
    }

    *out_tls = tls;
    return ESP_OK;
}

static size_t make_proto_frame(uint8_t *dst, uint8_t msg_type, uint8_t flags,
                               uint8_t serialization, int32_t seq,
                               const uint8_t *payload, size_t payload_len)
{
    size_t p = 0;
    dst[p++] = (PROTO_VER << 4) | HEADER_SIZE_WORDS;
    dst[p++] = (msg_type << 4) | flags;
    dst[p++] = (serialization << 4) | COMPRESS_NONE;
    dst[p++] = 0x00;

    if (flags == FLAG_POS_SEQ || flags == FLAG_LAST_NEG_SEQ) {
        put_be32(dst + p, seq);
        p += 4;
    }

    put_be32(dst + p, (int32_t)payload_len);
    p += 4;

    if (payload_len > 0 && payload) {
        memcpy(dst + p, payload, payload_len);
        p += payload_len;
    }
    return p;
}

static char *build_start_request(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *app = cJSON_AddObjectToObject(root, "app");
    cJSON *user = cJSON_AddObjectToObject(root, "user");
    cJSON *audio = cJSON_AddObjectToObject(root, "audio");
    cJSON *request = cJSON_AddObjectToObject(root, "request");
    if (!root || !app || !user || !audio || !request) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON_AddStringToObject(app, "appid", VOLC_APP_ID);
    cJSON_AddStringToObject(app, "token", VOLC_ACCESS_TOKEN);
    cJSON_AddStringToObject(app, "cluster", "volc_asr_v1");
    cJSON_AddStringToObject(user, "uid", "esp32_s3");
    cJSON_AddStringToObject(audio, "format", "pcm");
    cJSON_AddStringToObject(audio, "codec", "raw");
    cJSON_AddNumberToObject(audio, "rate", AUDIO_RATE_HZ);
    cJSON_AddNumberToObject(audio, "bits", AUDIO_BITS);
    cJSON_AddNumberToObject(audio, "channel", AUDIO_CHANNELS);
    cJSON_AddStringToObject(request, "model_name", "bigmodel");
    cJSON_AddStringToObject(request, "workflow", "audio_in,resample,partition,vad,fe,decode,itn,nlu_punctuate");
    cJSON_AddStringToObject(request, "result_type", "full");
    cJSON_AddBoolToObject(request, "enable_punc", true);
    cJSON_AddBoolToObject(request, "enable_itn", true);
    cJSON_AddBoolToObject(request, "show_utterances", true);
    cJSON_AddBoolToObject(request, "enable_emotion_detection", true);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static esp_err_t send_start_request(esp_tls_t *tls)
{
    char *json = build_start_request();
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    size_t json_len = strlen(json);
    uint8_t *proto = malloc(12 + json_len);
    if (!proto) {
        free(json);
        return ESP_ERR_NO_MEM;
    }

    size_t proto_len = make_proto_frame(proto, MSG_FULL_CLIENT_REQ, FLAG_NO_SEQ,
                                        SERIAL_JSON, 0,
                                        (const uint8_t *)json, json_len);
    ESP_LOGD(TAG, "start request: %s", json);
    free(json);

    esp_err_t err = ws_send_frame(tls, 0x02, proto, proto_len);
    free(proto);
    return err;
}

static esp_err_t send_audio_request(esp_tls_t *tls, int32_t seq,
                                    const int16_t *pcm, int samples, bool final)
{
    size_t audio_len = samples > 0 ? (size_t)samples * sizeof(int16_t) : 0;
    size_t cap = 12 + audio_len;
    uint8_t *proto = malloc(cap);
    if (!proto) {
        return ESP_ERR_NO_MEM;
    }

    int32_t frame_seq = final ? -seq : seq;
    size_t proto_len = make_proto_frame(proto, MSG_AUDIO_ONLY_REQ,
                                        final ? FLAG_LAST_NEG_SEQ : FLAG_POS_SEQ,
                                        SERIAL_NONE, frame_seq,
                                        (const uint8_t *)pcm, audio_len);
    esp_err_t err = ws_send_frame(tls, 0x02, proto, proto_len);
    free(proto);
    return err;
}

static const char *json_get_text(cJSON *root)
{
    cJSON *text = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(text) && text->valuestring[0]) {
        return text->valuestring;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *result_text = result ? cJSON_GetObjectItem(result, "text") : NULL;
    if (cJSON_IsString(result_text) && result_text->valuestring[0]) {
        return result_text->valuestring;
    }

    cJSON *utterances = result ? cJSON_GetObjectItem(result, "utterances") : NULL;
    if (cJSON_IsArray(utterances) && cJSON_GetArraySize(utterances) > 0) {
        cJSON *last = cJSON_GetArrayItem(utterances, cJSON_GetArraySize(utterances) - 1);
        cJSON *utt_text = last ? cJSON_GetObjectItem(last, "text") : NULL;
        if (cJSON_IsString(utt_text) && utt_text->valuestring[0]) {
            return utt_text->valuestring;
        }
    }

    return NULL;
}

static const char *json_get_emotion(cJSON *root)
{
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *utterances = result ? cJSON_GetObjectItem(result, "utterances") : NULL;
    if (cJSON_IsArray(utterances) && cJSON_GetArraySize(utterances) > 0) {
        cJSON *last = cJSON_GetArrayItem(utterances, cJSON_GetArraySize(utterances) - 1);
        cJSON *additions = last ? cJSON_GetObjectItem(last, "additions") : NULL;
        cJSON *emotion = additions ? cJSON_GetObjectItem(additions, "emotion") : NULL;
        if (cJSON_IsString(emotion) && emotion->valuestring[0]) {
            return emotion->valuestring;
        }
    }

    cJSON *additions = result ? cJSON_GetObjectItem(result, "additions") : NULL;
    cJSON *emotion = additions ? cJSON_GetObjectItem(additions, "emotion") : NULL;
    if (cJSON_IsString(emotion) && emotion->valuestring[0]) {
        return emotion->valuestring;
    }

    return NULL;
}

static bool handle_proto_payload(asr_stream_t *s, const uint8_t *data, size_t len)
{
    if (len < 8) {
        return true;
    }

    uint8_t header_bytes = (data[0] & 0x0f) * 4;
    uint8_t msg_type = data[1] >> 4;
    uint8_t flags = data[1] & 0x0f;
    uint8_t serialization = data[2] >> 4;
    size_t p = header_bytes;

    if (p > len) {
        return true;
    }

    if (msg_type == MSG_ERROR_RESP) {
        if ((flags == FLAG_POS_SEQ || flags == FLAG_LAST_NEG_SEQ) && p + 4 <= len) {
            int32_t err_seq = get_be32(data + p);
            p += 4;
            ESP_LOGE(TAG, "server error seq=%ld", (long)err_seq);
        }

        if (p + 8 <= len) {
            int32_t code = get_be32(data + p);
            int32_t msg_size = get_be32(data + p + 4);
            p += 8;
            if (msg_size > 0 && p + (size_t)msg_size <= len) {
                char *msg = calloc(1, (size_t)msg_size + 1);
                if (msg) {
                    memcpy(msg, data + p, (size_t)msg_size);
                    ESP_LOGE(TAG, "server error code=%ld msg=%s", (long)code, msg);
                    free(msg);
                } else {
                    ESP_LOGE(TAG, "server error code=%ld msg_len=%ld", (long)code, (long)msg_size);
                }
            } else {
                ESP_LOGE(TAG, "server error code=%ld msg_len=%ld frame_len=%u",
                         (long)code, (long)msg_size, (unsigned)len);
            }
        } else {
            ESP_LOGE(TAG, "server error frame len=%u", (unsigned)len);
        }
        return false;
    }

    if ((flags == FLAG_POS_SEQ || flags == FLAG_LAST_NEG_SEQ) && p + 4 <= len) {
        int32_t ack_seq = get_be32(data + p);
        p += 4;
        ESP_LOGD(TAG, "server seq=%ld", (long)ack_seq);
    }

    if (p + 4 > len) {
        return true;
    }
    int32_t payload_size = get_be32(data + p);
    p += 4;
    if (payload_size <= 0 || p + (size_t)payload_size > len) {
        return true;
    }

    const uint8_t *payload = data + p;
    if (serialization != SERIAL_JSON) {
        ESP_LOGD(TAG, "non-json server payload type=%u len=%ld", serialization, (long)payload_size);
        return true;
    }

    char *json = calloc(1, (size_t)payload_size + 1);
    if (!json) {
        return true;
    }
    memcpy(json, payload, (size_t)payload_size);
    cJSON *root = cJSON_Parse(json);
    if (root) {
        const char *text = json_get_text(root);
        const char *emotion = json_get_emotion(root);
        if (text && s->cb) {
            if (emotion) {
                ESP_LOGI(TAG, "text: %s emotion=%s", text, emotion);
            } else {
                ESP_LOGI(TAG, "text: %s", text);
            }
            s->cb(text, emotion, ESP_OK);
        }
        cJSON_Delete(root);
    }
    free(json);
    return true;
}

static bool drain_server_frames(asr_stream_t *s, esp_tls_t *tls, int timeout_ms)
{
    while (1) {
        uint8_t opcode = 0;
        uint8_t *payload = NULL;
        size_t payload_len = 0;
        esp_err_t err = ws_recv_frame(tls, &opcode, &payload, &payload_len, timeout_ms);
        timeout_ms = 0;
        if (err == ESP_ERR_TIMEOUT) {
            return true;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "recv frame failed: %s", esp_err_to_name(err));
            return false;
        }

        if (opcode == 0x02) {
            bool ok = handle_proto_payload(s, payload, payload_len);
            free(payload);
            if (!ok) {
                return false;
            }
        } else if (opcode == 0x08) {
            ESP_LOGI(TAG, "server closed websocket");
            free(payload);
            return false;
        } else if (opcode == 0x09) {
            ws_send_frame(tls, 0x0a, payload, payload_len);
            free(payload);
        } else {
            free(payload);
        }
    }
}

static void asr_stream_task(void *arg)
{
    asr_stream_t *s = (asr_stream_t *)arg;
    esp_tls_t *tls = NULL;
    esp_err_t err = ws_connect(&tls);
    if (err == ESP_OK) {
        err = send_start_request(tls);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ASR stream start failed: %s", esp_err_to_name(err));
        if (s->cb) {
            s->cb(NULL, NULL, err);
        }
        goto done;
    }

    ESP_LOGI(TAG, "ASR stream connected");
    s->seq = 1; /* The start request is counted as sequence 1 by the v1 protocol. */

    int queued = uxQueueMessagesWaiting(s->queue);
    if (queued > AUDIO_CONNECT_BACKLOG_KEEP) {
        int drop = queued - AUDIO_CONNECT_BACKLOG_KEEP;
        audio_msg_t old_msg;
        for (int i = 0; i < drop; i++) {
            if (xQueueReceive(s->queue, &old_msg, 0) != pdTRUE) {
                break;
            }
            if (old_msg.final) {
                xQueueSendToFront(s->queue, &old_msg, 0);
                break;
            }
        }
        ESP_LOGW(TAG, "dropped %d stale audio frames after slow connect", drop);
    }

    audio_msg_t msg;
    bool sent_final = false;
    while (!sent_final) {
        if (xQueueReceive(s->queue, &msg, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (msg.final) {
                s->seq++;
                err = send_audio_request(tls, s->seq, NULL, 0, true);
                sent_final = true;
            } else if (msg.samples > 0) {
                s->seq++;
                err = send_audio_request(tls, s->seq, msg.pcm, msg.samples, false);
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "send audio failed: %s", esp_err_to_name(err));
                if (s->cb) {
                    s->cb(NULL, NULL, err);
                }
                break;
            }
        }
        if (!drain_server_frames(s, tls, 1)) {
            break;
        }
    }

    if (err == ESP_OK && sent_final) {
        int wait_ms = 3000;
        while (wait_ms > 0) {
            if (!drain_server_frames(s, tls, 50)) {
                break;
            }
            wait_ms -= 50;
        }
    }

    ws_send_frame(tls, 0x08, NULL, 0);

done:
    if (tls) {
        esp_tls_conn_destroy(tls);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_stream == s) {
        s_stream = NULL;
    }
    xSemaphoreGive(s_lock);

    if (s->queue) {
        vQueueDelete(s->queue);
    }
    s->closed = true;
    free(s);
    vTaskDelete(NULL);
}

static esp_err_t ensure_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t audio_stream_start(asr_callback_t cb)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ensure_lock(), TAG, "lock");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_stream) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    asr_stream_t *s = calloc(1, sizeof(asr_stream_t));
    if (!s) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    s->cb = cb;
    s->queue = xQueueCreateWithCaps(AUDIO_QUEUE_DEPTH, sizeof(audio_msg_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->queue) {
        ESP_LOGE(TAG, "queue create failed, internal=%u psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        free(s);
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }

    s_stream = s;
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(asr_stream_task, "asr_ws", ASR_TASK_STACK,
                                                    s, 5, &s->task, 1,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed, internal=%u psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        s_stream = NULL;
        vQueueDelete(s->queue);
        free(s);
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t audio_stream_feed(const int16_t *pcm, int sample_count)
{
    if (!pcm || sample_count <= 0 || sample_count > AUDIO_FRAME_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(ensure_lock(), TAG, "lock");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    asr_stream_t *s = s_stream;
    xSemaphoreGive(s_lock);

    if (!s || s->finishing) {
        return ESP_ERR_INVALID_STATE;
    }

    audio_msg_t msg = {
        .samples = sample_count,
        .final = false,
    };
    memcpy(msg.pcm, pcm, sample_count * sizeof(int16_t));

    if (xQueueSend(s->queue, &msg, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t audio_stream_finish(void)
{
    ESP_RETURN_ON_ERROR(ensure_lock(), TAG, "lock");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    asr_stream_t *s = s_stream;
    if (s) {
        s->finishing = true;
    }
    xSemaphoreGive(s_lock);

    if (!s) {
        return ESP_ERR_INVALID_STATE;
    }

    audio_msg_t msg = {
        .samples = 0,
        .final = true,
    };
    if (xQueueSend(s->queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool audio_stream_active(void)
{
    if (ensure_lock() != ESP_OK) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = s_stream != NULL;
    xSemaphoreGive(s_lock);
    return active;
}

bool audio_stream_accepting(void)
{
    if (ensure_lock() != ESP_OK) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    asr_stream_t *s = s_stream;
    bool accepting = s && !s->finishing;
    xSemaphoreGive(s_lock);
    return accepting;
}

esp_err_t audio_upload_start(const int16_t *pcm, int sample_count, asr_callback_t cb)
{
    if (!pcm || sample_count < 160 || !cb) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = audio_stream_start(cb);
    if (err != ESP_OK) {
        return err;
    }

    int offset = 0;
    while (offset < sample_count) {
        int n = sample_count - offset;
        if (n > AUDIO_FRAME_SAMPLES) {
            n = AUDIO_FRAME_SAMPLES;
        }
        err = audio_stream_feed(pcm + offset, n);
        if (err != ESP_OK) {
            audio_stream_finish();
            return err;
        }
        offset += n;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return audio_stream_finish();
}
