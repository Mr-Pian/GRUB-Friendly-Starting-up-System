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
#include "esp_timer.h" 
#include "time.h"
#include "sys/time.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_smartconfig.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "lwip/inet.h"



// 必须引入 LVGL 核心头文件
#include "lvgl.h" 
#include "LVGL_UI.h"

static const char *TAG = "LCD_TEST";
// 加上 volatile 防止被编译器优化，专门给后台任务和 UI 桥接用
volatile bool g_wifi_connected = false;
volatile bool g_is_provisioning = false; // 【新增】标记是否正在配网


#define PIN_NUM_MOSI   11  
#define PIN_NUM_CLK    12  
#define PIN_NUM_CS     10
#define PIN_NUM_DC     9
#define PIN_NUM_RST    13  
#define PIN_NUM_BCKL   14

// 定义波轮开关的引脚
#define PIN_ENC_RIGHT  1  // 向右拨
#define PIN_ENC_PRESS  2  // 向下按
#define PIN_ENC_LEFT   3  // 向左拨 

// 注意：我们将物理分辨率定义放这里，但在 LVGL 中我们要把它“横过来”
#define TEST_LCD_PHYS_H_RES 172
#define TEST_LCD_PHYS_V_RES 320

// =========================================================
// Wi-Fi 与配网 (SmartConfig) 事件回调
// =========================================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect(); 
        ESP_LOGI(TAG, "Wi-Fi Started, attempting auto-connect...");
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_connected = false;
        if (!g_is_provisioning) {
            esp_wifi_connect();
            ESP_LOGI(TAG, "Disconnected, retrying...");
        }
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        g_wifi_connected = true;
        g_is_provisioning = false; 
        
        // 【关键修复：增加 SNTP 状态检查】
        if (!esp_sntp_enabled()) {
            ESP_LOGI(TAG, "Starting SNTP...");
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "ntp.aliyun.com");
            esp_sntp_init();
            // 设置时区（放在初始化后面确保生效）
            setenv("TZ", "CST-8", 1); 
            tzset();
        }
    } 
    else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD) {
        smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;
        wifi_config_t wifi_config;
        bzero(&wifi_config, sizeof(wifi_config_t));
        memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));

        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        esp_wifi_connect();
    }
}

// 修改后的触发函数
void trigger_smartconfig(void) {
    ESP_LOGI(TAG, "Starting SmartConfig...");
    // 【关键修复：设置配网标志位】
    g_is_provisioning = true; 
    
    esp_smartconfig_set_type(SC_TYPE_ESPTOUCH);
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    esp_smartconfig_start(&cfg);
}

void stop_smartconfig(void) {
    ESP_LOGI(TAG, "Stopping SmartConfig manually...");
    esp_smartconfig_stop();
    g_is_provisioning = false;
    esp_wifi_connect();
}

// =========================================================
// 波轮开关读取回调函数
// =========================================================
static void encoder_read_cb(lv_indev_t * indev, lv_indev_data_t * data) {
    // 记录上一次的状态，用于边缘检测
    static uint32_t last_left_state = 1;
    static uint32_t last_right_state = 1;
    
    // 用于记录长按的次数（时间 = hold_counter * 每次读取的周期时长）
    static uint32_t hold_counter = 0;

    // --- 可调节参数 ---
    // 假设 LVGL 默认输入设备的读取周期 (LV_DEF_REFR_PERIOD) 是 30ms
    const uint32_t HOLD_DELAY_TICKS = 15; // 长按多久才开始连发？ 15 * 30ms = 450ms
    const uint32_t REPEAT_TICKS = 3;      // 连发的速度有多快？ 3 * 30ms = 90ms 触发一次

    // 读取当前引脚的电平 (按下为 0，松开为 1)
    uint32_t left_state = gpio_get_level(PIN_ENC_LEFT);
    uint32_t right_state = gpio_get_level(PIN_ENC_RIGHT);
    uint32_t press_state = gpio_get_level(PIN_ENC_PRESS);

    int32_t diff = 0;

    // -------- 处理向左拨动 --------
    if (left_state == 0) {
        if (last_left_state == 1) {
            // 刚拨动的第一下（下降沿）
            diff = -1;
            hold_counter = 0; // 重置长按计数器
        } else {
            // 一直保持拨动状态
            hold_counter++;
            if (hold_counter >= HOLD_DELAY_TICKS) {
                // 超过初始延迟后，按照设定的速度连发
                if ((hold_counter - HOLD_DELAY_TICKS) % REPEAT_TICKS == 0) {
                    diff = -1;
                }
            }
        }
    } 
    // -------- 处理向右拨动 --------
    else if (right_state == 0) {
        if (last_right_state == 1) {
            // 刚拨动的第一下（下降沿）
            diff = 1;
            hold_counter = 0; // 重置长按计数器
        } else {
            // 一直保持拨动状态
            hold_counter++;
            if (hold_counter >= HOLD_DELAY_TICKS) {
                // 超过初始延迟后，按照设定的速度连发
                if ((hold_counter - HOLD_DELAY_TICKS) % REPEAT_TICKS == 0) {
                    diff = 1;
                }
            }
        }
    } 
    // -------- 没有任何拨动（松开状态） --------
    else {
        hold_counter = 0; // 只要松开，立马清零计数器
    }

    // 更新历史状态
    last_left_state = left_state;
    last_right_state = right_state;

    // 把变化量传给 LVGL
    data->enc_diff = diff;
    
    // 把按压状态传给 LVGL
    if (press_state == 0) {
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// =========================================================
// 1. DMA 传输完成回调 (释放 LVGL 渲染进程)
// =========================================================
static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    lv_display_t * disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

// =========================================================
// 2. LVGL 刷屏核心函数 (LVGL 将像素画好后，通过这里发给硬件)
// =========================================================
static void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    // 获取我们在初始化时存进去的屏幕句柄
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);
    
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;

    // 计算当前要刷新的像素总数
    uint32_t size = (offsetx2 - offsetx1 + 1) * (offsety2 - offsety1 + 1);
    
    // 【核心修复：高低字节互换】
    // 将 8位的指针强转为 16位，然后挨个把像素的高低 8 位对调！
    uint16_t *buf16 = (uint16_t *)px_map;
    for(uint32_t i = 0; i < size; i++) {
        buf16[i] = (buf16[i] >> 8) | (buf16[i] << 8);
    }
    
    // 通过 SPI 驱动把这块区域的像素推送到屏幕
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, px_map);
}

void app_main(void)
{
    // 1. 初始化 NVS 闪存 (必须，用于存储 Wi-Fi 账密)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 初始化网络接口和事件循环
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // 3. 配置 Wi-Fi 模式为 STA
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 4. 注册刚才写的事件回调函数
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // =======================================
    // 原有的 LEDC 和 SPI 屏幕初始化代码 (保持不变)
    // =======================================
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_0,
        .duty_resolution  = LEDC_TIMER_13_BIT, 
        .freq_hz          = 5000,              
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = PIN_NUM_BCKL,
        .duty           = 4095,                
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_CLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = -1, 
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        // 这里把传输上限加大，以支持横屏的缓冲大小
        .max_transfer_sz = 320 * 40 * sizeof(uint16_t), 
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = 80 * 1000 * 1000, 
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "Install ST7789 panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    
    esp_lcd_panel_invert_color(panel_handle, true);
    esp_lcd_panel_set_gap(panel_handle, 34, 0); 

    // 【极其关键的一步】：强制屏幕变为横屏模式 (Landscape)！
    // 如果画面颠倒了，把 false, true 改成 true, false 试试
    esp_lcd_panel_swap_xy(panel_handle, true); 
    esp_lcd_panel_mirror(panel_handle, false, true); 

    // 【关键修改 2】：横屏后，34 像素的物理黑边偏移量必须转移到 Y 轴！
    esp_lcd_panel_set_gap(panel_handle, 0, 34);

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    // =======================================
    // 启动 LVGL 与内存配置
    // =======================================
    // 1. 初始化 LVGL 引擎
    lv_init(); 

    // 2. 创建 LVGL 显示驱动 (注意这里写的是横屏尺寸 320x172)
    lv_display_t * disp = lv_display_create(320, 172);

    // 1. 直接拉满，一次性刷入 172 行 (全屏)
    #define LINES_PER_FLUSH 172
    size_t buffer_size = 320 * LINES_PER_FLUSH * sizeof(uint16_t);
    
    // 2. 只分配一个 buf1，强制使用内部高速 SRAM
    uint16_t *buf1 = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    
    if (buf1 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate 110KB internal DMA memory! (RAM fragmentation)");
        return;
    }

    // 3. 将回调、缓存绑定到 LVGL (注意：buf2 填 NULL)
    lv_display_set_flush_cb(disp, my_disp_flush);
    // 这里依然使用 PARTIAL 模式，但因为 buffer 已经和屏幕一样大了，它实际上会一波流发完
    lv_display_set_buffers(disp, buf1, NULL, buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(disp, panel_handle);

    // 5. 注册 DMA 完成硬件中断回调给 LVGL
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, disp);

    // 背光亮度开大
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 3000); // 0~4095
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

    // =======================================
    // 1. 初始化波轮开关的 GPIO
    // =======================================
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_ENC_LEFT) | (1ULL << PIN_ENC_PRESS) | (1ULL << PIN_ENC_RIGHT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, // 开启内部上拉（配合外部上拉更稳）
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE    // LVGL 会自动轮询，不需要中断
    };
    gpio_config(&io_conf);

    // =======================================
    // 2. 注册 LVGL 输入设备 (Encoder)
    // =======================================
    lv_indev_t * indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(indev, encoder_read_cb);

    // =======================================
    // 召唤 UI
    // =======================================
    setup_grub_os_ui();

    // =======================================
    // 永不退出的主循环 (LVGL 的心跳引擎)
    // =======================================
    uint32_t last_ms = esp_timer_get_time() / 1000;
    
    while (1) {
        // 1. 精确计算经过的毫秒数，喂给 LVGL
        uint32_t current_ms = esp_timer_get_time() / 1000;
        lv_tick_inc(current_ms - last_ms);
        last_ms = current_ms;
        
        // 2. 处理动画、渲染和事件
        lv_timer_handler();
        
        // 3. 【关键修改 3】延时至少 10ms (也就是 1 个完整的 Tick)，把 CPU 喘息的时间还给看门狗！
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}