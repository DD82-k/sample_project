/*
 * WiFi + MQTT quick phrase sender.
 *
 * The sender connects to the configured phone hotspot WiFi and publishes
 * Chinese quick phrases to the receiver ESP32 through broker.emqx.io.
 */
#include "wifi_mqtt.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"

static const char *TAG = "wifi_mqtt";

#define MQTT_BROKER_URI      "mqtt://broker.emqx.io:1883"
#define MQTT_CLIENT_ID       "esp32_s3_quick_key_sender"
#define MQTT_PUBLISH_TOPIC   "esp32/sign_speech/text"
#define MQTT_PUBLISH_QOS     1
#define MQTT_PUBLISH_RETAIN  0

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static volatile bool s_mqtt_connected = false;
static volatile bool s_started = false;

static void configure_network_for_realtime(void)
{
    ip_addr_t dns0;
    ip_addr_t dns1;

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));
    IP_ADDR4(&dns0, 223, 5, 5, 5);
    IP_ADDR4(&dns1, 114, 114, 114, 114);
    dns_setserver(0, &dns0);
    dns_setserver(1, &dns1);
    ESP_LOGI(TAG, "WiFi connected, power save off, DNS set");
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        configure_network_for_realtime();
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT connected: %s", MQTT_BROKER_URI);
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected, reconnecting automatically");
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT published, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_ERROR:
        s_mqtt_connected = false;
        ESP_LOGE(TAG, "MQTT error");
        if (event->error_handle &&
            event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "transport error: esp_tls=0x%x sock_errno=%d",
                     event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_transport_sock_errno);
        }
        break;

    default:
        ESP_LOGD(TAG, "MQTT event: %" PRId32, event_id);
        break;
    }
}

static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.client_id = MQTT_CLIENT_ID,
        .network.reconnect_timeout_ms = 5000,
        .session.keepalive = 30,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));
}

static void wifi_mqtt_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting WiFi + MQTT quick phrase sender");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, doing that now");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               ip_event_handler, NULL));

    ESP_LOGI(TAG, "Connecting to phone hotspot WiFi...");
    ESP_ERROR_CHECK(example_connect());
    configure_network_for_realtime();

    mqtt_app_start();

    ESP_LOGI(TAG, "WiFi/MQTT sender is ready");
    vTaskDelete(NULL);
}

esp_err_t wifi_mqtt_start(void)
{
    if (s_started) {
        ESP_LOGW(TAG, "WiFi/MQTT already started");
        return ESP_OK;
    }
    s_started = true;

    BaseType_t ret = xTaskCreate(
        wifi_mqtt_task,
        "wifi_mqtt",
        6144,
        NULL,
        5,
        NULL);

    if (ret != pdPASS) {
        s_started = false;
        ESP_LOGE(TAG, "Failed to create wifi_mqtt task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool wifi_mqtt_is_connected(void)
{
    return s_mqtt_connected;
}

esp_err_t wifi_mqtt_publish_text(const char *text)
{
    if (!text || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_mqtt_client) {
        ESP_LOGW(TAG, "MQTT not started, cannot send: %s", text);
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client,
                                         MQTT_PUBLISH_TOPIC,
                                         text,
                                         0,
                                         MQTT_PUBLISH_QOS,
                                         MQTT_PUBLISH_RETAIN);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Publish failed: %s", text);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Send text: %s", text);
    return ESP_OK;
}
