#pragma once
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

void lvgl_port_init(esp_lcd_panel_handle_t lcd, esp_lcd_touch_handle_t touch);
void lvgl_port_start(void);

/* Thread-safety: take/release the LVGL mutex from any task */
void lvgl_port_lock(void);
void lvgl_port_unlock(void);
