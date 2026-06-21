SET(SOURCES screens/ui_Splash.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    ui_events.c
    fonts/ui_font_font3Alibaba.c
    fonts/ui_font_emoji.c)

add_library(ui ${SOURCES})
