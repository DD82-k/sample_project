file(GLOB_RECURSE SRC_UI "ui/*.c")

idf_component_register(SRCS "main.c" "wifi_mqtt.c" ${SRC_UI}
                    "ui/fonts/ui_font_emoji.c"
                    INCLUDE_DIRS "." "ui"
                    REQUIRES lvgl lcd_st7789 touch_cst816t lvgl_port
                    PRIV_REQUIRES mqtt protocol_examples_common)
