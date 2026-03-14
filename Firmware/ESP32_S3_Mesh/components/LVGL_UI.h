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


#endif