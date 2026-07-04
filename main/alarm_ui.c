/* alarm_ui.c — 报警红屏覆盖层, 双击消除, LCD 反转适配 */
#include "alarm_ui.h"
#include "lvgl.h"
#include "ui/ui.h"
#include "alarm_detect.h"
#include "asr_display.h"
#include <stdio.h>

static lv_obj_t *alarm_overlay = NULL;
static lv_obj_t *alarm_box = NULL;
static lv_obj_t *alarm_label = NULL;
static lv_obj_t *alarm_dismiss_btn = NULL;
static volatile bool alarm_ui_active = false;
static bool alarm_dismissed = false;
static uint32_t last_click_tick = 0;

/* ---- 双击消除 ---- */
static void alarm_dismiss_click_cb(lv_event_t *e)
{
    uint32_t now = lv_tick_get();
    if (now - last_click_tick < 600) {
        alarm_ui_active = false;
        alarm_dismissed = true;
    }
    last_click_tick = now;
}

/* ---- 创建覆盖层 ---- */
static void alarm_overlay_create(void)
{
    lv_obj_t *top = lv_layer_top();
    alarm_overlay = lv_obj_create(top);
    lv_obj_set_size(alarm_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(alarm_overlay, lv_color_hex(0x00FFFF), 0);
    lv_obj_set_style_bg_opa(alarm_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(alarm_overlay, 0, 0);
    lv_obj_set_style_radius(alarm_overlay, 0, 0);
    lv_obj_add_flag(alarm_overlay, LV_OBJ_FLAG_HIDDEN);

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

/* ---- LVGL 定时器: 500ms 刷新状态 + 闪烁 ---- */
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
            asr_display_restore_current();
        }
    }
    if (alarm_ui_active && alarm_overlay) {
        static bool flash_state = false;
        flash_state = !flash_state;
        lv_color_t c = flash_state ? lv_color_hex(0x00FFFF) : lv_color_hex(0xFFFFFF);
        lv_obj_set_style_bg_color(alarm_overlay, c, 0);
    }
}

/* ---- 公共 API ---- */
void alarm_ui_init(void)
{
    lv_timer_create(alarm_ui_check_cb, 500, NULL);
}

void alarm_ui_on_alarm(alarm_type_t type, bool active)
{
    printf("ALARM: %s (type=%d)\n", active ? "ACTIVE" : "CLEAR", type);
    if (active && alarm_dismissed) return;  /* 消除后阻止重触发 */
    if (!active) alarm_dismissed = false;    /* 自然停止后重置 */
    alarm_ui_active = active;
}
