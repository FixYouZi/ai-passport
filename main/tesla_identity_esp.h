#pragma once

#include "tesla_identity.h"

#include "esp_err.h"

/* SHA-1 is used only to derive Tesla's public BLE advertisement identifier.
 * It must never be used for key derivation, authentication, or secret storage.
 */
esp_err_t tesla_ble_name_from_vin(const char *vin,
                                  char output[TESLA_BLE_NAME_BUFFER_SIZE]);
