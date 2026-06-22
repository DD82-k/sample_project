#ifndef DOUBAO_H
#define DOUBAO_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback for Doubao API response
 * @param response  The response text (UTF-8), or NULL on error.
 *                  Freed after callback returns — strdup() if you keep it.
 * @param status    ESP_OK on success
 */
typedef void (*doubao_callback_t)(const char *response, esp_err_t status);

/**
 * @brief Send a text prompt to Doubao API asynchronously
 *
 * Spawns a background task that makes an HTTPS POST to the Doubao
 * (Volcengine) chat-completions endpoint.  On completion, `callback`
 * is invoked from the HTTP task context — access LVGL via
 * lvgl_port_lock()/lvgl_port_unlock() inside the callback.
 *
 * @param prompt   Null-terminated UTF-8 prompt text
 * @param callback Function called with the response
 * @return ESP_OK if the request was queued
 */
esp_err_t doubao_request(const char *prompt, doubao_callback_t callback);

#ifdef __cplusplus
}
#endif

#endif /* DOUBAO_H */
