#include "ticksio/ticksio_helpers.h"

size_e determine_min_size_uint64(uint64_t value)
{
    if (value < UINT8_MAX) {
        return SIZE_8BIT;  // 1 byte
    } else if (value < UINT16_MAX) {
        return SIZE_16BIT; // 2 bytes
    } else if (value < UINT32_MAX) {
        return SIZE_32BIT; // 4 bytes
    } else {
        return SIZE_64BIT; // 8 bytes
    }
}

int is_little_endian() {
    int x = 1;
    char* y = (char*)&x;
    return (y[0] == 1);
}