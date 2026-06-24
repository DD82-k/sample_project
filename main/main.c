/*
 * P169H002-CTP Smartwatch
 * ST7789 LCD (240x280) + CST816T touch + LVGL v9
 */
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_st7789.h"
#include "touch_cst816t.h"
#include "lvgl_port.h"
#include "ui/ui.h"
#include "wifi_mqtt.h"
#include "doubao.h"
#include "audio_mic.h"
#include "vad.h"
#include "alarm_detect.h"
#include "speech_prep.h"

extern lv_obj_t * uic_LabelContent;
extern lv_obj_t * ui_LabelTime;

/* 报警 UI 标志 (audio task 写入, LVGL task 读取) */
static volatile bool alarm_ui_active = false;
static char saved_text[512] = {0};
static bool alarm_dismissed = false;   /* 双击消除后阻止重触发 */

void Button_Clear(lv_event_t * e)
{
    lv_label_set_text(uic_LabelContent, "");
}

/* ============= 报警覆盖层 ============= */
static lv_obj_t *alarm_overlay = NULL;
static lv_obj_t *alarm_box = NULL;
static lv_obj_t *alarm_label = NULL;
static lv_obj_t *alarm_dismiss_btn = NULL;

/* 双击检测: 记录上次点击时间 */
static uint32_t last_click_tick = 0;

static void alarm_dismiss_click_cb(lv_event_t *e)
{
    uint32_t now = lv_tick_get();
    /* 两次点击间隔 < 600ms 视为双击 */
    if (now - last_click_tick < 600) {
        /* 双击 → 消除报警, 不再重触发 */
        alarm_ui_active = false;
        alarm_dismissed = true;
    }
    last_click_tick = now;
}

static void alarm_overlay_create(void)
{
    lv_obj_t *scr = lv_scr_act();

    /* 全屏覆盖层 (LCD反转: 写0x00FFFF→显示红色) */
    alarm_overlay = lv_obj_create(scr);
    lv_obj_set_size(alarm_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(alarm_overlay, lv_color_hex(0x00FFFF), 0);
    lv_obj_set_style_bg_opa(alarm_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(alarm_overlay, 0, 0);
    lv_obj_set_style_radius(alarm_overlay, 0, 0);
    lv_obj_add_flag(alarm_overlay, LV_OBJ_FLAG_HIDDEN);

    /* 中间提示框 */
    alarm_box = lv_obj_create(alarm_overlay);
    lv_obj_set_size(alarm_box, lv_pct(90), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(alarm_box, 20, 0);
    lv_obj_set_style_bg_color(alarm_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(alarm_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(alarm_box, 0, 0);
    lv_obj_set_style_radius(alarm_box, 12, 0);
    lv_obj_center(alarm_box);

    alarm_label = lv_label_create(alarm_box);
    lv_label_set_text(alarm_label, "检测到警报声！");
    lv_obj_set_style_text_color(alarm_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(alarm_label, &ui_font_font3Alibaba, LV_PART_MAIN);
    lv_obj_center(alarm_label);

    /* 底部双击返回按钮 */
    alarm_dismiss_btn = lv_btn_create(alarm_overlay);
    lv_obj_set_size(alarm_dismiss_btn, lv_pct(80), 40);
    lv_obj_set_style_bg_color(alarm_dismiss_btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(alarm_dismiss_btn, LV_OPA_60, 0);
    lv_obj_set_style_radius(alarm_dismiss_btn, 8, 0);
    lv_obj_align(alarm_dismiss_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_add_event_cb(alarm_dismiss_btn, alarm_dismiss_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(alarm_dismiss_btn);
    lv_label_set_text(btn_label, "双击返回");
    lv_obj_set_style_text_color(btn_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(btn_label, &ui_font_font3Alibaba, LV_PART_MAIN);
    lv_obj_center(btn_label);
}

/* LVGL timer — 每 500ms 检查报警标志并刷新闪烁 */
static void alarm_ui_check_cb(lv_timer_t *timer)
{
    static bool last_state = false;

    if (alarm_ui_active != last_state) {
        last_state = alarm_ui_active;
        if (alarm_ui_active) {
            if (!alarm_overlay) alarm_overlay_create();
            lv_obj_clear_flag(alarm_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(alarm_overlay);
        } else {
            if (alarm_overlay) lv_obj_add_flag(alarm_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(uic_LabelContent, saved_text);
        }
    }

    if (alarm_ui_active && alarm_overlay) {
        static bool flash_state = false;
        flash_state = !flash_state;
        lv_color_t c = flash_state ? lv_color_hex(0x00FFFF) : lv_color_hex(0xFFFFFF);
        lv_obj_set_style_bg_color(alarm_overlay, c, 0);
    }
}

/* ============= Doubao ============= */
/* Forward declaration */
static void doubao_response_cb(const char *response, esp_err_t status);

/* Task: wait for WiFi, then fire Doubao demo request */
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
        strncpy(saved_text, response, sizeof(saved_text) - 1);
    } else {
        lv_label_set_text(uic_LabelContent, "Doubao request failed");
    }
    lvgl_port_unlock();
}

/* ============= 时钟 ============= */
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

/* ============= 报警事件回调 ============= */
static void on_alarm(alarm_type_t type, bool active)
{
    printf("ALARM: %s (type=%d)\n", active ? "ACTIVE" : "CLEAR", type);
    if (active && alarm_dismissed) return;
    if (!active) alarm_dismissed = false;
    alarm_ui_active = active;
}

/* ============= 麦克风采集 + VAD + 报警 ============= */
static void audio_task(void *arg)
{
    int16_t *buf = heap_caps_malloc(320 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) { printf("audio: malloc failed\n"); vTaskDelete(NULL); return; }

    vad_init(2000, 800);
    alarm_detect_init(0, 0);
    alarm_detect_set_callback(on_alarm);
    speech_prep_init(2000.0f, 12.0f);

    while (1) {
        int n = audio_mic_read(buf, 320);
        if (n < 1) continue;

        alarm_detect_feed(buf, n);
        vad_feed(buf, n);

        switch (vad_state()) {
        case VAD_STATE_IDLE:
            speech_prep_feed_idle(buf, n);
            break;

        case VAD_STATE_SPEAKING: {
            printf("VAD: speaking\n");
            int64_t sum_before = 0;
            for (int i = 0; i < n; i++) sum_before += (int32_t)buf[i] * buf[i];
            float rms_before = sqrtf((float)sum_before / n);

            speech_prep_process(buf, n);

            int64_t sum_after = 0;
            for (int i = 0; i < n; i++) sum_after += (int32_t)buf[i] * buf[i];
            float rms_after = sqrtf((float)sum_after / n);
            printf("  prep: RMS %.0f->%.0f noise=%.0f\n",
                   rms_before, rms_after, speech_prep_noise_rms());
            break;
        }

        case VAD_STATE_DONE:
            printf("VAD: done\n");
            speech_prep_reset();
            printf("  -> ready for ASR upload\n");
            if (alarm_detect_type() != ALARM_NONE) {
                printf("  -> alarm also active, overlay shown\n");
            }
            vad_reset();
            break;

        default: break;
        }
    }
}

/* ============= 主函数 ============= */
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
    /* Alarm UI check — 500ms interval */
    lv_timer_create(alarm_ui_check_cb, 500, NULL);

    lvgl_port_start();

    /* Start WiFi + OneNet MQTT in background */
    wifi_mqtt_start();

    /* Doubao demo: wait for WiFi, then send a prompt */
    xTaskCreate(doubao_demo_task, "doubao_demo", 4096, NULL, 3, NULL);

    /* Start microphone audio capture */
    if (audio_mic_init() == ESP_OK) {
        xTaskCreate(audio_task, "audio", 4096, NULL, 3, NULL);
        printf("Audio task started\n");
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
