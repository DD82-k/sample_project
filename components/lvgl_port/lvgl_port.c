#include "lvgl_port.h"
#include "lcd_st7789.h"
#include "touch_cst816t.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "lvgl_port";
static SemaphoreHandle_t lvgl_mux = NULL;

#define LCD_H_RES         240
#define LCD_V_RES         280
#define LVGL_BUF_LINES    20
#define LVGL_BUFFER_SIZE  (LCD_H_RES * LVGL_BUF_LINES * sizeof(lv_color_t))
#define TASK_STACK        8192
#define TASK_PRIO         2
#define TICK_PERIOD_MS    2
#define TASK_MAX_DELAY    500
#define TASK_MIN_DELAY    1

static lv_display_t *lvgl_disp = NULL;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    esp_lcd_panel_handle_t p = lv_display_get_user_data(disp);
    esp_lcd_panel_draw_bitmap(p, area->x1, area->y1, area->x2 + 1, area->y2 + 1, (void *)px);
}

static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x[1] = {0}, y[1] = {0}; uint8_t n = 0;
    void *h = lv_indev_get_user_data(indev);
    SemaphoreHandle_t m = touch_cst816t_get_mux();
    if (!h || !m) { data->state = LV_INDEV_STATE_RELEASED; return; }

    if (xSemaphoreTake(m, 0) == pdTRUE) {
        esp_lcd_touch_read_data(h);
    }

    bool ok = esp_lcd_touch_get_coordinates(h, x, y, NULL, &n, 1);
    if (ok && n > 0) {
        data->point.x = x[0]; data->point.y = y[0]; data->state = LV_INDEV_STATE_PRESSED;
        ESP_LOGI(TAG, "Touch: x=%d y=%d", x[0], y[0]);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void tick_cb(void *arg) { lv_tick_inc(TICK_PERIOD_MS); }

static bool lock(int ms) { return xSemaphoreTake(lvgl_mux, ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(ms)) == pdTRUE; }
static void unlock(void) { xSemaphoreGive(lvgl_mux); }

static void lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task running");
    uint32_t d = TASK_MAX_DELAY;
    while (1) {
        if (lock(-1)) { d = lv_timer_handler(); unlock(); }
        if (d > TASK_MAX_DELAY) d = TASK_MAX_DELAY;
        else if (d < TASK_MIN_DELAY) d = TASK_MIN_DELAY;
        vTaskDelay(pdMS_TO_TICKS(d));
    }
}

void lvgl_port_init(esp_lcd_panel_handle_t lcd, esp_lcd_touch_handle_t touch)
{
    ESP_LOGI(TAG, "Init LVGL");
    lv_init();

    lv_color_t *b1 = heap_caps_malloc(LVGL_BUFFER_SIZE, MALLOC_CAP_DMA);
    lv_color_t *b2 = heap_caps_malloc(LVGL_BUFFER_SIZE, MALLOC_CAP_DMA);
    assert(b1 && b2);

    lvgl_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_flush_cb(lvgl_disp, flush_cb);
    lv_display_set_user_data(lvgl_disp, lcd);
    lv_display_set_buffers(lvgl_disp, b1, b2, LVGL_BUFFER_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(lvgl_disp, LV_COLOR_FORMAT_RGB565);
    lcd_st7789_set_lvgl_display(lvgl_disp);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_cb);
    lv_indev_set_user_data(indev, touch);
    lv_indev_set_display(indev, lvgl_disp);

    esp_timer_handle_t t;
    esp_timer_create_args_t ta = { .callback = tick_cb, .name = "lvgl_tick" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &t));
    ESP_ERROR_CHECK(esp_timer_start_periodic(t, TICK_PERIOD_MS * 1000));
}

void lvgl_port_lock(void)   { xSemaphoreTake(lvgl_mux, portMAX_DELAY); }
void lvgl_port_unlock(void) { xSemaphoreGive(lvgl_mux); }

void lvgl_port_start(void)
{
    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);
    xTaskCreate(lvgl_task, "LVGL", TASK_STACK, NULL, TASK_PRIO, NULL);
}
