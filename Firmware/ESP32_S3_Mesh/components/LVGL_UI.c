#include "LVGL_UI.h"
// 【修复 1】：引入蓝牙头文件，打通底层接口和回调函数的跨文件链接
#include "ble_client.h" 
#include "driver/ledc.h"
#include "nvs_flash.h"
#include "nvs.h"

extern volatile bool g_server_connected;
extern volatile bool g_wifi_connected;
extern volatile bool g_is_provisioning;
extern void stop_smartconfig(void);
extern void trigger_smartconfig(void);
extern void trigger_windows_boot_macro(void);
static lv_obj_t * lbl_wifi; 

// ==================== 全局资源声明 ====================
LV_FONT_DECLARE(ka1);
LV_IMG_DECLARE(computer);
LV_IMG_DECLARE(power_button);
LV_IMG_DECLARE(linux_button);
LV_IMG_DECLARE(connection);
LV_IMG_DECLARE(settings);

// ==================== 屏幕与容器指针 ====================
static lv_obj_t * scr_main;
static lv_obj_t * scr_aod;

static lv_obj_t * scr_boot;
static lv_obj_t * scr_linux;
static lv_obj_t * scr_mesh;
static lv_obj_t * scr_settings;
static lv_obj_t * label_time;

static lv_style_t style_focus; 
static lv_group_t * g_main;

static lv_obj_t * int_boot;
static lv_obj_t * int_linux;
static lv_obj_t * int_mesh;
static lv_obj_t * int_settings;

static lv_obj_t * active_app_scr = NULL;
static lv_obj_t * active_app_interior = NULL;

static lv_area_t start_area;
static lv_obj_t * dummy_obj;

static lv_obj_t * btn_arch;
static lv_obj_t * btn_win;
static lv_obj_t * label_arch;
static lv_obj_t * label_win;

// ==================== 【一拖二重构】UI 状态记录器 ====================
static volatile bool pc_is_on[2] = {false, false};
static volatile bool ble_is_connected[2] = {false, false};

// 【修复 2】：补全所有需要动态修改的控件指针，并确保它们是全局的
static lv_obj_t * pc_icon;
static lv_obj_t * pc_label;
static lv_obj_t * btn_boot_res;
static lv_obj_t * lbl_boot_res;
static lv_obj_t * btn_force_off; 

#define MAX_STATUS_BARS 10
static lv_obj_t * wifi_icons[MAX_STATUS_BARS];
static lv_obj_t * uptime_labels[MAX_STATUS_BARS];
static lv_obj_t * ble_icons[MAX_STATUS_BARS][2]; // 状态栏上的两个蓝牙图标
static uint8_t status_bar_cnt = 0;
static uint32_t last_seen_ms[2] = {0, 0};
static lv_obj_t * lbl_batt[2] = {NULL, NULL};
static lv_obj_t * lbl_node_name[2] = {NULL, NULL};
static lv_obj_t * lbl_conn_time[2] = {NULL, NULL};
static uint32_t conn_start_time[2] = {0, 0};
static uint16_t current_batt_mv[2] = {0, 0};

// 【新增】：记录当前亮度的全局变量
static uint8_t current_active_bri = 80;
static uint8_t current_aod_bri = 20;

static uint8_t selected_os = 0; // 0 for Arch, 1 for Win

// 暴露出底层的物理状态，供 main.c 的 HTTP 任务读取
void ui_get_node_state(uint8_t node, bool *is_online, bool *pc_on, uint16_t *batt_mv) {
    if(node < 2) {
        *is_online = ble_is_connected[node];
        *pc_on = pc_is_on[node];
        *batt_mv = current_batt_mv[node];
    }
}

// 【新增】：一个通用的底层背光刷新函数
static void update_hw_backlight(uint8_t percent) {
    // 【物理截断】：满载是 8191，我们砍掉最亮的 20%，最高限制在 6553 左右
    uint32_t max_duty = 8191 * 0.8; 
    
    // 【Gamma 平方校正】：percent * percent / 10000 
    // 这样推 50% 时，实际输出只有 25%，把大量的滑动空间留给了低亮度区间！
    uint32_t duty = (percent * percent * max_duty) / 10000;
    
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void load_brightness_from_nvs(void) {
    nvs_handle_t my_handle;
    // 打开命名空间 "storage"
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        nvs_get_u8(my_handle, "act_bri", &current_active_bri);
        nvs_get_u8(my_handle, "aod_bri", &current_aod_bri);
        nvs_close(my_handle);
    }
}

static void save_brightness_to_nvs(void) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_u8(my_handle, "act_bri", current_active_bri);
        nvs_set_u8(my_handle, "aod_bri", current_aod_bri);
        nvs_commit(my_handle); // 必须 commit 才会写入物理 Flash
        nvs_close(my_handle);
    }
}

// 【新增】：专门用于 LVGL 松手时的回调
static void slider_save_nvs_cb(lv_event_t * e) {
    save_brightness_to_nvs();
}

// 【新增】：正常亮度调节回调（实时生效）
static void slider_active_bri_cb(lv_event_t * e) {
    lv_obj_t * slider = lv_event_get_target(e);
    current_active_bri = lv_slider_get_value(slider);
    
    // 如果当前没在息屏状态，拖动滑动条立刻改变物理屏幕亮度
    if(lv_screen_active() != scr_aod) {
        update_hw_backlight(current_active_bri);
    }
}

// 【新增】：AOD息屏亮度调节回调（只存变量，进入息屏才生效）
static void slider_aod_bri_cb(lv_event_t * e) {
    lv_obj_t * slider = lv_event_get_target(e);
    current_aod_bri = lv_slider_get_value(slider);
}

// ==================== 接收底层蓝牙状态的回调 ====================
void ui_ble_connection_state_cb(uint8_t target_node, bool connected) {
   if(target_node < 2) {
        if (connected) {
            last_seen_ms[target_node] = esp_timer_get_time() / 1000;
            
            // 【新增】：如果是从“断开”变为“连接”的第一瞬间，记录起点时间
            if (!ble_is_connected[target_node]) {
                conn_start_time[target_node] = esp_timer_get_time() / 1000000ULL; 
            }
        }
        ble_is_connected[target_node] = connected; 
    }
}

void ui_ble_command_result_cb(uint8_t target_node, uint8_t cmd, bool success) {
    // if(target_node < 2 && success) {
    //     // 指令发送成功，根据指令更新电脑状态的 UI
    //     if(cmd == 0x01) pc_is_on[target_node] = true;       // 开机指令 -> 绿
    //     else if(cmd == 0x03) pc_is_on[target_node] = false; // 关机指令 -> 灰
    //     // 0x02 重启指令不改变状态
    // }
}

void ui_ble_real_pc_state_cb(uint8_t target_node, bool is_on, uint16_t batt_mv) {
    if(target_node < 2) {
        // 1. 同步最真实的物理开机状态
        pc_is_on[target_node] = is_on; 
        
        // 2. 【核心修复】：绝对不能在这里调用 lv_label_set_text！
        // 只把电压缓存到全局变量，剩下的交给 LVGL 自己的定时器去画。
        current_batt_mv[target_node] = batt_mv; 
    }
}

extern void reboot_to_flash_mode(void);
static void do_reboot_timer_cb(lv_timer_t * timer) {
    reboot_to_flash_mode();
}

extern void trigger_linux_reisub(void);
static void btn_reisub_cb(lv_event_t * e) {
    trigger_linux_reisub();
}

static void flash_btn_cb(lv_event_t * e) {
    lv_obj_t * scr_download = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_download, lv_color_hex(0x000000), 0);

    lv_obj_t * label = lv_label_create(scr_download);
    lv_label_set_text(label, LV_SYMBOL_DOWNLOAD " Entering Download Mode...\nWaiting for firmware download.\n\nHint: Due to the chip startup process\nplease manually reset the chip after\ndownload, otherwise it may fail to enter\ndownload mode next time");
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x28A745), 0); 
    lv_obj_center(label);

    lv_screen_load(scr_download);

    lv_timer_t * timer = lv_timer_create(do_reboot_timer_cb, 300, NULL);
    lv_timer_set_repeat_count(timer, 1);
}

static void add_focusable_to_group(lv_obj_t * obj) {
    if(!obj) return;
    
    const lv_obj_class_t * cls = lv_obj_get_class(obj);
    if(cls == &lv_button_class || cls == &lv_slider_class || cls == &lv_tileview_class) {
        lv_group_add_obj(g_main, obj);
    }

    uint32_t child_cnt = lv_obj_get_child_cnt(obj);
    for(uint32_t i = 0; i < child_cnt; i++) {
        add_focusable_to_group(lv_obj_get_child(obj, i));
    }
}

static void refresh_group_focus(lv_obj_t * active_screen) {
    if(!g_main) return;
    lv_group_remove_all_objs(g_main); 
    add_focusable_to_group(active_screen); 
}

static void create_status_bar(lv_obj_t * parent) {
    lv_obj_t * status_bar = lv_obj_create(parent);
    lv_obj_set_size(status_bar, 320, 24);
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_radius(status_bar, 0, 0); 
    lv_obj_set_style_border_width(status_bar, 1, 0);
    lv_obj_set_style_border_side(status_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(status_bar, lv_color_hex(0x333333), 0);
    lv_obj_set_style_pad_all(status_bar, 2, 0);

    lv_obj_t * wifi_icon = lv_label_create(status_bar);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_align(wifi_icon, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0xd40f12), 0);

    lv_obj_t * ble1 = lv_label_create(status_bar);
    lv_label_set_text(ble1, LV_SYMBOL_BLUETOOTH "1");
    lv_obj_align_to(ble1, wifi_icon, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    lv_obj_set_style_text_color(ble1, lv_color_hex(0x555555), 0); 

    lv_obj_t * ble2 = lv_label_create(status_bar);
    lv_label_set_text(ble2, LV_SYMBOL_BLUETOOTH "2");
    lv_obj_align_to(ble2, ble1, LV_ALIGN_OUT_RIGHT_MID, 5, 0);
    lv_obj_set_style_text_color(ble2, lv_color_hex(0x555555), 0);

    lv_obj_t * uptime_label = lv_label_create(status_bar);
    lv_label_set_text(uptime_label, "Up: 00h00m");
    lv_obj_align(uptime_label, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_style_text_color(uptime_label, lv_color_hex(0xAAAAAA), 0);

    if(status_bar_cnt < MAX_STATUS_BARS) {
        wifi_icons[status_bar_cnt] = wifi_icon;
        ble_icons[status_bar_cnt][0] = ble1; 
        ble_icons[status_bar_cnt][1] = ble2;
        uptime_labels[status_bar_cnt] = uptime_label;
        status_bar_cnt++;
    }
}

static void ui_state_update_task(lv_timer_t * timer) {
    uint32_t now_ms = esp_timer_get_time() / 1000;
    uint32_t now_sec = now_ms / 1000;
    for(int n = 0; n < 2; n++) {
        if(ble_is_connected[n]) {
            if((now_ms - last_seen_ms[n]) > 20000) {
                ble_is_connected[n] = false; // 超时断联
            } else {
                // 【在线状态】：计算连接时长，并高亮所有文字
                if(lbl_conn_time[n]) {
                    uint32_t duration = now_sec - conn_start_time[n];
                    char t_buf[16];
                    snprintf(t_buf, sizeof(t_buf), LV_SYMBOL_WIFI " %02d:%02d", (int)(duration / 3600), (int)(duration % 3600) / 60);
                    lv_label_set_text(lbl_conn_time[n], t_buf);
                }
                if(lbl_node_name[n]) lv_obj_set_style_text_color(lbl_node_name[n], lv_color_hex(0x17A2B8), 0); // 青蓝色
                if(lbl_batt[n]) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), LV_SYMBOL_BATTERY_FULL " %.3fV", current_batt_mv[n] / 1000.0f);
                    lv_label_set_text(lbl_batt[n], buf); // 安全刷新文字
                    lv_obj_set_style_text_color(lbl_batt[n], lv_color_hex(0x28A745), 0); // 变绿
                }
                if(lbl_conn_time[n]) lv_obj_set_style_text_color(lbl_conn_time[n], lv_color_hex(0xFFFFFF), 0); // 白色
            }
        }
        
        // 【离线状态】：无论是一直没连上，还是刚超时断开，全部置灰！
        if(!ble_is_connected[n]) {
            if(lbl_conn_time[n]) lv_label_set_text(lbl_conn_time[n], LV_SYMBOL_WIFI " --:--");
            if(lbl_node_name[n]) lv_obj_set_style_text_color(lbl_node_name[n], lv_color_hex(0x555555), 0);
            if(lbl_batt[n])      lv_obj_set_style_text_color(lbl_batt[n], lv_color_hex(0x555555), 0);
            if(lbl_conn_time[n]) lv_obj_set_style_text_color(lbl_conn_time[n], lv_color_hex(0x555555), 0);
        }
    }
    char uptime_str[32];
    uint32_t up_sec = esp_timer_get_time() / 1000000ULL; 
    uint32_t h = up_sec / 3600;
    uint32_t m = (up_sec % 3600) / 60;
    snprintf(uptime_str, sizeof(uptime_str), "Up: %02dh%02dm", (int)h, (int)m);

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    bool is_time_synced = (timeinfo.tm_year > (2024 - 1900));
    char time_str[16];
    if (is_time_synced) {
        snprintf(time_str, sizeof(time_str), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    } else {
        snprintf(time_str, sizeof(time_str), "--:--"); 
    }

    if(label_time) {
        lv_label_set_text(label_time, time_str);
    }

    // 刷新所有的状态栏
    for(int i = 0; i < status_bar_cnt; i++) {
        if(wifi_icons[i]) {
            // 【三重网络状态判断】
            if(g_server_connected) {
                // 状态 1：彻底连通公网服务器 -> 耀眼绿
                lv_obj_set_style_text_color(wifi_icons[i], lv_color_hex(0x28A745), 0); 
            } else if (g_wifi_connected) {
                // 状态 2：路由器连上了，但服务器没理你 -> 警告橙
                lv_obj_set_style_text_color(wifi_icons[i], lv_color_hex(0xFF9800), 0); 
            } else {
                // 状态 3：连 Wi-Fi 都没连上 -> 掉线红
                lv_obj_set_style_text_color(wifi_icons[i], lv_color_hex(0xd40f12), 0); 
            }
        }
        for(int n = 0; n < 2; n++) {
            if(ble_icons[i][n]) {
                if(ble_is_connected[n]) {
                    lv_obj_set_style_text_color(ble_icons[i][n], lv_color_hex(0x007BFF), 0); 
                } else {
                    lv_obj_set_style_text_color(ble_icons[i][n], lv_color_hex(0x555555), 0); 
                }
            }
        }
        if(uptime_labels[i]) {
            lv_label_set_text(uptime_labels[i], uptime_str);
        }
    }

    if(lbl_wifi) {
        if(!g_is_provisioning) {
            const char * current_text = lv_label_get_text(lbl_wifi);
            if(strstr(current_text, "Waiting")) {
                if(g_wifi_connected) {
                    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI " Connected!");
                    lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(0x28A745), 0);
                } else {
                    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI "Network");
                    lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(0xFFFFFF), 0);
                }
            }
        }
    }

    // 【核心刷新】：根据物理开机状态动态变化按钮
    if(pc_icon && pc_label && btn_boot_res) {
        if (!ble_is_connected[0]) {
            // 【状态1】：设备脱机，全部置灰，按钮锁死不可按！
            lv_obj_set_style_image_recolor(pc_icon, lv_color_hex(0x555555), 0);
            lv_obj_set_style_image_recolor_opa(pc_icon, LV_OPA_COVER, LV_PART_MAIN); 
            lv_label_set_text(pc_label, "Status: OFFL.");
            lv_obj_set_style_text_color(pc_label, lv_color_hex(0x555555), 0);
            
            lv_obj_set_style_bg_color(btn_boot_res, lv_color_hex(0x333333), 0);
            lv_label_set_text(lbl_boot_res, LV_SYMBOL_POWER " OFFL.");
            lv_obj_add_state(btn_boot_res, LV_STATE_DISABLED); // LVGL 原生禁用按键属性
        } else {
            // 设备在线，解除按钮锁定
            lv_obj_clear_state(btn_boot_res, LV_STATE_DISABLED);

            if(pc_is_on[0]) {
                // 【状态2】：设备在线且已开机 -> 绿色 RES
                lv_obj_set_style_image_recolor_opa(pc_icon, LV_OPA_TRANSP, LV_PART_MAIN);
                lv_label_set_text(pc_label, "Status: ON");
                lv_obj_set_style_text_color(pc_label, lv_color_hex(0x28A745), 0);
                
                lv_obj_set_style_bg_color(btn_boot_res, lv_color_hex(0xFD7E14), 0);
                lv_label_set_text(lbl_boot_res, LV_SYMBOL_REFRESH " RES");
            } else {
                // 【状态3】：设备在线但未开机 -> 待命 BOOT
                lv_obj_set_style_image_recolor(pc_icon, lv_color_hex(0x555555), 0);
                lv_obj_set_style_image_recolor_opa(pc_icon, LV_OPA_COVER, LV_PART_MAIN); 
                lv_label_set_text(pc_label, "Status: OFF");
                lv_obj_set_style_text_color(pc_label, lv_color_hex(0x888888), 0);
                
                lv_obj_set_style_bg_color(btn_boot_res, lv_color_hex(0x28A745), 0);
                lv_label_set_text(lbl_boot_res, LV_SYMBOL_POWER " BOOT");
            }
        }
    }
}

static void back_to_main_cb(lv_event_t * e) {
    lv_screen_load_anim(scr_main, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
    refresh_group_focus(scr_main);
}

static void create_app_base(lv_obj_t ** scr_ptr, lv_obj_t ** interior_ptr) {
    *scr_ptr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(*scr_ptr, lv_color_hex(0x000000), 0); 
    lv_obj_set_style_bg_opa(*scr_ptr, LV_OPA_COVER, 0);

    create_status_bar(*scr_ptr);

    lv_obj_t * content_box = lv_obj_create(*scr_ptr);
    lv_obj_set_size(content_box, 320, 148); 
    lv_obj_align(content_box, LV_ALIGN_BOTTOM_MID, 0, 0); 
    lv_obj_set_style_bg_color(content_box, lv_color_hex(0x2D2D2D), 0); 
    lv_obj_set_style_radius(content_box, 0, 0); 
    lv_obj_set_style_border_width(content_box, 0, 0);
    lv_obj_set_scrollbar_mode(content_box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(content_box, 0, 0); 

    *interior_ptr = lv_obj_create(content_box);
    lv_obj_set_size(*interior_ptr, 320, 148);
    lv_obj_set_style_bg_opa(*interior_ptr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(*interior_ptr, 0, 0);
    lv_obj_set_style_pad_all(*interior_ptr, 0, 0);
    lv_obj_set_style_opa(*interior_ptr, 0, 0); 

    lv_obj_t * btn_back = lv_button_create(*interior_ptr);
    lv_obj_set_size(btn_back, 60, 30);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 5, 4); 
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x3A3A3A), 0); 
    lv_obj_t * label_back = lv_label_create(btn_back);
    lv_label_set_text(label_back, LV_SYMBOL_LEFT " Back"); 
    lv_obj_center(label_back);
    lv_obj_add_event_cb(btn_back, back_to_main_cb, LV_EVENT_CLICKED, NULL);
}

static void fade_in_anim_cb(void * var, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)var, v, 0);
}

static void expand_anim_cb(void * var, int32_t v) {
    lv_obj_t * obj = (lv_obj_t *)var;
    int32_t start_x = start_area.x1;
    int32_t start_y = start_area.y1;
    int32_t start_w = lv_area_get_width(&start_area);
    int32_t start_h = lv_area_get_height(&start_area);

    int32_t current_x = start_x + ((0 - start_x) * v) / 1024;
    int32_t current_y = start_y + ((24 - start_y) * v) / 1024; 
    int32_t current_w = start_w + ((320 - start_w) * v) / 1024;
    int32_t current_h = start_h + ((148 - start_h) * v) / 1024; 
    int32_t current_r = 22 - ((22 - 0) * v) / 1024;

    lv_obj_set_pos(obj, current_x, current_y);
    lv_obj_set_size(obj, current_w, current_h);
    lv_obj_set_style_radius(obj, current_r, 0);
}

static void expand_anim_ready_cb(lv_anim_t * a) {
    lv_screen_load_anim(active_app_scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    lv_obj_delete(dummy_obj);
    refresh_group_focus(active_app_scr);

    lv_anim_t opa_anim;
    lv_anim_init(&opa_anim);
    lv_anim_set_var(&opa_anim, active_app_interior);
    lv_anim_set_values(&opa_anim, 0, 255); 
    lv_anim_set_time(&opa_anim, 200);
    lv_anim_set_exec_cb(&opa_anim, (lv_anim_exec_xcb_t)fade_in_anim_cb);
    lv_anim_start(&opa_anim);
}

static void enter_app_cb(lv_event_t * e) {
    lv_obj_t * btn = lv_event_get_target(e);
    lv_obj_get_coords(btn, &start_area);

    int app_id = (int)(uintptr_t)lv_event_get_user_data(e);
    if(app_id == 0)      { active_app_scr = scr_boot;     active_app_interior = int_boot; }
    else if(app_id == 1) { active_app_scr = scr_linux;    active_app_interior = int_linux; }
    else if(app_id == 2) { active_app_scr = scr_mesh;     active_app_interior = int_mesh; }
    else if(app_id == 3) { active_app_scr = scr_settings; active_app_interior = int_settings; }

    lv_obj_set_style_opa(active_app_interior, 0, 0);

    dummy_obj = lv_obj_create(lv_layer_top());
    lv_obj_set_style_bg_color(dummy_obj, lv_color_hex(0x2D2D2D), 0);
    lv_obj_set_style_border_width(dummy_obj, 0, 0);
    lv_obj_set_scrollbar_mode(dummy_obj, LV_SCROLLBAR_MODE_OFF);
    
    lv_obj_set_pos(dummy_obj, start_area.x1, start_area.y1);
    lv_obj_set_size(dummy_obj, lv_area_get_width(&start_area), lv_area_get_height(&start_area));
    lv_obj_set_style_radius(dummy_obj, 22, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, dummy_obj);
    lv_anim_set_values(&a, 0, 1024);
    lv_anim_set_time(&a, 350);       
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out); 
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)expand_anim_cb); 
    lv_anim_set_ready_cb(&a, expand_anim_ready_cb);
    lv_anim_start(&a);
}

static void os_select_cb(lv_event_t * e) {
    lv_obj_t * target = lv_event_get_target(e);
    lv_color_t bg_inactive = lv_color_hex(0x2D2D2D); 
    lv_color_t txt_inactive = lv_color_hex(0x888888);
    lv_color_t arch_active = lv_color_hex(0x17A2B8); 
    lv_color_t win_active  = lv_color_hex(0x0078D7); 
    lv_color_t txt_active  = lv_color_hex(0xFFFFFF); 

    if(target == btn_arch) {
        selected_os = 0;
        lv_obj_set_style_bg_color(btn_arch, arch_active, 0);
        lv_obj_set_style_text_color(label_arch, txt_active, 0);
        lv_obj_set_style_bg_color(btn_win, bg_inactive, 0);
        lv_obj_set_style_text_color(label_win, txt_inactive, 0);
    } else if(target == btn_win) {
        selected_os = 1;
        lv_obj_set_style_bg_color(btn_win, win_active, 0);
        lv_obj_set_style_text_color(label_win, txt_active, 0);
        lv_obj_set_style_bg_color(btn_arch, bg_inactive, 0);
        lv_obj_set_style_text_color(label_arch, txt_inactive, 0);
    }
}

// 【修复 4】：更新这部分，匹配蓝牙接口参数
static void btn_boot_res_cb(lv_event_t * e) {
    if(pc_is_on[0]) {
        ble_trigger_pc_command(0, 0x02); // 已开机则发 Reset
    } else {
        ble_trigger_pc_command(0, 0x01); // 未开机则发 Boot

        if (selected_os == 1) { 
            trigger_windows_boot_macro();
        }
    }
}

static void btn_force_off_cb(lv_event_t * e) {
    ble_trigger_pc_command(0, 0x03); 
}

static void init_boot_screen() {
    create_app_base(&scr_boot, &int_boot);

    lv_obj_t * pc_status_box = lv_obj_create(int_boot);
    lv_obj_set_size(pc_status_box, 130, 95);
    lv_obj_align(pc_status_box, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_set_style_bg_color(pc_status_box, lv_color_hex(0x1C1C1C), 0); 
    lv_obj_set_style_border_width(pc_status_box, 0, 0);
    lv_obj_set_style_radius(pc_status_box, 16, 0); 

    // 这里必须是对全局变量赋值！
    pc_icon = lv_image_create(pc_status_box); 
    lv_image_set_src(pc_icon, &computer);
    lv_obj_align(pc_icon, LV_ALIGN_CENTER, 0, -12); 
    lv_obj_set_style_image_recolor_opa(pc_icon, LV_OPA_COVER, 0); 
    lv_obj_set_style_image_recolor(pc_icon, lv_color_hex(0x555555), 0); 

    pc_label = lv_label_create(pc_status_box);
    lv_label_set_text(pc_label, "Status: OFF");
    lv_obj_set_style_text_color(pc_label, lv_color_hex(0x888888), 0);
    lv_obj_align(pc_label, LV_ALIGN_BOTTOM_MID, 0, -5);

    lv_obj_t * os_selector = lv_obj_create(int_boot);
    lv_obj_set_size(os_selector, 155, 42);
    lv_obj_align(os_selector, LV_ALIGN_TOP_RIGHT, -10, 40); 
    lv_obj_set_style_bg_color(os_selector, lv_color_hex(0x1C1C1C), 0); 
    lv_obj_set_style_border_width(os_selector, 0, 0);
    lv_obj_set_style_pad_all(os_selector, 4, 0);
    lv_obj_set_style_radius(os_selector, 20, 0); 
    lv_obj_set_flex_flow(os_selector, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(os_selector, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    btn_arch = lv_button_create(os_selector);
    lv_obj_set_size(btn_arch, 70, 34);
    lv_obj_set_style_bg_color(btn_arch, lv_color_hex(0x17A2B8), 0); 
    lv_obj_set_style_radius(btn_arch, 16, 0);
    label_arch = lv_label_create(btn_arch);
    lv_label_set_text(label_arch, "Arch");
    lv_obj_set_style_text_color(label_arch, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label_arch);
    lv_obj_add_event_cb(btn_arch, os_select_cb, LV_EVENT_CLICKED, NULL); 

    btn_win = lv_button_create(os_selector);
    lv_obj_set_size(btn_win, 70, 34);
    lv_obj_set_style_bg_color(btn_win, lv_color_hex(0x2D2D2D), 0);  
    lv_obj_set_style_radius(btn_win, 16, 0);
    label_win = lv_label_create(btn_win);
    lv_label_set_text(label_win, "Win");
    lv_obj_set_style_text_color(label_win, lv_color_hex(0x888888), 0); 
    lv_obj_center(label_win);
    lv_obj_add_event_cb(btn_win, os_select_cb, LV_EVENT_CLICKED, NULL); 

    btn_boot_res = lv_button_create(int_boot);
    lv_obj_set_size(btn_boot_res, 74, 45); 
    lv_obj_align(btn_boot_res, LV_ALIGN_BOTTOM_RIGHT, -88, -10); 
    lv_obj_set_style_radius(btn_boot_res, 12, 0);
    lbl_boot_res = lv_label_create(btn_boot_res);
    lv_label_set_text(lbl_boot_res, LV_SYMBOL_POWER " BOOT"); 
    lv_obj_center(lbl_boot_res);
    lv_obj_add_event_cb(btn_boot_res, btn_boot_res_cb, LV_EVENT_CLICKED, NULL);

    btn_force_off = lv_button_create(int_boot);
    lv_obj_set_size(btn_force_off, 74, 45);
    lv_obj_align(btn_force_off, LV_ALIGN_BOTTOM_RIGHT, -10, -10); 
    lv_obj_set_style_bg_color(btn_force_off, lv_color_hex(0xDC3545), 0); 
    lv_obj_set_style_radius(btn_force_off, 12, 0);
    lv_obj_t * lbl_force = lv_label_create(btn_force_off);
    lv_label_set_text(lbl_force, LV_SYMBOL_STOP " OFF");
    lv_obj_center(lbl_force);
    lv_obj_add_event_cb(btn_force_off, btn_force_off_cb, LV_EVENT_CLICKED, NULL);
}

static void init_linux_screen() {
    create_app_base(&scr_linux, &int_linux);

    lv_obj_t * linux_box = lv_obj_create(int_linux);
    lv_obj_set_size(linux_box, 130, 95);
    lv_obj_align(linux_box, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_set_style_bg_color(linux_box, lv_color_hex(0x1C1C1C), 0); 
    lv_obj_set_style_border_width(linux_box, 0, 0);
    
    lv_obj_t * lx_icon = lv_image_create(linux_box);
    lv_image_set_src(lx_icon, &linux_button); 
    lv_obj_align(lx_icon, LV_ALIGN_CENTER, 0, -10);
    lv_obj_t * lx_lbl = lv_label_create(linux_box);
    lv_label_set_text(lx_lbl, "SysRq Ctrl");
    lv_obj_set_style_text_color(lx_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lx_lbl, LV_ALIGN_BOTTOM_MID, 0, 5);

    lv_obj_t * btn_reisub = lv_button_create(int_linux);
    lv_obj_set_size(btn_reisub, 155, 60);
    lv_obj_align(btn_reisub, LV_ALIGN_RIGHT_MID, -10, 10); 
    lv_obj_set_style_bg_color(btn_reisub, lv_color_hex(0xDC3545), 0); 
    lv_obj_set_style_radius(btn_reisub, 12, 0);
    lv_obj_add_event_cb(btn_reisub, btn_reisub_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t * label_r = lv_label_create(btn_reisub);
    lv_label_set_text(label_r, LV_SYMBOL_WARNING " Safe REBOOT\n  ( R E I S U B )");
    lv_obj_set_style_text_align(label_r, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label_r);
}

static void mesh_tv_key_cb(lv_event_t * e) {
    lv_obj_t * tv = lv_event_get_target(e);
    uint32_t key = lv_event_get_key(e);
    lv_group_t * g = lv_obj_get_group(tv);
    
    if(lv_group_get_editing(g)) {
        if(key == LV_KEY_ENTER) {
            lv_group_set_editing(g, false); 
            return; 
        }
        lv_obj_t * act_tile = lv_tileview_get_tile_active(tv);
        if(!act_tile) return;
        
        int current_idx = lv_obj_get_index(act_tile);
        if(key == LV_KEY_RIGHT && current_idx == 0) {
            lv_obj_set_tile_id(tv, 1, 0, LV_ANIM_ON);
        } 
        else if(key == LV_KEY_LEFT && current_idx == 1) {
            lv_obj_set_tile_id(tv, 0, 0, LV_ANIM_ON);
        }
    } else {
        if(key == LV_KEY_ENTER) {
            lv_group_set_editing(g, true);
        }
    }
}

static void init_mesh_screen() {
    create_app_base(&scr_mesh, &int_mesh);

    lv_obj_t * mesh_tv = lv_tileview_create(int_mesh);
    lv_obj_set_size(mesh_tv, 320, 105);
    lv_obj_align(mesh_tv, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(mesh_tv, LV_OPA_TRANSP, 0);

    lv_obj_add_style(mesh_tv, &style_focus, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_color(mesh_tv, lv_color_hex(0xad494a), LV_STATE_FOCUSED | LV_STATE_EDITED);
    lv_obj_add_event_cb(mesh_tv, mesh_tv_key_cb, LV_EVENT_KEY, NULL);


    lv_obj_t * tile1 = lv_tileview_add_tile(mesh_tv, 0, 0, LV_DIR_HOR);
    lv_obj_t * node_card = lv_obj_create(tile1);
    lv_obj_set_size(node_card, 300, 90);
    lv_obj_center(node_card);
    lv_obj_set_style_bg_color(node_card, lv_color_hex(0x1C1C1C), 0);
    lv_obj_set_style_border_width(node_card, 0, 0);

    // ================= Node 0: mambo =================
    lbl_node_name[0] = lv_label_create(node_card);
    lv_label_set_text(lbl_node_name[0], LV_SYMBOL_BLUETOOTH " mambo");
    lv_obj_align(lbl_node_name[0], LV_ALIGN_TOP_LEFT, 0, 10);  // Y=10

    lbl_batt[0] = lv_label_create(node_card);
    lv_label_set_text(lbl_batt[0], LV_SYMBOL_BATTERY_FULL " --.-V"); 
    lv_obj_align(lbl_batt[0], LV_ALIGN_TOP_LEFT, 110, 10);     // X=110

    lbl_conn_time[0] = lv_label_create(node_card);
    lv_label_set_text(lbl_conn_time[0], LV_SYMBOL_WIFI " --:--");
    lv_obj_align(lbl_conn_time[0], LV_ALIGN_TOP_LEFT, 210, 10); // X=210

    // ================= Node 1: kang =================
    lbl_node_name[1] = lv_label_create(node_card);
    lv_label_set_text(lbl_node_name[1], LV_SYMBOL_BLUETOOTH " kang");
    lv_obj_align(lbl_node_name[1], LV_ALIGN_TOP_LEFT, 0, 35);  // Y=50 下移一行

    lbl_batt[1] = lv_label_create(node_card);
    lv_label_set_text(lbl_batt[1], LV_SYMBOL_BATTERY_FULL " --.-V"); 
    lv_obj_align(lbl_batt[1], LV_ALIGN_TOP_LEFT, 110, 35);

    lbl_conn_time[1] = lv_label_create(node_card);
    lv_label_set_text(lbl_conn_time[1], LV_SYMBOL_WIFI " --:--");
    lv_obj_align(lbl_conn_time[1], LV_ALIGN_TOP_LEFT, 210, 35);

    lv_obj_t * tile2 = lv_tileview_add_tile(mesh_tv, 1, 0, LV_DIR_HOR);
    lv_obj_t * term_bg = lv_obj_create(tile2);
    lv_obj_set_size(term_bg, 300, 90);
    lv_obj_center(term_bg);
    lv_obj_set_style_bg_color(term_bg, lv_color_hex(0x050505), 0);
    lv_obj_set_style_border_color(term_bg, lv_color_hex(0x444444), 0);
    
    lv_obj_t * log_txt = lv_label_create(term_bg);
    lv_label_set_text(log_txt, "[OK] Mesh Init\n[OK] TCP to Server\n[--] Waiting trigger...");
    lv_obj_set_style_text_color(log_txt, lv_color_hex(0x00FF00), 0); 
    lv_obj_align(log_txt, LV_ALIGN_TOP_LEFT, 0, 0);
}

static void wifi_setup_cb(lv_event_t * e) {
    if(!g_is_provisioning) {
        g_is_provisioning = true; 
        if(lbl_wifi) {
            lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI " Waiting...");
            lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(0xFF9800), 0);
        }
        trigger_smartconfig();
    } else {
        stop_smartconfig();
        if(lbl_wifi) {
            lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI "Network");
            lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(0xFFFFFF), 0);
        }
    }
}

static void init_settings_screen() {
    create_app_base(&scr_settings, &int_settings);

    lv_obj_t * lbl_bri = lv_label_create(int_settings);
    lv_label_set_text(lbl_bri, LV_SYMBOL_EYE_OPEN " Active Brightness");
    lv_obj_set_style_text_color(lbl_bri, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_bri, LV_ALIGN_TOP_LEFT, 167, 42);

    lv_obj_t * slider_bri = lv_slider_create(int_settings);
    lv_obj_set_size(slider_bri, 140, 10);
    lv_obj_align(slider_bri, LV_ALIGN_TOP_LEFT, 20, 45);
    lv_slider_set_value(slider_bri, current_active_bri, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider_bri, slider_active_bri_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider_bri, slider_save_nvs_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t * lbl_aod = lv_label_create(int_settings);
    lv_label_set_text(lbl_aod, LV_SYMBOL_EYE_CLOSE " AOD Brightness");
    lv_obj_set_style_text_color(lbl_aod, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_aod, LV_ALIGN_TOP_LEFT, 167, 72);

    lv_obj_t * slider_aod = lv_slider_create(int_settings);
    lv_obj_set_size(slider_aod, 140, 10);
    lv_obj_align(slider_aod, LV_ALIGN_TOP_LEFT, 20, 75);
    lv_slider_set_value(slider_aod, current_aod_bri, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider_aod, slider_aod_bri_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider_aod, slider_save_nvs_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t * btn_wifi = lv_button_create(int_settings);
    lv_obj_set_size(btn_wifi, 135, 35);
    lv_obj_align(btn_wifi, LV_ALIGN_BOTTOM_LEFT, 20, -10); 
    lv_obj_set_style_bg_color(btn_wifi, lv_color_hex(0x444444), 0);
    lv_obj_add_style(btn_wifi, &style_focus, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);

    lbl_wifi = lv_label_create(btn_wifi);
    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI " Network"); 
    lv_obj_center(lbl_wifi);
    lv_obj_add_event_cb(btn_wifi, wifi_setup_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * btn_flash = lv_button_create(int_settings);
    lv_obj_set_size(btn_flash, 135, 35);
    lv_obj_align(btn_flash, LV_ALIGN_BOTTOM_RIGHT, -20, -10); 
    lv_obj_set_style_bg_color(btn_flash, lv_color_hex(0x8B0000), 0); 
    lv_obj_add_style(btn_flash, &style_focus, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);

    lv_obj_t * label_flash = lv_label_create(btn_flash);
    lv_label_set_text(label_flash, LV_SYMBOL_DOWNLOAD " Flash");
    lv_obj_center(label_flash);
    lv_obj_add_event_cb(btn_flash, flash_btn_cb, LV_EVENT_CLICKED, NULL);
}

static void init_aod_screen() {
    scr_aod = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_aod, lv_color_hex(0x000000), 0); 
    create_status_bar(scr_aod);
    lv_obj_set_style_bg_opa(scr_aod, LV_OPA_COVER, 0);

    label_time = lv_label_create(scr_aod);
    lv_label_set_text(label_time, "--:--");
    lv_obj_set_style_text_font(label_time, &ka1, 0); 
    lv_obj_set_style_text_color(label_time, lv_color_hex(0xD0D0D0), 0);
    lv_obj_center(label_time); 
}

static void init_main_screen() {
    scr_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_main, lv_color_hex(0x111111), 0); 
    create_status_bar(scr_main);
    lv_obj_set_style_bg_opa(scr_main, LV_OPA_COVER, 0);

    lv_obj_t * scroll_cont = lv_obj_create(scr_main);
    lv_obj_set_size(scroll_cont, 320, 148); 
    lv_obj_align(scroll_cont, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(scroll_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll_cont, 0, 0);

    lv_obj_set_flex_flow(scroll_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(scroll_cont, 20, 0);
    lv_obj_set_style_pad_left(scroll_cont, 46, 0);
    lv_obj_set_style_pad_right(scroll_cont, 46, 0);
    lv_obj_set_scrollbar_mode(scroll_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(scroll_cont, LV_DIR_HOR);

    const char * titles[] = {"Boot", "Linux", "Mesh", "Set"};
    const void * img_srcs[] = {&power_button, &linux_button, &connection, &settings};

    for(int i = 0; i < 4; i++) {
        lv_obj_t * wrapper = lv_obj_create(scroll_cont);
        lv_obj_set_size(wrapper, 100, 130); 
        lv_obj_set_style_bg_opa(wrapper, LV_OPA_TRANSP, 0); 
        lv_obj_set_style_border_width(wrapper, 0, 0);       
        lv_obj_set_style_pad_all(wrapper, 0, 0);
        
        lv_obj_remove_flag(wrapper, LV_OBJ_FLAG_SCROLLABLE);   
        lv_obj_add_flag(wrapper, LV_OBJ_FLAG_OVERFLOW_VISIBLE); 

        lv_obj_t * btn = lv_button_create(wrapper); 
        lv_obj_set_size(btn, 100, 100); 
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 0); 
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A2A2A), 0);
        lv_obj_set_style_radius(btn, 22, 0); 

        lv_obj_add_style(btn, &style_focus, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
        
        lv_obj_t * img = lv_image_create(btn); 
        lv_image_set_src(img, img_srcs[i]);    
        lv_obj_center(img); 

        lv_obj_t * label = lv_label_create(wrapper);
        lv_label_set_text(label, titles[i]);
        lv_obj_set_style_text_color(label, lv_color_hex(0xAAAAAA), 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -5); 

        lv_obj_add_event_cb(btn, enter_app_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
    refresh_group_focus(scr_main); 
}

static void inactivity_monitor_task(lv_timer_t * timer) {
    uint32_t inactive_time = lv_display_get_inactive_time(NULL); 
    
    if(inactive_time > 10000 && lv_screen_active() != scr_aod && !g_is_provisioning) {
        lv_screen_load_anim(scr_aod, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, false);
        if(g_main) lv_group_remove_all_objs(g_main); 
        update_hw_backlight(current_aod_bri);
    }
    else if(inactive_time < 500 && lv_screen_active() == scr_aod) {
        lv_screen_load_anim(scr_main, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
        update_hw_backlight(current_active_bri);
        lv_indev_t * indev = lv_indev_get_next(NULL);
        if(indev) lv_indev_wait_release(indev);
        
        refresh_group_focus(scr_main);

        if(g_main) {
            lv_group_set_editing(g_main, false); 
        }
    }
}

static void init_custom_focus_style() {
    lv_style_init(&style_focus);
    lv_style_set_border_color(&style_focus, lv_color_hex(0x17A2B8)); 
    lv_style_set_border_width(&style_focus, 3); 
    lv_style_set_border_side(&style_focus, LV_BORDER_SIDE_FULL);
    lv_style_set_radius(&style_focus, 22); 
    lv_style_set_outline_width(&style_focus, 0);
}

void setup_grub_os_ui() {
    load_brightness_from_nvs();

    init_custom_focus_style();
    g_main = lv_group_create();
    lv_indev_t * indev = lv_indev_get_next(NULL);
    while(indev) {
        if(lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD || lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
            lv_indev_set_group(indev, g_main);
        }
        indev = lv_indev_get_next(indev);
    }

    init_aod_screen();
    init_main_screen();
    
    init_boot_screen();
    init_linux_screen();
    init_mesh_screen();
    init_settings_screen();
    
    lv_screen_load(scr_main);
    lv_timer_create(inactivity_monitor_task, 100, NULL);
    refresh_group_focus(scr_main);
    lv_timer_create(ui_state_update_task, 1000, NULL);

    ble_client_register_callbacks(ui_ble_connection_state_cb, ui_ble_command_result_cb, ui_ble_real_pc_state_cb);

    update_hw_backlight(current_active_bri);
}