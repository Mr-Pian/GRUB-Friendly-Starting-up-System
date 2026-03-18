#ifndef LVGL_UI_H
#define LVGL_UI_H

#include <stdlib.h>
#include <stdio.h>
#ifdef _MSC_VER
  #include <Windows.h>
#else
  #include <unistd.h>
  #include <pthread.h>
#endif

#include "lvgl.h"

#include <time.h>
#include <sys/time.h>
#include "esp_timer.h" // 用于获取精准的 Uptime
#include "string.h"

void setup_grub_os_ui();
void ui_get_node_state(uint8_t node, bool *is_online, bool *pc_on, uint16_t *batt_mv);

#endif