#include "lcd_st7789.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7789.h"
#include "esp_log.h"

static const char *TAG = "lcd_st7789";
static lv_display_t *lvgl_disp = NULL;

/* Pin config — P169H002-CTP module */
#define LCD_HOST         SPI2_HOST
#define PIN_DC           GPIO_NUM_9
#define PIN_CS           GPIO_NUM_10
#define PIN_SCLK         GPIO_NUM_11
#define PIN_MOSI         GPIO_NUM_12
#define PIN_RST          GPIO_NUM_13
#define LCD_H_RES        240
#define LCD_V_RES        280
#define LCD_BITS         16

static bool flush_ready_cb(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    if (lvgl_disp) lv_display_flush_ready(lvgl_disp);
    return false;
}

esp_lcd_panel_handle_t lcd_st7789_init(void)
{
    ESP_LOGI(TAG, "Init SPI bus");
    spi_bus_config_t buscfg = ST7789_PANEL_BUS_SPI_CONFIG(PIN_SCLK, PIN_MOSI,
                                LCD_H_RES * LCD_V_RES * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Init panel IO");
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t iocfg = ST7789_PANEL_IO_SPI_CONFIG(PIN_CS, PIN_DC, flush_ready_cb, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &iocfg, &io));

    ESP_LOGI(TAG, "Init ST7789 driver");
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &pcfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    return panel;
}

void lcd_st7789_set_lvgl_display(lv_display_t *disp) { lvgl_disp = disp; }
