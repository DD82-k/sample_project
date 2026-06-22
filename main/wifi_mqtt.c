/*
 * WiFi + OneNet MQTT module
 *
 * Adapted from the tcp project.  Handles:
 *   - NVS init
 *   - WiFi station connection (via protocol_examples_common)
 *   - MQTT connection to OneNet (heclouds.com)
 *   - Device property publish & property-set subscription
 */
#include "wifi_mqtt.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "mqtt_client.h"

static const char *TAG = "wifi_mqtt";

/* ================================================================== */
/*  OneNet configuration                                               */
/* ================================================================== */
#define ONENET_BROKER_URI   "mqtt://mqtts.heclouds.com:1883"
#define ONENET_USERNAME     "90hui97R9G"
#define ONENET_CLIENT_ID    "esp32_s3"
#define ONENET_PASSWORD     "version=2018-10-31&res=products%2F90hui97R9G%2Fdevices%2Fesp32_s3&et=1908337489&method=md5&sign=mV1gUTVH7J%2FzvusHL6v5OA%3D%3D"

/* OneNet MQTT topics */
#define ONENET_TOPIC_PROPERTY_POST   "$sys/90hui97R9G/esp32_s3/thing/property/post"
#define ONENET_TOPIC_PROPERTY_SET    "$sys/90hui97R9G/esp32_s3/thing/property/set"

/* Device property data payload (JSON) */
static const char *DEVICE_PROPERTY_JSON = "{"
    "\"id\": \"123\","
    "\"version\": \"1.0\","
    "\"params\": {"
        "\"device_status\": {"
            "\"value\": 23"
        "}"
    "}"
"}";

static esp_mqtt_client_handle_t s_mqtt_client = NULL;

/* ================================================================== */
/*  MQTT event handler                                                  */
/* ================================================================== */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected to OneNet broker");

            /* Publish device property */
            esp_mqtt_client_publish(s_mqtt_client,
                                    ONENET_TOPIC_PROPERTY_POST,
                                    DEVICE_PROPERTY_JSON, 0, 1, 0);
            ESP_LOGI(TAG, "Published: %s", DEVICE_PROPERTY_JSON);

            /* Subscribe to property set commands */
            esp_mqtt_client_subscribe(s_mqtt_client,
                                      ONENET_TOPIC_PROPERTY_SET, 0);
            ESP_LOGI(TAG, "Subscribed to: %s", ONENET_TOPIC_PROPERTY_SET);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT disconnected from broker");
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Topic: %.*s, Data: %.*s",
                     event->topic_len, event->topic,
                     event->data_len, event->data);
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT subscribed successfully");
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT unsubscribed");
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT message published");
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error occurred");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "Last esp-tls error: 0x%x",
                         event->error_handle->esp_tls_last_esp_err);
            }
            break;

        default:
            ESP_LOGD(TAG, "MQTT event: %" PRId32, event_id);
            break;
    }
}

/* ================================================================== */
/*  Start MQTT client                                                   */
/* ================================================================== */
static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri    = ONENET_BROKER_URI,
        .credentials.username  = ONENET_USERNAME,
        .credentials.client_id = ONENET_CLIENT_ID,
        .credentials.authentication.password = ONENET_PASSWORD,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));
}

/* ================================================================== */
/*  Background task — NVS → WiFi → MQTT                                 */
/* ================================================================== */
static void wifi_mqtt_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting WiFi + MQTT background task");

    /* Initialize NVS (may need erase on version change) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, doing that now");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    /* Connect to WiFi (blocks until connected or timeout) */
    ESP_LOGI(TAG, "Connecting to WiFi...");
    ESP_ERROR_CHECK(example_connect());
    ESP_LOGI(TAG, "WiFi connected successfully");

    /* Start MQTT to OneNet */
    mqtt_app_start();

    ESP_LOGI(TAG, "WiFi + MQTT init complete, task exiting");
    vTaskDelete(NULL);
}

/* ================================================================== */
/*  Public API                                                          */
/* ================================================================== */
esp_err_t wifi_mqtt_start(void)
{
    BaseType_t ret = xTaskCreate(
        wifi_mqtt_task,
        "wifi_mqtt",
        8192,       /* stack size (bytes) */
        NULL,       /* arg */
        5,          /* priority */
        NULL);      /* task handle (not needed) */

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create wifi_mqtt task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WiFi/MQTT background task spawned");
    return ESP_OK;
}
