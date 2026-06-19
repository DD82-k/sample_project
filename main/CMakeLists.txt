file(GLOB_RECURSE SRC_UI ${CMAKE_SOURCE_DIR} "ui/*.c")

idf_component_register(SRCS "main.c" ${SRC_UI}
                    INCLUDE_DIRS "." "ui"
                    REQUIRES lvgl lcd_st7789 touch_cst816t lvgl_port)
