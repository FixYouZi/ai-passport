// main/tesla_config.h
// 车辆配置：VIN / 车牌 / 车型，保存在 NVS。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define TESLA_VIN_LEN 17
#define TESLA_VIN_BUF 18
#define TESLA_PLATE_BUF 33
#define TESLA_MODEL_BUF 33

typedef struct {
    char vin[TESLA_VIN_BUF];
    char plate[TESLA_PLATE_BUF];
    char model[TESLA_MODEL_BUF];
} tesla_config_t;

esp_err_t tesla_config_load(tesla_config_t *out);
esp_err_t tesla_config_save(const tesla_config_t *config);
// 清空已保存的车辆信息；从未保存过时同样返回成功。
esp_err_t tesla_config_erase(void);
bool tesla_config_valid(const tesla_config_t *config);
bool tesla_vin_valid(const char *vin);
// 去空格/连字符、转大写并校验允许字符（不含 I/O/Q）。
bool tesla_config_normalize_vin(const char *input, char out[TESLA_VIN_BUF]);
