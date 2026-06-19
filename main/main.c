/*
 * P169H002-CTP Smartwatch
 * ST7789 LCD (240x280) + CST816T touch + LVGL v9
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_st7789.h"
#include "touch_cst816t.h"
#include "lvgl_port.h"
#include "ui/ui.h"

extern lv_obj_t * uic_LabelContent;

void Button_Clear(lv_event_t * e)
{
    lv_label_set_text(uic_LabelContent, "");
}

void app_main(void)
{
    esp_lcd_panel_handle_t lcd   = lcd_st7789_init();
    esp_lcd_touch_handle_t touch = touch_cst816t_init();
    lvgl_port_init(lcd, touch);
    ui_init();
    lvgl_port_start();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
