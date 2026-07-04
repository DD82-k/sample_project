/* alarm_ui.h — 报警红屏覆盖层，独立于业务页面 */
#pragma once
#include "alarm_detect.h"

void alarm_ui_init(void);                          /* 注册 LVGL 定时器 */
void alarm_ui_on_alarm(alarm_type_t type, bool active); /* 报警事件入口 */
