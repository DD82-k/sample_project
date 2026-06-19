#include "touch_cst816t.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"

static const char *TAG = "touch_cst816t";
static SemaphoreHandle_t touch_mux = NULL;

/* Pin config — P169H002-CTP module */
#define I2C_PORT         I2C_NUM_0
#define PIN_SCL          GPIO_NUM_4
#define PIN_SDA          GPIO_NUM_5
#define PIN_RST          GPIO_NUM_6
#define PIN_INT          GPIO_NUM_7
#define TOUCH_X_MAX      240
#define TOUCH_Y_MAX      280

static void IRAM_ATTR touch_isr(esp_lcd_touch_handle_t tp)
{
    BaseType_t higher = pdFALSE;
    xSemaphoreGiveFromISR(touch_mux, &higher);
    if (higher) portYIELD_FROM_ISR();
}

esp_lcd_touch_handle_t touch_cst816t_init(void)
{
    touch_mux = xSemaphoreCreateBinary();
    assert(touch_mux);

    ESP_LOGI(TAG, "Init I2C bus");
    i2c_config_t i2c = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = PIN_SDA,
        .scl_io_num       = PIN_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    i2c_param_config(I2C_PORT, &i2c);
    i2c_driver_install(I2C_PORT, i2c.mode, 0, 0, 0);

    ESP_LOGI(TAG, "Init CST816T touch");
    esp_lcd_panel_io_i2c_config_t iocfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    esp_lcd_touch_config_t tcfg = {
        .x_max  = TOUCH_X_MAX,
        .y_max  = TOUCH_Y_MAX,
        .rst_gpio_num = PIN_RST,
        .int_gpio_num = PIN_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags  = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
        .interrupt_callback = touch_isr,
    };
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_touch_handle_t    tp    = NULL;
    esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)I2C_PORT, &iocfg, &tp_io);
    esp_lcd_touch_new_i2c_cst816s(tp_io, &tcfg, &tp);

    return tp;
}

SemaphoreHandle_t touch_cst816t_get_mux(void) { return touch_mux; }

/* Raw I2C write helper */
static void i2c_write(uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t c = i2c_cmd_link_create();
    i2c_master_start(c);
    i2c_master_write_byte(c, (0x15 << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(c, reg, true);
    i2c_master_write_byte(c, val, true);
    i2c_master_stop(c);
    i2c_master_cmd_begin(I2C_PORT, c, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(c);
}

/* Deep sleep: write 0x03 to 0xE5. I2C stops, only RST can wake. */
void touch_cst816t_sleep(void)
{
    ESP_LOGI(TAG, "Enter deep sleep");
    i2c_write(0xE5, 0x03);
}

/* Wake from sleep: RST pulse + disable auto-sleep */
void touch_cst816t_wakeup(void)
{
    ESP_LOGI(TAG, "Wake up");
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    i2c_write(0xFE, 0x01);
    vTaskDelay(pdMS_TO_TICKS(50));
}
