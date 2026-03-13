#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "driver/ledc.h"

static const char *TAG = "LCD_TEST";

// 引脚定义 (根据你的硬件)
#define PIN_NUM_MOSI   11  // SDA
#define PIN_NUM_CLK    12  // SCL
#define PIN_NUM_CS     10
#define PIN_NUM_DC     9
#define PIN_NUM_RST    13  // RES
#define PIN_NUM_BCKL   14  // BLK

// 1.47寸屏幕分辨率常见为 172 x 320 或 240 x 280
#define TEST_LCD_H_RES 172
#define TEST_LCD_V_RES 320

void app_main(void)
{
    // Config LEDC timer
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_0,
        .duty_resolution  = LEDC_TIMER_13_BIT, // Resolution: 0 to 8191
        .freq_hz          = 5000,              // 5 kHz PWM frequency (no flicker)
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Config LEDC channel
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = PIN_NUM_BCKL,
        .duty           = 4095,                // Initial duty cycle (50% brightness)
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_CLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = -1, // 我们不需要从屏幕读取数据
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TEST_LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = 40 * 1000 * 1000, // 40MHz SPI clock
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    // 绑定 SPI 总线
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "Install ST7789 panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_endian = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    
    // 如果屏幕颜色反转（黑色显示白色等），可以取消注释下面这行
     esp_lcd_panel_invert_color(panel_handle, true);
    
    // 如果屏幕显示有偏移，ST7789V3 经常需要设置偏移量，可以后续调整，这里先点亮
    esp_lcd_panel_set_gap(panel_handle, 34, 0); 

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "Draw color to screen");
    ESP_LOGI(TAG, "Start dynamic animation");
    
    // 我们每次刷 40 行。分块刷新可以节省内存，同时依靠 DMA 依然能保持极高的帧率
    #define LINES_PER_FLUSH 40
    size_t buffer_size = TEST_LCD_H_RES * LINES_PER_FLUSH * sizeof(uint16_t);
    
    // 分配一块明确支持 DMA 硬件搬运的内存！这步非常关键。
    uint16_t *color_data = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA);
    if (color_data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate DMA memory!");
        return;
    }

    uint16_t color = 0;
    int duty = 0;
    int fade_step = 150; // Step size for breathing effect

    while (1) {
        // 1. Screen color update (DMA)
        color += 0x0119; 
        for (int i = 0; i < TEST_LCD_H_RES * LINES_PER_FLUSH; i++) {
            color_data[i] = color;
        }
        for (int y = 0; y < TEST_LCD_V_RES; y += LINES_PER_FLUSH) {
            esp_lcd_panel_draw_bitmap(panel_handle, 0, y, TEST_LCD_H_RES, y + LINES_PER_FLUSH, color_data);
        }

        // 2. Backlight breathing logic
        duty += fade_step;
        
        // 13-bit resolution max value is 8191
        if (duty >= 8191) {
            duty = 8191;
            fade_step = -fade_step; // Reverse direction to dim
        } else if (duty <= 0) {
            duty = 0;
            fade_step = -fade_step; // Reverse direction to brighten
        }
        
        // Apply new duty cycle to hardware
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

        // Frame rate / breathing speed control
        vTaskDelay(pdMS_TO_TICKS(100)); 
    }
    
}