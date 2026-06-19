#pragma once
#include "lvgl.h"
#include "esp_lcd_panel_ops.h"

esp_lcd_panel_handle_t lcd_st7789_init(void);
void lcd_st7789_set_lvgl_display(lv_display_t *disp);
