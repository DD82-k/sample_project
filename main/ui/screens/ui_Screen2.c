// Screen2 — quick phrase panel, swipe right to go back
#include "ui_Screen2.h"
#include "../ui.h"

lv_obj_t *ui_Screen2 = NULL;

static lv_obj_t *card_container;

static void screen2_gesture_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_RIGHT) {
        lv_scr_load_anim(ui_Splash, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
    }
}

void ui_Screen2_screen_init(void)
{
    /* ---- Screen ---- */
    ui_Screen2 = lv_obj_create(NULL);
    lv_obj_add_event_cb(ui_Screen2, screen2_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_set_style_bg_color(ui_Screen2, lv_color_hex(0xF2F3F5), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Screen2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scroll_dir(ui_Screen2, LV_DIR_VER);

    /* ---- Top bar ---- */
    lv_obj_t *top_bar = lv_obj_create(ui_Screen2);
    lv_obj_set_size(top_bar, lv_pct(100), 36);
    lv_obj_set_style_bg_color(top_bar, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(top_bar, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(top_bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(top_bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(top_bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(top_bar, lv_color_hex(0xE0E2E6), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(top_bar, 1, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *title = lv_label_create(top_bar);
    lv_label_set_text(title, "快捷短语");
    lv_obj_set_style_text_color(title, lv_color_hex(0x1A1A1A), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title, &ui_font_font3Alibaba, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(title);

    /* ---- Scrollable card list ---- */
    card_container = lv_obj_create(ui_Screen2);
    lv_obj_set_width(card_container, lv_pct(95));
    lv_obj_set_height(card_container, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(card_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(card_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(card_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(card_container, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(card_container, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(card_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_align(card_container, LV_ALIGN_TOP_MID);
    lv_obj_set_y(card_container, 44);

    /* ---- Buttons ---- */
    const char *labels[] = { "你好", "真的吗", "我需要帮助", "有什么问题吗" };
    lv_color_t bg_colors[] = {
        lv_color_hex(0xFFFFFF),
        lv_color_hex(0xFFFFFF),
        lv_color_hex(0xFFFFFF),
        lv_color_hex(0xFFFFFF),
    };
    lv_color_t accent_colors[] = {
        lv_color_hex(0x4A90D9),
        lv_color_hex(0xE8A838),
        lv_color_hex(0x5BAB6F),
        lv_color_hex(0xD95A5A),
    };

    for (int i = 0; i < 4; i++) {
        /* Card wrapper */
        lv_obj_t *card = lv_obj_create(card_container);
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, 56);
        lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(card, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(card, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_hor(card, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_margin_bottom(card, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
        // 发送功能暂时禁用

        /* Accent dot */
        lv_obj_t *dot = lv_obj_create(card);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_bg_color(dot, accent_colors[i], LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(dot, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(dot, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);

        /* Label */
        lv_obj_t *label = lv_label_create(card);
        lv_label_set_text(label, labels[i]);
        lv_obj_set_style_text_color(label, lv_color_hex(0x1A1A1A), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(label, &ui_font_font3Alibaba, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 18, 0);
    }
}

void ui_Screen2_screen_destroy(void)
{
    if (ui_Screen2) lv_obj_del(ui_Screen2);
    ui_Screen2 = NULL;
    card_container = NULL;
}
