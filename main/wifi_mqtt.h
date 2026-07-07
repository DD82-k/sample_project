#ifndef WIFI_MQTT_H
#define WIFI_MQTT_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_mqtt_start(void);
bool wifi_mqtt_is_connected(void);
esp_err_t wifi_mqtt_publish_text(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_MQTT_H */
