#include "ticksio.h"
#include <string.h>
#include <stdlib.h>

int ticks_create(const char *filename, const ticks_header_t *header) {
    FILE *f = fopen(filename, "wb");
    if (!f) return -1;

    // Write magic
    fwrite(TICKS_MAGIC, 1, 4, f);

    // Version
    uint16_t version = 1;
    fwrite(&version, sizeof(version), 1, f);

    // Endianness
    uint8_t endianness = 0; // little-endian for now
    fwrite(&endianness, sizeof(endianness), 1, f);

    // Write header fields
    fwrite(header->ticker, 1, TICKS_TICKER_SIZE, f);
    fwrite(header->currency, 1, TICKS_CURRENCY_SIZE, f);
    fwrite(&header->asset_class, sizeof(header->asset_class), 1, f);
    fwrite(header->country, 1, TICKS_COUNTRY_SIZE, f);
    fwrite(&header->compression_type, sizeof(header->compression_type), 1, f);

    // TODO: write index_offset placeholder (8 bytes)
    uint64_t zero = 0;
    fwrite(&zero, sizeof(zero), 1, f);

    fclose(f);
    return 0;
}

int ticks_open(const char *filename, FILE **out_file) {
    FILE *f = fopen(filename, "rb+");
    if (!f) return -1;
    *out_file = f;
    return 0;
}

int ticks_close(FILE *file) {
    return fclose(file);
}
