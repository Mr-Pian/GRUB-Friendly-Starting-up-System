#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include "esp_log.h"
#include "esp_timer.h" // 【新增】：引入高精度定时器
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "ble_client.h"

static const char *TAG = "BLE_RADAR";

static bool g_assassin_mode = false;  
static bool g_is_connecting = false;
static uint8_t pending_target = 0; 
static uint8_t pending_cmd = 0;
static uint16_t active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t cmd_rx_handle = 0; 
static uint8_t cmd_retry_count = 0;

// 【新增】：雷达脉冲定时器
static esp_timer_handle_t radar_timer = NULL;

static ble_conn_state_cb_t g_conn_state_cb = NULL;
static ble_cmd_result_cb_t g_cmd_result_cb = NULL;
static ble_real_state_cb_t g_real_state_cb = NULL; 

static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0x80, 0x73, 0x3a, 0x85, 0x6c, 0xd0, 0xe4, 0xab, 0x4a, 0x4a, 0x8d, 0x06, 0xb1, 0x6e, 0x3c, 0x6d);
static const ble_uuid128_t cmd_rx_uuid = BLE_UUID128_INIT(
    0x71, 0x3b, 0x60, 0xed, 0xce, 0xed, 0xe2, 0xb9, 0xf1, 0x4a, 0xf2, 0xe3, 0xb4, 0x2d, 0x0c, 0x93);
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg);

void ble_client_register_callbacks(ble_conn_state_cb_t conn_cb, ble_cmd_result_cb_t cmd_cb, ble_real_state_cb_t state_cb) {
    g_conn_state_cb = conn_cb;
    g_cmd_result_cb = cmd_cb;
    g_real_state_cb = state_cb;
}

static void start_assassin_scan(void) {
    g_assassin_mode = true;
    g_is_connecting = false;
    struct ble_gap_disc_params disc_params = { 
        .filter_duplicates = 1, 
        .passive = 0,           
        .itvl = 160,            
        .window = 160,          
        .filter_policy = 0, .limited = 0 
    };
    ble_gap_disc_cancel(); 
    // 每次尝试扫描 4 秒
    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 4000, &disc_params, ble_gap_event_cb, NULL);
    ESP_LOGI(TAG, "Radar active. Hunting Target %d... (Retries left: %d)", pending_target, cmd_retry_count);
}

// ==========================================
// 【终极低功耗策略】：脉冲雷达触发器
// ==========================================
static void start_slow_radar(void) {
    // 如果正在连接，或者正在执行刺杀，直接跳过本次探测
    if (active_conn_handle != BLE_HS_CONN_HANDLE_NONE || g_is_connecting || g_assassin_mode) {
        return; 
    }
    
    struct ble_gap_disc_params disc_params = { 
        .filter_duplicates = 1, 
        .passive = 1,           
        .itvl = 160,    // 100ms
        .window = 144,  // 100ms (100% 满载倾听，绝不漏听)
        .filter_policy = 0, .limited = 0 
    };
    
    // 【物理学绝杀】：只听 1500 毫秒！因为 EFR32 每 1000 毫秒必发一次。
    // 如果 1.5 秒没听到，说明设备真没电了，直接结束，绝不恋战发热。
    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 1200, &disc_params, ble_gap_event_cb, NULL);
}

// 定时器回调函数：每 2 秒唤醒一次
static void radar_timer_cb(void* arg) {
    start_slow_radar();
}

static int on_write_complete_cb(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg) {
    bool success = (error->status == 0);
    if (g_cmd_result_cb) g_cmd_result_cb(pending_target, pending_cmd, success);
    if (success) {
        cmd_retry_count = 0;
    }
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static int char_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg) {
    if (error->status == 0 && ble_uuid_cmp(&chr->uuid.u, &cmd_rx_uuid.u) == 0) {
        cmd_rx_handle = chr->val_handle;
    } else if (error->status == BLE_HS_EDONE) {
        if (cmd_rx_handle != 0) {
            ble_gattc_write_flat(conn_handle, cmd_rx_handle, &pending_cmd, 1, on_write_complete_cb, NULL);
        } else {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    return 0;
}

static int svc_disc_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_svc *svc, void *arg) {
    if (error->status == 0 && ble_uuid_cmp(&svc->uuid.u, &svc_uuid.u) == 0) {
        ble_gattc_disc_all_chrs(conn_handle, svc->start_handle, svc->end_handle, char_disc_cb, NULL);
    }
    return 0;
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            struct ble_hs_adv_fields fields;
            ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);

            if (fields.mfg_data_len >= 6 && fields.mfg_data[0] == 0xFF && fields.mfg_data[1] == 0xFF) {
                
                uint8_t node_id = fields.mfg_data[2];
                uint8_t pc_state = fields.mfg_data[3];
                uint16_t batt_mv = fields.mfg_data[4] | (fields.mfg_data[5] << 8);

                // 同步状态给 UI
                if (g_conn_state_cb) g_conn_state_cb(node_id, true);
                if (g_real_state_cb) g_real_state_cb(node_id, (pc_state == 0), batt_mv);

                if (g_assassin_mode) {
                    // 【多节点修复 1】：必须判断抓到的广播是不是我们要刺杀的目标！
                    // 如果不加这个判断，它可能会错连到另一个正在发广播的节点。
                    if (node_id == pending_target) {
                        ESP_LOGI(TAG, "Target %d Locked! Executing Assassin Protocol...", node_id);
                        g_assassin_mode = false; 
                        g_is_connecting = true;
                        ble_gap_disc_cancel();  
                        ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &event->disc.addr, 3000, NULL, ble_gap_event_cb, NULL);
                    }
                } else {
                    // 【多节点修复 2】：删掉之前的 ble_gap_disc_cancel() ！！！
                    // 之前的代码为了省电，听到第一个广播就立刻关闭雷达。
                    // 可是现在有两个节点！如果你听到 mambo 就关闭雷达，kang 就永远“离线”了！
                    // 解决办法：什么都不做，让它安安静静地把这 1.5 秒的扫描窗口听完即可。
                }
            }
            break;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE:
            if (g_assassin_mode) {
                // 如果 4 秒内没扫到目标
                if (cmd_retry_count > 0) {
                    cmd_retry_count--;
                    ESP_LOGW(TAG, "Target %d missed. Retrying scan...", pending_target);
                    start_assassin_scan(); // 重新发起扫描
                } else {
                    ESP_LOGE(TAG, "Mission Failed: Target %d out of range.", pending_target);
                    g_assassin_mode = false;
                    if (g_cmd_result_cb) g_cmd_result_cb(pending_target, pending_cmd, false);
                }
            }
            break;
            
        case BLE_GAP_EVENT_CONNECT:
            g_is_connecting = false;
            if (event->connect.status == 0) {
                active_conn_handle = event->connect.conn_handle;
                ble_gattc_disc_svc_by_uuid(active_conn_handle, &svc_uuid.u, svc_disc_cb, NULL);
            } else {
                active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                ESP_LOGE(TAG, "Connection failed (status %d)", event->connect.status);
                // 如果连接瞬间因为信号弱断开了，立刻重试
                if (cmd_retry_count > 0) {
                    cmd_retry_count--;
                    start_assassin_scan();
                } else if (g_assassin_mode) {
                    g_assassin_mode = false;
                    if (g_cmd_result_cb) g_cmd_result_cb(pending_target, pending_cmd, false);
                }
            }
            break;
            
        case BLE_GAP_EVENT_DISCONNECT:
            active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            // 如果在服务发现或发送指令途中，突然信号丢失断开了
            if (cmd_retry_count > 0) {
                cmd_retry_count--;
                ESP_LOGW(TAG, "Connection dropped mid-flight! Retrying...");
                start_assassin_scan();
            }
            break;
    }
    return 0;
}

static void ble_client_on_sync(void) {
    ble_hs_util_ensure_addr(0);
    
    // 【新增】：底层协议栈准备好后，开启 2 秒一次的心跳定时器
    if (radar_timer == NULL) {
        esp_timer_create_args_t timer_args = {
            .callback = &radar_timer_cb,
            .name = "radar_timer"
        };
        esp_timer_create(&timer_args, &radar_timer);
        esp_timer_start_periodic(radar_timer, 2000000); // 2000000 微秒 = 2 秒
        ESP_LOGI(TAG, "Pulse Radar Timer Started (2s interval).");
    }
}

static void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_client_init(void) {
    nimble_port_init();
    ble_hs_cfg.sync_cb = ble_client_on_sync;
    nimble_port_freertos_init(ble_host_task);
}

void ble_trigger_pc_command(uint8_t target_node, uint8_t cmd) {
    if (active_conn_handle != BLE_HS_CONN_HANDLE_NONE || g_is_connecting) {
        ESP_LOGW(TAG, "Busy! Skipping...");
        return;
    }
    
    // 记录目标和指令
    pending_target = target_node;
    pending_cmd = cmd;
    
    // 【关键】：装填弹药！给它 3 次重试机会
    cmd_retry_count = 2; 
    
    // 呼叫扫描引擎开始干活
    start_assassin_scan();
}