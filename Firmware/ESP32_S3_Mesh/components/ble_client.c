#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "ble_client.h"

static const char *TAG = "BLE_SNIPER";

static bool g_assassin_mode = false;  
static uint8_t pending_target = 0; 
static uint8_t pending_cmd = 0;
static uint16_t active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t cmd_rx_handle = 0; 

static ble_conn_state_cb_t g_conn_state_cb = NULL;
static ble_cmd_result_cb_t g_cmd_result_cb = NULL;
static ble_real_state_cb_t g_real_state_cb = NULL; // 预留以后用

static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0x80, 0x73, 0x3a, 0x85, 0x6c, 0xd0, 0xe4, 0xab, 0x4a, 0x4a, 0x8d, 0x06, 0xb1, 0x6e, 0x3c, 0x6d);
static const ble_uuid128_t cmd_rx_uuid = BLE_UUID128_INIT(
    0x71, 0x3b, 0x60, 0xed, 0xce, 0xed, 0xe2, 0xb9, 0xf1, 0x4a, 0xf2, 0xe3, 0xb4, 0x2d, 0x0c, 0x93);

void ble_client_register_callbacks(ble_conn_state_cb_t conn_cb, ble_cmd_result_cb_t cmd_cb, ble_real_state_cb_t state_cb) {
    g_conn_state_cb = conn_cb;
    g_cmd_result_cb = cmd_cb;
    g_real_state_cb = state_cb;
}

static int on_write_complete_cb(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg) {
    bool success = (error->status == 0);
    if (g_cmd_result_cb) g_cmd_result_cb(pending_target, pending_cmd, success);
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
            if (!g_assassin_mode) break; // 不是刺杀模式，直接忽略包

            struct ble_hs_adv_fields fields;
            ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
            
            bool is_our_node = false;
            for (int i = 0; i < fields.num_uuids128; i++) {
                if (ble_uuid_cmp(&fields.uuids128[i].u, &svc_uuid.u) == 0) {
                    is_our_node = true; break;
                }
            }
            if (!is_our_node && fields.name != NULL) {
                if (strncmp((char *)fields.name, "Empty Example", fields.name_len) == 0) is_our_node = true;
            }

            if (is_our_node) {
                ESP_LOGI(TAG, "Target Locked! Connecting...");
                g_assassin_mode = false; // 找到目标，结束寻找
                ble_gap_disc_cancel();  
                ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &event->disc.addr, 3000, NULL, ble_gap_event_cb, NULL);
            }
            break;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE:
            // 【核心逻辑】：5秒扫描结束了。如果 g_assassin_mode 还是 true，说明根本没找到！
            if (g_assassin_mode) {
                ESP_LOGE(TAG, "Scan Timeout! Target is missing.");
                g_assassin_mode = false;
                // 告诉 UI 目标彻底失联了，熄灭蓝牙图标！
                if (g_conn_state_cb) g_conn_state_cb(pending_target, false);
            }
            break;
            
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                active_conn_handle = event->connect.conn_handle;
                // 连上了！告诉 UI 目标活着，点亮蓝牙图标！
                if (g_conn_state_cb) g_conn_state_cb(pending_target, true);
                ble_gattc_disc_svc_by_uuid(active_conn_handle, &svc_uuid.u, svc_disc_cb, NULL);
            } else {
                active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                // 连接意外失败，也视作不在线
                if (g_conn_state_cb) g_conn_state_cb(pending_target, false);
            }
            break;
            
        case BLE_GAP_EVENT_DISCONNECT:
            active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            // 【关键】：这里【不】熄灭图标！因为正常执行完指令后也会断开。
            // 只要我们不灭，图标就会常亮，直到下一次按按钮并且5秒找不到才会灭！
            break;
    }
    return 0;
}

static void ble_client_on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ESP_LOGI(TAG, "BLE Sniper Ready. Waiting for trigger...");
    // 启动时不扫描！彻底零耗电！
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
    if (active_conn_handle != BLE_HS_CONN_HANDLE_NONE || g_assassin_mode) {
        ESP_LOGW(TAG, "Busy! Skipping...");
        return;
    }
    
    pending_target = target_node;
    pending_cmd = cmd;
    g_assassin_mode = true; 

    // 【一击必杀】：开启 5 秒主动扫描。找不到就放弃。
    struct ble_gap_disc_params disc_params = { 
        .filter_duplicates = 1, 
        .passive = 0, // 主动扫描以获取 UUID
        .itvl = 160,            
        .window = 160,          
        .filter_policy = 0, .limited = 0 
    };
    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 5000, &disc_params, ble_gap_event_cb, NULL);
}