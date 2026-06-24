/*
 * Doubao (Volcengine) chat-completions API client
 *
 * Uses ESP HTTP client (HTTPS) to talk to the OpenAI-compatible
 * endpoint at ai-gateway.vei.volces.com.
 *
 * Credentials are baked in at compile time (see defines below).
 */
#include "doubao.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "doubao";

/* ================================================================== */
/*  Doubao / Volcengine configuration                                   */
/* ================================================================== */
#define DOUBAO_BASE_URL     "https://ai-gateway.vei.volces.com/v1"
#define DOUBAO_API_KEY      "sk-e03e012d80c349938be158b7acb8f75csoeh2gwnh4bpp1bd"
#define DOUBAO_MODEL        "doubao-seed-1.6-thinking"
#define DOUBAO_MAX_TOKENS   500

/* Response buffer (heap).  Adjust if your prompts yield longer replies. */
#define RESPONSE_BUF_SIZE   8192
#define TASK_STACK_SIZE     8192

/* ================================================================== */
/*  HTTP event handler — accumulate body, parse JSON on finish          */
/* ================================================================== */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    /*
     * We stash a dynamic buffer and the doubao_callback_t inside user_data.
     * user_data layout:
     *   bytes 0..sizeof(void*)-1  : callback (doubao_callback_t)
     *   bytes sizeof(void*)..end  : response buffer pointer
     * But for simplicity we use a small wrapper struct passed as user_data.
     */
    struct http_ctx {
        doubao_callback_t callback;
        char            *buf;
        size_t           len;
        size_t           cap;
    } *ctx = (struct http_ctx *)evt->user_data;

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
            ESP_LOGD(TAG, "Raw response: %s", ctx->buf);

            /* Parse JSON */
            cJSON *root = cJSON_Parse(ctx->buf);
            if (root) {
                cJSON *choices = cJSON_GetObjectItem(root, "choices");
                if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
                    cJSON *first  = cJSON_GetArrayItem(choices, 0);
                    cJSON *msg    = cJSON_GetObjectItem(first, "message");
                    cJSON *ct     = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
                    if (cJSON_IsString(ct)) {
                        ESP_LOGI(TAG, "Response: %s", ct->valuestring);
                        if (ctx->callback) {
                            ctx->callback(ct->valuestring, ESP_OK);
                        }
                        cJSON_Delete(root);
                        goto cleanup;
                    }
                }
                cJSON_Delete(root);
            }
            ESP_LOGE(TAG, "Failed to parse JSON response");
            if (ctx->callback) ctx->callback(NULL, ESP_FAIL);
        }
    cleanup:
        free(ctx->buf);
        ctx->buf = NULL;
        ctx->len = 0;
        break;

    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "HTTP connection error");
        if (ctx->buf) {
            free(ctx->buf);
            ctx->buf = NULL;
            ctx->len = 0;
        }
        if (ctx->callback) ctx->callback(NULL, ESP_ERR_HTTP_CONNECT);
        break;

    default:
        break;
    }
    return ESP_OK;
}

/* ================================================================== */
/*  Background task — builds JSON, fires HTTPS POST, feeds event handler */
/* ================================================================== */
typedef struct {
    char             *prompt;
    doubao_callback_t callback;
} request_t;

static void doubao_task(void *arg)
{
    request_t *req = (request_t *)arg;
    if (!req) { vTaskDelete(NULL); return; }

    /* ---- Build JSON body ---- */
    cJSON *root   = cJSON_CreateObject();
    cJSON *msgs   = cJSON_AddArrayToObject(root, "messages");
    cJSON *msg    = cJSON_CreateObject();
    cJSON *cntarr = cJSON_AddArrayToObject(msg, "content");
    cJSON *txt    = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "model",      DOUBAO_MODEL);
    cJSON_AddNumberToObject(root, "max_tokens", DOUBAO_MAX_TOKENS);
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(txt, "type", "text");
    cJSON_AddStringToObject(txt, "text", req->prompt);
    cJSON_AddItemToArray(cntarr, txt);
    cJSON_AddItemToArray(msgs, msg);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!body) {
        ESP_LOGE(TAG, "Failed to build JSON body");
        if (req->callback) req->callback(NULL, ESP_FAIL);
        free(req->prompt); free(req);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Sending %zu bytes to Doubao...", strlen(body));

    /* ---- HTTPS POST ---- */
    char url[128];
    snprintf(url, sizeof(url), "%s/chat/completions", DOUBAO_BASE_URL);

    /* Context for the event handler — lives on the task stack */
    struct http_ctx {
        doubao_callback_t callback;
        char             *buf;
        size_t            len;
        size_t            cap;
    } ctx = {
        .callback = req->callback,
        .buf      = NULL,
        .len      = 0,
        .cap      = 0,
    };

    esp_http_client_config_t cfg = {
        .url                         = url,
        .method                      = HTTP_METHOD_POST,
        .event_handler               = http_event_handler,
        .user_data                   = &ctx,
        .timeout_ms                  = 60000,
        .buffer_size                 = RESPONSE_BUF_SIZE,
        .skip_cert_common_name_check = false,
        .crt_bundle_attach           = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        if (req->callback) req->callback(NULL, ESP_FAIL);
        free(body); free(req->prompt); free(req);
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");

    char auth[256];
    snprintf(auth, sizeof(auth), "Bearer %s", DOUBAO_API_KEY);
    esp_http_client_set_header(client, "Authorization", auth);

    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        /* The event handler already called the callback with error,
         * but if perform never called ON_FINISH, we need to do it here. */
        if (ctx.buf == NULL && err != ESP_ERR_HTTP_CONNECT) {
            /* No ON_FINISH was triggered (e.g. connection timeout) */
            if (req->callback) req->callback(NULL, err);
        }
    }

    esp_http_client_cleanup(client);
    free(body);
    free(req->prompt);
    free(req);
    vTaskDelete(NULL);
}

/* ================================================================== */
/*  Public API                                                          */
/* ================================================================== */
esp_err_t doubao_request(const char *prompt, doubao_callback_t callback)
{
    if (!prompt || !callback) return ESP_ERR_INVALID_ARG;

    request_t *req = calloc(1, sizeof(request_t));
    if (!req) return ESP_ERR_NO_MEM;

    req->prompt   = strdup(prompt);
    if (!req->prompt) { free(req); return ESP_ERR_NO_MEM; }
    req->callback = callback;

    BaseType_t ret = xTaskCreate(doubao_task, "doubao",
                                 TASK_STACK_SIZE, req, 5, NULL);
    if (ret != pdPASS) {
        free(req->prompt);
        free(req);
        return ESP_FAIL;
    }
    return ESP_OK;
}
