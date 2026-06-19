#pragma once
#include "esp_lcd_touch.h"
#include "freertos/semphr.h"

esp_lcd_touch_handle_t touch_cst816t_init(void);
SemaphoreHandle_t touch_cst816t_get_mux(void);

/* Deep sleep: write 0x03 to 0xE5. I2C stops, only RST can wake. */
void touch_cst816t_sleep(void);

/* Wake from sleep: RST pulse (10ms low + 50ms high) + write 0xFE=0x01 */
void touch_cst816t_wakeup(void);
