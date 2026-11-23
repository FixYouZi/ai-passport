#include "tesla_identity.h"

#include <string.h>

static bool is_ascii_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static char ascii_upper(char c)
{
    if (c >= 'a' && c <= 'z') return (char)(c - ('a' - 'A'));
    return c;
}

static bool is_vin_character(char c)
{
    return (c >= 'A' && c <= 'H') ||
           (c >= 'J' && c <= 'N') ||
           (c >= 'P' && c <= 'R') ||
           (c >= 'S' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

bool tesla_vin_normalize(const char *input, char output[TESLA_VIN_BUFFER_SIZE])
{
    if (!input || !output) return false;

    const char *begin = input;
    while (*begin && is_ascii_space(*begin)) begin++;

    const char *end = begin + strlen(begin);
    while (end > begin && is_ascii_space(end[-1])) end--;

    if ((size_t)(end - begin) != TESLA_VIN_LENGTH) return false;

    for (size_t i = 0; i < TESLA_VIN_LENGTH; ++i) {
        const char c = ascii_upper(begin[i]);
        if (!is_vin_character(c)) return false;
        output[i] = c;
    }
    output[TESLA_VIN_LENGTH] = '\0';
    return true;
}

bool tesla_ble_name_from_digest(const uint8_t digest[TESLA_SHA1_LENGTH],
                                char output[TESLA_BLE_NAME_BUFFER_SIZE])
{
    static const char hex[] = "0123456789abcdef";
    if (!digest || !output) return false;

    output[0] = 'S';
    for (size_t i = 0; i < 8; ++i) {
        output[1 + i * 2] = hex[digest[i] >> 4];
        output[2 + i * 2] = hex[digest[i] & 0x0f];
    }
    output[17] = 'C';
    output[18] = '\0';
    return true;
}
