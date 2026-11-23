#include "tesla_identity_esp.h"

#include "mbedtls/sha1.h"

esp_err_t tesla_ble_name_from_vin(const char *vin,
                                  char output[TESLA_BLE_NAME_BUFFER_SIZE])
{
    char normalized[TESLA_VIN_BUFFER_SIZE];
    uint8_t digest[TESLA_SHA1_LENGTH];

    if (!tesla_vin_normalize(vin, normalized) || !output) {
        return ESP_ERR_INVALID_ARG;
    }

    const int rc = mbedtls_sha1((const unsigned char *)normalized,
                               TESLA_VIN_LENGTH, digest);
    if (rc != 0) return ESP_FAIL;

    return tesla_ble_name_from_digest(digest, output) ? ESP_OK : ESP_FAIL;
}
