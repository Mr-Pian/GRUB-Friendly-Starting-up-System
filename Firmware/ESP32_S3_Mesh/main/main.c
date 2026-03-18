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
#include "tinyusb.h"
#include "class/hid/hid_device.h" 
#include "tusb.h"
#include "tusb_console.h"
#include "soc/rtc_cntl_reg.h" // 引入底层 RTC 寄存器控制
#include "esp_system.h"
#include "soc/usb_serial_jtag_reg.h"
#include "ble_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_pm.h"

// 必须引入 LVGL 核心头文件
#include "lvgl.h" 
#include "LVGL_UI.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h" // 用于自动校验 HTTPS 证书 (Let's Encrypt)
#include "cJSON.h"          // 用于生成和解析 JSON 数据

static const char *TAG = "LCD_TEST";
// 加上 volatile 防止被编译器优化，专门给后台任务和 UI 桥接用
volatile bool g_wifi_connected = false;
volatile bool g_is_provisioning = false; // 【新增】标记是否正在配网
volatile bool g_server_connected = false; // 记录公网服务器存活状态

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

// 标准键盘的 HID 报告描述符
static const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};

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

    // // 计算当前要刷新的像素总数
    // uint32_t size = (offsetx2 - offsetx1 + 1) * (offsety2 - offsety1 + 1);
    
    // // 【核心修复：高低字节互换】
    // // 将 8位的指针强转为 16位，然后挨个把像素的高低 8 位对调！
    // uint16_t *buf16 = (uint16_t *)px_map;
    // for(uint32_t i = 0; i < size; i++) {
    //     buf16[i] = (buf16[i] >> 8) | (buf16[i] << 8);
    // }
    
    // 通过 SPI 驱动把这块区域的像素推送到屏幕
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, px_map);
}

// ==========================================
// TinyUSB HID 必须实现的核心回调函数
// ==========================================

// 1. 当电脑请求获取 HID 报告描述符时，触发此回调
uint8_t const * tud_hid_descriptor_report_cb(uint8_t instance)
{
    // 把我们前面定义的键盘描述符交还给电脑
    return hid_report_descriptor;
}

// 2. 当电脑发起 GET_REPORT 请求时触发（本项目暂不需要用到，返回 0 即可）
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
    return 0; 
}

// 3. 当电脑发起 SET_REPORT 请求时触发（例如按下电脑键盘的 CapsLock 键，电脑发信号让小键盘亮灯，本项目暂不处理）
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
    // 置空即可
}

// ==========================================
// 终极形态：纯净版 HID 键盘描述符 (剔除 CDC)
// ==========================================
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

enum {
    ITF_NUM_HID = 0,
    ITF_NUM_TOTAL
};

#define EPNUM_HID_IN 0x81 

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,    // 0x00 表示类定义在 Interface 层
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,  
    .idProduct = 0x4002,     // 改个 PID 防止电脑缓存旧配置
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof(hid_report_descriptor), EPNUM_HID_IN, 16, 10)
};

static const char* string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 }, 
    "Espressif",                   
    "S3 GRUB Assistor",            
    "114514",                      
};

void reboot_to_flash_mode(void) {
   ESP_LOGI(TAG, "Forcing D+ LOW to simulate unplug, then reboot to ROM...");
   REG_WRITE(RTC_CNTL_OPTION1_REG, 1);
   esp_restart();
}

// ==========================================
// 【防卡键神药】：绝对安全的 HID 报告发送器
// ==========================================
static void safe_hid_report(uint8_t modifier, uint8_t keycode[]) {
    // 每次发送前，最多等 50 毫秒看 USB 线路是否空闲
    for (int i = 0; i < 5; i++) {
        if (tud_hid_ready()) {
            tud_hid_keyboard_report(0, modifier, keycode);
            return; // 发送成功立刻退出
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // 线路忙，等 10ms 再试
    }
    ESP_LOGW(TAG, "HID Endpoint busy, packet dropped!");
}

static void reisub_macro_task(void *arg) {

    if (!tud_hid_ready()) {
        ESP_LOGW(TAG, "HID not ready, cannot send macro!");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Initiating Magic SysRq REISUB sequence...");

    uint8_t keycode[6] = {0};
    uint8_t modifier = KEYBOARD_MODIFIER_LEFTALT; // 保持按下 Left Alt

    // SysRq 在标准 USB HID 协议中对应的是 Print Screen 键 (0x46)
    keycode[0] = HID_KEY_PRINT_SCREEN; 

    // 1. 发送 Alt + SysRq 的初始按下状态，让内核准备接客
    safe_hid_report(modifier, keycode);
    vTaskDelay(pdMS_TO_TICKS(500)); 

    // 2. 依次按下 R, E, I, S, U, B
    uint8_t seq[] = {HID_KEY_R, HID_KEY_E, HID_KEY_I, HID_KEY_S, HID_KEY_U, HID_KEY_B};
    
    for(int i = 0; i < 6; i++) {
        // 按下字母键 (同时保持 Alt+SysRq 按下)
        keycode[1] = seq[i];
        safe_hid_report(modifier, keycode);
        vTaskDelay(pdMS_TO_TICKS(200)); // 模拟人手按压的持续时间

        // 松开字母键 (但保持 Alt+SysRq 按下)
        keycode[1] = 0; 
        safe_hid_report(modifier, keycode);
        
        // 【灵魂细节】：进程结束和硬盘同步需要时间！
        if (seq[i] == HID_KEY_S) {
            // Sync 阶段必须多等一会儿，给硬盘写入缓存的时间，保护数据
            vTaskDelay(pdMS_TO_TICKS(1500)); 
        } else {
            vTaskDelay(pdMS_TO_TICKS(400));
        }
    }

    // 3. 宏发送完毕，释放所有按键
    safe_hid_report(0, NULL);
    ESP_LOGI(TAG, "REISUB macro complete. System should reboot now.");
    
    // 销毁当前任务
    vTaskDelete(NULL);
}

// 暴露给 LVGL 按钮调用的触发函数
void trigger_linux_reisub(void) {
    // 创建一个独立任务来跑这个宏，绝对不卡死 UI！
    xTaskCreate(reisub_macro_task, "reisub_task", 2048, NULL, 5, NULL);
}

static void win_boot_macro_task(void *arg) {
    ESP_LOGI(TAG, "Waiting for PC USB enumeration (BIOS phase)...");
    uint32_t wait_ms = 0;
    
    // 1. 等待主板 BIOS/UEFI 首次给 USB 供电并握手
    while (!tud_hid_ready()) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_ms += 100;
        if (wait_ms > 40000) {
            ESP_LOGW(TAG, "PC boot timeout, macro aborted.");
            vTaskDelete(NULL);
            return;
        }
    }
    
    // 【关键修复 1】：跳过主板 LOGO 自检期！
    // 此时 BIOS 刚认识键盘，但 GRUB 还没出来。
    // 根据你电脑开机的实际速度，在这里盲等 3~6 秒。我先给你设了 4 秒。
    ESP_LOGI(TAG, "BIOS USB Ready! Sleeping 4 seconds to bypass Motherboard POST...");
    vTaskDelay(pdMS_TO_TICKS(4000)); 
    
    ESP_LOGI(TAG, "Initiating 15-second 'W' spam for GRUB...");
    
    // 【关键修复 2】：扩大火力覆盖，并降低按键频率防止 GRUB 吞键！
    // 循环 75 次 * (100ms按下 + 100ms松开) = 15 秒的持续火力
    for (int i = 0; i < 75; i++) {
        uint8_t keycode[6] = {HID_KEY_W, 0};
        
        // 按下 W，保持 100ms (给迟钝的 GRUB 足够的反应时间)
        safe_hid_report(0, keycode);
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // 松开 W，保持 100ms
        safe_hid_report(0, NULL);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    // 发送最后一次清空包，确保绝对不卡键
    safe_hid_report(0, NULL);
    ESP_LOGI(TAG, "Windows boot macro finished.");
    vTaskDelete(NULL);
}

void trigger_windows_boot_macro(void) {
    // 创建一个后台任务，绝对不阻塞 LVGL 渲染
    xTaskCreate(win_boot_macro_task, "win_boot_task", 2048, NULL, 5, NULL);
}

void print_task_stats() {
    // 申请一块足够大的内存来存放统计字符串
    char *buf = malloc(2048);
    if (buf == NULL) return;

    printf("\n任务名称       状态   优先级  剩余堆栈   任务编号   CPU核心\n");
    printf("----------------------------------------------------------\n");
    // vTaskList 会列出任务名称、状态、优先级、剩余堆栈
    vTaskList(buf);
    printf("%s", buf);

    printf("\n任务名称       运行时间(绝对值)    百分比\n");
    printf("----------------------------------------\n");
    // vTaskGetRunTimeStats 会列出每个任务消耗 CPU 的百分比
    vTaskGetRunTimeStats(buf);
    printf("%s", buf);

    free(buf);
}

// =========================================================
// 公网服务器联动引擎 (HTTP 心跳与指令下发)
// =========================================================
static void server_sync_task(void *arg) {
    char post_data[128];
    char response_buffer[512];

    // 1. 【发热优化】：将初始化移出死循环！
    // 保持 TCP/TLS 长连接，避免每 3 秒做一次极度耗费 CPU 的非对称加密握手
    esp_http_client_config_t config = {
        .url = "https://boot.mambo.icu/api/esp/sync",
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 4000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    while (1) {
        if (!g_wifi_connected) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        for (int i = 0; i < 2; i++) {
            bool online = false, pc_on = false;
            uint16_t batt = 0;

            ui_get_node_state(i, &online, &pc_on, &batt);
            if (!online) continue;

            snprintf(post_data, sizeof(post_data), 
                     "{\"node\":\"%s\",\"batt\":%.2f,\"pc_on\":%s}",
                     (i == 0) ? "mambo" : "kang",
                     batt / 1000.0f,
                     pc_on ? "true" : "false");

            // 2. 每次循环只更新 Body 数据
            esp_http_client_set_post_field(client, post_data, strlen(post_data));

            esp_err_t err = esp_http_client_open(client, strlen(post_data));
            if (err == ESP_OK) {
                esp_http_client_write(client, post_data, strlen(post_data));
                esp_http_client_fetch_headers(client);
                
                g_server_connected = true; 
                
                int total_read = 0;
                while (total_read < sizeof(response_buffer) - 1) {
                    int len = esp_http_client_read_response(client, response_buffer + total_read, sizeof(response_buffer) - 1 - total_read);
                    if (len <= 0) break; 
                    total_read += len;
                }
                response_buffer[total_read] = '\0'; 
                
                if (total_read > 0) {
                    cJSON *root = cJSON_Parse(response_buffer);
                    if (root != NULL) {
                        cJSON *has_cmd = cJSON_GetObjectItem(root, "has_cmd");
                        
                        if (has_cmd && cJSON_IsTrue(has_cmd)) {
                            cJSON *action = cJSON_GetObjectItem(root, "action");
                            cJSON *os = cJSON_GetObjectItem(root, "os");
                            
                            if (action && action->valuestring) {
                                uint8_t cmd_code = 0;
                                if (strcmp(action->valuestring, "boot") == 0) cmd_code = 0x01;
                                else if (strcmp(action->valuestring, "reset") == 0) cmd_code = 0x02;
                                else if (strcmp(action->valuestring, "force_off") == 0) cmd_code = 0x03;

                                if (cmd_code != 0) {
                                    ESP_LOGI(TAG, "⚡ [Server Command] Executing %s for node %d", action->valuestring, i);
                                    ble_trigger_pc_command(i, cmd_code);

                                    if (i == 0 && cmd_code == 0x01 && os && os->valuestring) {
                                        if (strcmp(os->valuestring, "win") == 0) {
                                            ESP_LOGI(TAG, "⌨️ [Server Command] Triggering Windows GRUB Macro...");
                                            trigger_windows_boot_macro();
                                        }
                                    }
                                }
                            }
                        }
                        cJSON_Delete(root);
                    }
                }
                // 3. 【发热优化】：用 close 代替 cleanup，仅关闭当前流，但保持 TLS Session 不死！
                esp_http_client_close(client);
            } else {
                g_server_connected = false;
                ESP_LOGE(TAG, "HTTP Open failed: %s", esp_err_to_name(err));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    // 虽然是个死循环永远走不到这里，但语法上写上防止警告
    esp_http_client_cleanup(client);
}

void app_main(void)
{

    #if CONFIG_PM_ENABLE
        esp_pm_config_t pm_config = {
            .max_freq_mhz = 240,        // 满血爆发频率，确保 LVGL 动画和蓝牙出击绝不卡顿
            .min_freq_mhz = 80,         // 挂机退烧频率，设为 80MHz 确保 SPI 屏幕不闪烁、不撕裂
            .light_sleep_enable = false // 带有 LCD 屏幕的设备强力建议设为 false，防止花屏
        };
        ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
        ESP_LOGI(TAG, "Dynamic Frequency Scaling enabled! Min: 80MHz, Max: 240MHz");
    #endif

    // 1. 初始化 NVS 闪存 (必须，用于存储 Wi-Fi 账密)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret); 

   // ==========================================
    // 2. 初始化 TinyUSB (CDC + HID 复合设备)
    // ==========================================
    ESP_LOGI(TAG, "USB initialization");
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &desc_device,
        .string_descriptor = string_desc_arr,
        .string_descriptor_count = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]),
        .external_phy = false,
        .configuration_descriptor = desc_configuration,
    };
    
    esp_err_t usb_err = tinyusb_driver_install(&tusb_cfg);
    if(usb_err != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB Init Failed! Error code: %d", usb_err);
    } else {
        ESP_LOGI(TAG, "TinyUSB Init SUCCESS!");
    }
    // ==========================================

    // 2. 初始化网络接口和事件循环
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // 3. 配置 Wi-Fi 模式为 STA
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

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
        .max_transfer_sz = 320 * 172 * sizeof(uint16_t), 
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

    // 1. 让底层硬件自己处理横屏矩阵！
    esp_lcd_panel_swap_xy(panel_handle, true); 
    
    // 2. 硬件镜像微调（如果字是反的，改这里为 true, false 或者 false, false）
    esp_lcd_panel_mirror(panel_handle, false, true); 

    // 3. 硬件横屏下的黑边补偿（必须转移到 Y 轴！）
    esp_lcd_panel_set_gap(panel_handle, 0, 34);

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    // =======================================
    // 启动 LVGL 与内存配置
    // =======================================
    // 1. 初始化 LVGL 引擎
    lv_init(); 

    lv_display_t * disp = lv_display_create(320, 172);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    
    // 【核心战略转移】：使用 1/2 屏幕 (86行)
    // 大约占用 55KB 内存。因为 LVGL 不抢地盘了，内部 SRAM 绝对装得下！
    #define LINES_PER_FLUSH 86  
    size_t buffer_size = 320 * LINES_PER_FLUSH * sizeof(uint16_t);
    
    // 【终极魔法】：彻底抛弃 PSRAM，使用 MALLOC_CAP_INTERNAL！
    uint16_t *buf1 = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    
    if (buf1 == NULL) {
        ESP_LOGE(TAG, "FATAL: Failed to allocate 55KB internal DMA memory!");
        return;
    }
    ESP_LOGI(TAG, "SUCCESS: Allocated 55KB Buffer in INTERNAL SRAM!");

    lv_display_set_flush_cb(disp, my_disp_flush);

    // 【必须】改回 PARTIAL 局部刷新模式
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
    // 新增：把蓝牙初始化挪到这里！等显存申请完再启动蓝牙！
    // =======================================
    ble_client_init();

    // =======================================
    // 召唤 UI
    // =======================================
    setup_grub_os_ui();

    xTaskCreatePinnedToCore(server_sync_task, "server_sync", 8192, NULL, 5, NULL, 1);
    // =======================================
    // 永不退出的主循环 (LVGL 的心跳引擎)
    // =======================================
    uint32_t last_ms = esp_timer_get_time() / 1000;
    //static uint32_t last_stats_ms = 0;
    
    while (1) {
        // 1. 精确计算经过的毫秒数，喂给 LVGL
        uint32_t current_ms = esp_timer_get_time() / 1000;
        lv_tick_inc(current_ms - last_ms);
        last_ms = current_ms;
        
        // 2. 处理动画、渲染和事件
        lv_timer_handler();
        
        // 3. 【关键修改 3】延时至少 10ms (也就是 1 个完整的 Tick)，把 CPU 喘息的时间还给看门狗！
        vTaskDelay(pdMS_TO_TICKS(10)); 
        
        // if (current_ms - last_stats_ms > 5000) {
        // print_task_stats();
        // last_stats_ms = current_ms;
    }
    
}