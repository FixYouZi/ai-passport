// main/tesla_web.h
// 设备热点 + 网页配置：录入 VIN / 车牌号 / 车型，保存到 NVS。
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "tesla_config.h"

typedef void (*tesla_web_saved_cb_t)(const tesla_config_t *config);

esp_err_t tesla_web_start(const tesla_config_t *current, tesla_web_saved_cb_t on_saved);
void tesla_web_stop(void);
bool tesla_web_running(void);
void tesla_web_get_ap(char ssid[33], char pass[33], char ip[16]);
