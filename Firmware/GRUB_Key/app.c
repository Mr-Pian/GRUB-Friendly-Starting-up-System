/***************************************************************************//**
 * @file
 * @brief Core application logic.
 *******************************************************************************
 * # License
 * <b>Copyright 2020 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * SPDX-License-Identifier: Zlib
 *
 * The licensor of this software is Silicon Laboratories Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *
 ******************************************************************************/
#include "em_common.h"
#include "app_assert.h"
#include "sl_bluetooth.h"
#include "app.h"
#include "em_cmu.h"
#include "em_gpio.h"
#include "app.h"
#include "em_cmu.h"
#include "em_gpio.h"
#include "gatt_db.h"      // 包含我们自定义的特征值 ID
#include "sl_udelay.h"    // 提供微秒级延时函数
#include "gpiointerrupt.h"
#include "em_iadc.h"
#include "app_timer.h"

// 定义 Simple Timer 的句柄
static app_timer_t battery_timer;
static app_timer_t long_press_timer;

// The advertising set handle allocated from Bluetooth stack.
static uint8_t advertising_set_handle = 0xff;
// 0xFF 代表当前没有设备连接
static uint8_t esp32_connection = 0xFF;

/**************************************************************************//**
 * Application Init.
 *****************************************************************************/
// 中断回调函数 (当 PC02 电平发生跳变时，硬件瞬间唤醒并跳到这里执行)
static void pc02_interrupt_callback(uint8_t intNo)
{
  if (intNo == 2) { // 确认是 PC02 (引脚号为2) 触发的中断
    // 每次 PC02 被拉低或拉高，翻转一下调试灯 PC05，方便肉眼验证
    //GPIO_PinOutToggle(gpioPortC, 5);

    // 同时向蓝牙内核发送一个编号为 1 的“外部唤醒信号”
    // (这是为我们下一小步通过蓝牙主动向 ESP32 推送状态做准备，蓝牙协议栈不能直接在中断里发数据)
    sl_bt_external_signal(1);
  }
}

// 长按时间到！松开开机键
static void long_press_callback(app_timer_t *timer, void *data)
{
  (void)timer;
  (void)data;
  // 模拟松开按键：把 PC04 拉低
  GPIO_PinOutClear(gpioPortC, 4);
}

// 初始化 IADC (配置为极其省电的单次测量模式)
static void init_battery_adc(void)
{
  IADC_Init_t init = IADC_INIT_DEFAULT;
  IADC_AllConfigs_t allConfigs = IADC_ALLCONFIGS_DEFAULT;
  IADC_InitSingle_t initSingle = IADC_INITSINGLE_DEFAULT;
  IADC_SingleInput_t singleInput = IADC_SINGLEINPUT_DEFAULT;

  // 开启 IADC 时钟
  CMU_ClockEnable(cmuClock_IADC0, true);
  init.srcClkPrescale = IADC_calcSrcClkPrescale(IADC0, 20000000, 0);

  // 配置内部 1.21V 参考电压
  allConfigs.configs[0].reference = iadcCfgReferenceInt1V2;
  allConfigs.configs[0].vRef = 1210;
  allConfigs.configs[0].analogGain = iadcCfgAnalogGain1x;

  // 核心：正极输入强行连到内部的 AVDD (电池端)，负极连 GND
  singleInput.posInput = iadcPosInputAvdd;
  singleInput.negInput = iadcNegInputGnd;

  IADC_init(IADC0, &init, &allConfigs);
  IADC_initSingle(IADC0, &initSingle, &singleInput);
}

// 触发一次测量，并返回毫伏值 (mV)
static uint16_t read_battery_mv(void)
{
  uint32_t total_sample = 0;
  // 采样 16 次 (强烈建议用 2 的幂次方，编译器会自动把除法优化成极其省电的位移运算)
  const uint8_t sample_count = 16;

  for (uint8_t i = 0; i < sample_count; i++) {
    // 启动一次 ADC 测量
    IADC_command(IADC0, iadcCmdStartSingle);

    // 等待转换结束并且 FIFO 数据有效
    while((IADC0->STATUS & (_IADC_STATUS_CONVERTING_MASK | _IADC_STATUS_SINGLEFIFODV_MASK)) != IADC_STATUS_SINGLEFIFODV);

    // 累加 12 位的原始采样数据
    total_sample += IADC_pullSingleFifoResult(IADC0).data;
  }

  // 求平均值
  uint32_t avg_sample = total_sample / sample_count;

  // 根据公式推算真实毫伏电压: V = avg_sample * 1.21 * 4 / 4095
  // 为了避免浮点运算消耗 CPU，全部转换为整数乘法
  uint16_t voltage_mv = (uint16_t)((avg_sample * 4840) / 4095);

  return voltage_mv;
}

// 6小时定时器回调 (因为是非中断上下文，可以直接随便调蓝牙 API)
static void battery_timer_callback(app_timer_t *timer, void *data)
{
  (void)timer;
  (void)data;

  // 【绝杀机制】：如果没有设备连接，我们先把射频广播停掉！
  if (esp32_connection == 0xFF) {
    sl_bt_advertiser_stop(advertising_set_handle);

    // 延时 5 毫秒 (5000微秒)
    // 射频关断后，给 CR2032 充足的时间让内部化学反应恢复，电压回升到绝对真实的空载值
    sl_udelay_wait(5000);
  }

  // 1. 测电压 (此时射频绝对是死寂状态，加上之前的 16 次过采样，测出来的值纯净无比)
  uint16_t batt_mv = read_battery_mv();
  uint8_t batt_data[2];
  batt_data[0] = (uint8_t)(batt_mv & 0xFF);
  batt_data[1] = (uint8_t)((batt_mv >> 8) & 0xFF);

  // 2. 如果 ESP32 连着，直接 Notify 推送给它
  if (esp32_connection != 0xFF) {
    sl_bt_gatt_server_send_notification(esp32_connection,
                                        gattdb_xgatt_batt_tx,
                                        2,
                                        batt_data);
  } else {
    // 3. 测完之后，如果没连接，别忘了把广播重新打开！否则 ESP32 就永远找不到它了
    sl_bt_legacy_advertiser_start(advertising_set_handle,
                                  sl_bt_legacy_advertiser_connectable);
  }

  // 4. 无论连不连着，都把最新电压写入本地 GATT 缓存
  sl_bt_gatt_server_write_attribute_value(gattdb_xgatt_batt_tx,
                                          0,
                                          2,
                                          batt_data);
}

void custom_gpio_init(void)
{
  CMU_ClockEnable(cmuClock_GPIO, true);

    GPIO_PinModeSet(gpioPortC, 4, gpioModePushPull, 0);
    GPIO_PinModeSet(gpioPortC, 3, gpioModePushPull, 0);
    GPIO_PinModeSet(gpioPortC, 2, gpioModeInput, 0);
    GPIO_PinModeSet(gpioPortC, 5, gpioModePushPull, 0);

    // --- 新增：初始化 GPIO 中断模块 ---
    GPIOINT_Init();

    // 配置外部中断：端口C, 引脚2, 对应中断号2
    // 参数 true, true, true 代表：开启上升沿触发、开启下降沿触发、立刻使能中断
    GPIO_ExtIntConfig(gpioPortC, 2, 2, true, true, true);

    // 注册我们的回调函数：将中断号2 与刚才写的函数绑定
    GPIOINT_CallbackRegister(2, pc02_interrupt_callback);

}

SL_WEAK void app_init(void)
{
  /////////////////////////////////////////////////////////////////////////////
  // Put your additional application init code here!                         //
  // This is called once during start-up.
  // --- 新增：上电冷启动防抖延时 ---
  // 给电池弹片 500 毫秒的时间完成物理摩擦和电压稳定，
  // 避开所有电源毛刺后再去初始化 GPIO 和 ADC。
  sl_udelay_wait(500000);
  custom_gpio_init();
  init_battery_adc();
  /////////////////////////////////////////////////////////////////////////////
}

/**************************************************************************//**
 * Application Process Action.
 *****************************************************************************/
SL_WEAK void app_process_action(void)
{
  /////////////////////////////////////////////////////////////////////////////
  // Put your additional application code here!                              //
  // This is called infinitely.                                              //
  // Do not call blocking functions from here!                               //
  /////////////////////////////////////////////////////////////////////////////
}

/**************************************************************************//**
 * Bluetooth stack event handler.
 * This overrides the dummy weak implementation.
 *
 * @param[in] evt Event coming from the Bluetooth stack.
 *****************************************************************************/
void sl_bt_on_event(sl_bt_msg_t *evt)
{
  sl_status_t sc;

  switch (SL_BT_MSG_ID(evt->header)) {
    // -------------------------------
    // This event indicates the device has started and the radio is ready.
    // Do not call any stack command before receiving this boot event!
    case sl_bt_evt_system_boot_id:
      // 【新增极限省电】：限制最大蓝牙发射功率为 -3 dBm (大约 0.5mW)
            // 这将极大降低射频发射瞬间对 CR2032 的电流冲击
            int16_t set_power_min, set_power_max;
            sl_bt_system_set_tx_power(-30, -30, &set_power_min, &set_power_max); // -30 代表 -3.0 dBm

            // 创建广播句柄
            sc = sl_bt_advertiser_create_set(&advertising_set_handle);
            app_assert_status(sc);

            // 生成广播数据
            sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                                       sl_bt_advertiser_general_discoverable);
            app_assert_status(sc);

            // 【核心优化】：把广播间隔从 100ms 爆改到 1000ms (1秒)
            // 1600 * 0.625ms = 1000ms
            sc = sl_bt_advertiser_set_timing(
              advertising_set_handle,
              1600, // min. adv. interval
              1600, // max. adv. interval
              0,    // adv. duration (0 = 无限广播)
              0);   // max. num. adv. events (0 = 无限次数)
            app_assert_status(sc);

            // 启动广播
            sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                               sl_bt_legacy_advertiser_connectable);
            app_assert_status(sc);

            // --- 启动 6 小时 (21600000 毫秒) 循环定时器 ---
                  // (测试时建议先写 5000 测 5 秒)
                  app_timer_start(&battery_timer,
                                        21600000,
                                        battery_timer_callback,
                                        NULL,
                                        true);
      break;

    // -------------------------------
    // This event indicates that a new connection was opened.
    case sl_bt_evt_connection_opened_id:
      esp32_connection = evt->data.evt_connection_opened.connection; //记录当前ID
      break;

    // -------------------------------
    // This event indicates that a connection was closed.
    case sl_bt_evt_connection_closed_id:
      esp32_connection = 0xFF; //重置ID
      // Generate data for advertising
      sc = sl_bt_legacy_advertiser_generate_data(advertising_set_handle,
                                                 sl_bt_advertiser_general_discoverable);
      app_assert_status(sc);

      // Restart advertising after client has disconnected.
      sc = sl_bt_legacy_advertiser_start(advertising_set_handle,
                                         sl_bt_legacy_advertiser_connectable);
      app_assert_status(sc);
      break;

    ///////////////////////////////////////////////////////////////////////////
    // Add additional event handlers here as your application requires!      //
    ///////////////////////////////////////////////////////////////////////////
   // ESP32写入事件
    case sl_bt_evt_gatt_server_attribute_value_id:
          // 1. 判断是不是写到了我们的 "Command RX" 信箱
          if (evt->data.evt_gatt_server_attribute_value.attribute == gattdb_xgatt_cmd_rx) {

            // 2. 提取收到的第一个字节 (因为我们之前设置长度为 1 byte)
            uint8_t command = evt->data.evt_gatt_server_attribute_value.value.data[0];

            // 3. 执行动作：0x01 开机，0x02 复位
            if (command == 0x01) {
              // 模拟按下开机键：PC04 拉高驱动光耦，延时 200 毫秒，再拉低
              GPIO_PinOutSet(gpioPortC, 4);
              sl_udelay_wait(300000);  // 200,000 微秒 = 300 毫秒脉冲
              GPIO_PinOutClear(gpioPortC, 4);
            }
            else if (command == 0x02) {
              // 模拟按下复位键：PC03 拉高驱动光耦，延时 300 毫秒，再拉低
              GPIO_PinOutSet(gpioPortC, 3);
              sl_udelay_wait(300000);
              GPIO_PinOutClear(gpioPortC, 3);
            }
            else if (command == 0x03) {
                          // 1. 模拟按下按键不松手：PC04 拉高
                          GPIO_PinOutSet(gpioPortC, 4);

                          // 2. 开启一个 5 秒 (5000 毫秒) 的单次定时器
                          // 最后一个参数 false 代表只执行一次 (Single-shot)
                          app_timer_start(&long_press_timer,
                                          5000,
                                          long_press_callback,
                                          NULL,
                                          false);
                        }
          }
      break;

      case sl_bt_evt_system_external_signal_id:
            // 判断是不是我们刚才定义的 1 号信号 (PC02 触发的)
            if (evt->data.evt_system_external_signal.extsignals == 1) {

              // 1. 确保 ESP32 (或手机) 当前是连着蓝牙的
              if (esp32_connection != 0xFF) {

                // 2. 读取 PC02 的当前电平 (0 或 1)
                uint8_t pc02_state = (uint8_t)GPIO_PinInGet(gpioPortC, 2);

                // 3. 把状态通过 Status TX 信箱 Notify 出去
                sl_bt_gatt_server_send_notification(
                  esp32_connection,
                  gattdb_xgatt_status_tx, // 你之前在 GATT 里配好的特征值 ID
                  1,                      // 发送长度: 1 byte
                  &pc02_state             // 发送的数据指针
                );
              }
            }
       break;

       // ---------------------------------------------------------
       // 核心时序修复：当客户端 (手机/ESP32) 开启 Notify 订阅时，立刻推送一次电量
       // ---------------------------------------------------------
       case sl_bt_evt_gatt_server_characteristic_status_id:
         // 1. 检查是不是我们的 Battery TX 信箱发生了状态改变
         if (evt->data.evt_gatt_server_characteristic_status.characteristic == gattdb_xgatt_batt_tx) {

           // 2. 检查改变的原因是不是“客户端修改了配置 (Client Config)”
           if (evt->data.evt_gatt_server_characteristic_status.status_flags == sl_bt_gatt_server_client_config) {

             // 3. 检查客户端是不是开启了 Notification (值为 1)
             if (evt->data.evt_gatt_server_characteristic_status.client_config_flags == sl_bt_gatt_server_notification) {

               // --- 完美时机：此时客户端已经张开嘴准备好接收了！ ---
               uint16_t batt_mv = read_battery_mv();
               uint8_t batt_data[2];
               batt_data[0] = (uint8_t)(batt_mv & 0xFF);
               batt_data[1] = (uint8_t)((batt_mv >> 8) & 0xFF);

               // 准确无误地推送给它
               sl_bt_gatt_server_send_notification(
                 esp32_connection,
                 gattdb_xgatt_batt_tx,
                 2,
                 batt_data
               );
             }
           }
         }
         break;

    // -------------------------------
    // Default event handler.
    default:
      break;
  }
}
