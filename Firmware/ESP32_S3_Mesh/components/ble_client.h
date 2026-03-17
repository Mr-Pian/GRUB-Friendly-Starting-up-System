#ifndef BLE_CLIENT_H
#define BLE_CLIENT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// UI 注册的回调函数指针类型
typedef void (*ble_conn_state_cb_t)(uint8_t target_node, bool connected);
typedef void (*ble_cmd_result_cb_t)(uint8_t target_node, uint8_t cmd, bool success);
// 【新增】：接收真实物理状态的回调！
typedef void (*ble_real_state_cb_t)(uint8_t target_node, bool is_on); 

void ble_client_register_callbacks(ble_conn_state_cb_t conn_cb, ble_cmd_result_cb_t cmd_cb, ble_real_state_cb_t state_cb);

void ble_client_init(void);

// 触发刺杀指令
void ble_trigger_pc_command(uint8_t target_node, uint8_t cmd);

#ifdef __cplusplus
}
#endif

#endif