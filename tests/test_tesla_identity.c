#include "tesla_identity.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_vin_normalization(void)
{
    char normalized[TESLA_VIN_BUFFER_SIZE];

    assert(tesla_vin_normalize("5YJS0000000000000", normalized));
    assert(strcmp(normalized, "5YJS0000000000000") == 0);

    assert(tesla_vin_normalize(" 5yjs0000000000000\n", normalized));
    assert(strcmp(normalized, "5YJS0000000000000") == 0);

    assert(!tesla_vin_normalize("5YJS000000000000", normalized));
    assert(!tesla_vin_normalize("5YJS00000000000000", normalized));
    assert(!tesla_vin_normalize("5YJS00000000000I0", normalized));
    assert(!tesla_vin_normalize("5YJS00000000000O0", normalized));
    assert(!tesla_vin_normalize("5YJS00000000000Q0", normalized));
    assert(!tesla_vin_normalize(NULL, normalized));
    assert(!tesla_vin_normalize("5YJS0000000000000", NULL));
}

static void test_ble_name_format(void)
{
    const uint8_t digest[TESLA_SHA1_LENGTH] = {
        0x1a, 0x87, 0xa5, 0xa7, 0x5f, 0x3d, 0xf8, 0x58,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b
    };
    char name[TESLA_BLE_NAME_BUFFER_SIZE];

    assert(tesla_ble_name_from_digest(digest, name));
    assert(strcmp(name, "S1a87a5a75f3df858C") == 0);
    assert(strlen(name) == TESLA_BLE_NAME_LENGTH);
    assert(!tesla_ble_name_from_digest(NULL, name));
    assert(!tesla_ble_name_from_digest(digest, NULL));
}

int main(void)
{
    test_vin_normalization();
    test_ble_name_format();
    puts("tesla_identity: PASS");
    return 0;
}
