/*
 * P169H002-CTP Smartwatch
 * ST7789 LCD (240x280) + CST816T touch + LVGL v9
 */
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_st7789.h"
#include "touch_cst816t.h"
#include "lvgl_port.h"
#include "ui/ui.h"
#include "wifi_mqtt.h"
#include "doubao.h"

extern lv_obj_t * uic_LabelContent;
extern lv_obj_t * ui_LabelTime;

void Button_Clear(lv_event_t * e)
{
    lv_label_set_text(uic_LabelContent, "");
}

/* Task: wait for WiFi, then fire the Doubao demo request */
static void doubao_demo_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(15000));
    doubao_request("你好，请用中文简单介绍一下你自己", doubao_response_cb);
    vTaskDelete(NULL);
}

/* Doubao response callback — updates the scrollable text label */
static void doubao_response_cb(const char *response, esp_err_t status)
{
    lvgl_port_lock();
    if (status == ESP_OK && response) {
        lv_label_set_text(uic_LabelContent, response);
    } else {
        lv_label_set_text(uic_LabelContent, "Doubao request failed");
    }
    lvgl_port_unlock();
}

/* LVGL timer callback — update time display every second */
static void rtc_update_cb(lv_timer_t *timer)
{
    time_t now;
    struct tm timeinfo;
    char buf[16];

    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
    lv_label_set_text(ui_LabelTime, buf);
}

void app_main(void)
{
    /* Set RTC to compile time on first boot (if not already set) */
    time_t now;
    time(&now);
    if (now < 1700000000) {  /* before 2023 = RTC never set */
        struct tm tm = {0};
        tm.tm_year = 2026 - 1900;
        tm.tm_mon  = 6 - 1;
        tm.tm_mday = 21;
        tm.tm_hour = 12;
        tm.tm_min  = 0;
        tm.tm_sec  = 0;
        struct timeval tv = { .tv_sec = mktime(&tm), .tv_usec = 0 };
        settimeofday(&tv, NULL);
    }

    printf("Start\n");

    esp_lcd_panel_handle_t lcd   = lcd_st7789_init();
    esp_lcd_touch_handle_t touch = touch_cst816t_init();
    lvgl_port_init(lcd, touch);
    ui_init();

    /* Start RTC display update — runs every 1000ms inside lv_timer_handler */
    lv_timer_create(rtc_update_cb, 1000, NULL);

    lvgl_port_start();

    /* Start WiFi + OneNet MQTT in background (non-blocking) */
    wifi_mqtt_start();

    /* Doubao demo: wait for WiFi (+ short settle), then send a prompt */
    xTaskCreate(doubao_demo_task, "doubao_demo", 4096, NULL, 3, NULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
