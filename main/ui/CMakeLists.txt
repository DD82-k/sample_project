SET(SOURCES screens/ui_Splash.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    ui_events.c
    fonts/ui_font_font1.c
    fonts/ui_font_font2Alibaba.c
    fonts/ui_font_font1Alibaba.c
    fonts/ui_font_font3Alibaba.c)

add_library(ui ${SOURCES})
