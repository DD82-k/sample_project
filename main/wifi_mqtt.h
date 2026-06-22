#ifndef WIFI_MQTT_H
#define WIFI_MQTT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start WiFi connection and OneNet MQTT in a background task
 *
 * Spawns a task that initializes NVS, connects to WiFi via
 * example_connect(), then starts the MQTT client to OneNet broker.
 * Returns immediately — WiFi/MQTT runs asynchronously.
 *
 * @return ESP_OK on success, ESP_FAIL if task creation failed
 */
esp_err_t wifi_mqtt_start(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MQTT_H */
