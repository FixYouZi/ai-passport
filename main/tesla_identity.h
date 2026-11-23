#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TESLA_VIN_LENGTH 17
#define TESLA_VIN_BUFFER_SIZE (TESLA_VIN_LENGTH + 1)
#define TESLA_SHA1_LENGTH 20
#define TESLA_BLE_NAME_LENGTH 18
#define TESLA_BLE_NAME_BUFFER_SIZE (TESLA_BLE_NAME_LENGTH + 1)

/*
 * Normalize a VIN by trimming surrounding ASCII whitespace and uppercasing
 * ASCII letters. Returns false unless the result is a valid 17-character VIN.
 * The output is always NUL-terminated when the function succeeds.
 */
bool tesla_vin_normalize(const char *input, char output[TESLA_VIN_BUFFER_SIZE]);

/*
 * Format the Tesla BLE advertised local name from a SHA-1 digest:
 * 'S' + lowercase hex(first 8 digest bytes) + 'C'.
 * This is an identification hint, not an authentication primitive.
 */
bool tesla_ble_name_from_digest(const uint8_t digest[TESLA_SHA1_LENGTH],
                                char output[TESLA_BLE_NAME_BUFFER_SIZE]);
