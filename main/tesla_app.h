// main/tesla_app.h
// 卡片钥匙应用层：中文 LVGL 界面 + 三键交互 + BLE 命令调度。
#pragma once

#include "bsp_button.h"

void tesla_app_start(void);
void tesla_app_key(bsp_btn_t btn, bsp_btn_ev_t ev);
