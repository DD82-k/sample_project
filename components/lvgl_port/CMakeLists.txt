idf_component_register(SRCS "lvgl_port.c"
                    INCLUDE_DIRS "."
                    REQUIRES lvgl lcd_st7789 touch_cst816t esp_timer)
