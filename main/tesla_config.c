// main/tesla_config.c
#include "tesla_config.h"

#include <ctype.h>
#include <string.h>

#include "nvs.h"

#define CONFIG_NAMESPACE "tesla_cfg"
#define KEY_VIN "vin"
#define KEY_PLATE "plate"
#define KEY_MODEL "model"

bool tesla_vin_valid(const char *vin)
{
    if (vin == NULL || strlen(vin) != TESLA_VIN_LEN) {
        return false;
    }
    for (size_t i = 0; i < TESLA_VIN_LEN; i++) {
        char c = vin[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return false;
        }
        if (c == 'I' || c == 'O' || c == 'Q') {
            return false;
        }
    }
    return true;
}

bool tesla_config_normalize_vin(const char *input, char out[TESLA_VIN_BUF])
{
    if (input == NULL || out == NULL) {
        return false;
    }
    size_t count = 0;
    for (const char *p = input; *p != '\0' && count < TESLA_VIN_LEN; p++) {
        if (*p == ' ' || *p == '\t' || *p == '-') {
            continue;
        }
        out[count++] = (char)toupper((unsigned char)*p);
    }
    out[count] = '\0';
    return tesla_vin_valid(out);
}

esp_err_t tesla_config_load(tesla_config_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t size = sizeof(out->vin);
    err = nvs_get_str(handle, KEY_VIN, out->vin, &size);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }
    size = sizeof(out->plate);
    if (nvs_get_str(handle, KEY_PLATE, out->plate, &size) != ESP_OK) {
        out->plate[0] = '\0';
    }
    size = sizeof(out->model);
    if (nvs_get_str(handle, KEY_MODEL, out->model, &size) != ESP_OK) {
        out->model[0] = '\0';
    }
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t tesla_config_save(const tesla_config_t *config)
{
    if (config == NULL || !tesla_config_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    if (nvs_set_str(handle, KEY_VIN, config->vin) != ESP_OK ||
        nvs_set_str(handle, KEY_PLATE, config->plate) != ESP_OK ||
        nvs_set_str(handle, KEY_MODEL, config->model) != ESP_OK) {
        err = ESP_FAIL;
    } else {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t tesla_config_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

bool tesla_config_valid(const tesla_config_t *config)
{
    if (config == NULL || !tesla_vin_valid(config->vin)) {
        return false;
    }
    if (strlen(config->plate) >= TESLA_PLATE_BUF || strlen(config->model) >= TESLA_MODEL_BUF) {
        return false;
    }
    return true;
}
