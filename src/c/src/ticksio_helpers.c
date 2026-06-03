#include "ticksio/ticksio_helpers.h"

#include <string.h>

#include "ticksio/ticksio_constants.h"

size_e determine_min_size_uint64(uint64_t value)
{
    // Bounds are inclusive: a value equal to UINT8_MAX (255) still fits in one
    // byte, so it must select SIZE_8BIT, not be bumped to the next width. Using
    // `<` here would over-size every delta that lands exactly on a boundary.
    if (value <= UINT8_MAX) {
        return SIZE_8BIT;  // 1 byte
    } else if (value <= UINT16_MAX) {
        return SIZE_16BIT; // 2 bytes
    } else if (value <= UINT32_MAX) {
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

// --- Zig-zag (de)coding ---
uint64_t zigzag_encode(int64_t value) {
    // Arithmetic right shift replicates the sign bit; cast to unsigned keeps the
    // shift well-defined. (value >> 63) is 0 for >= 0 and all-ones for < 0.
    return ((uint64_t)value << 1) ^ (uint64_t)(value >> 63);
}

int64_t zigzag_decode(uint64_t value) {
    return (int64_t)(value >> 1) ^ -(int64_t)(value & 1u);
}

// --- Little-endian primitives ---
void le_put_u16(uint8_t* buf, uint16_t value) {
    buf[0] = (uint8_t)(value & 0xFF);
    buf[1] = (uint8_t)((value >> 8) & 0xFF);
}

void le_put_u32(uint8_t* buf, uint32_t value) {
    buf[0] = (uint8_t)(value & 0xFF);
    buf[1] = (uint8_t)((value >> 8) & 0xFF);
    buf[2] = (uint8_t)((value >> 16) & 0xFF);
    buf[3] = (uint8_t)((value >> 24) & 0xFF);
}

void le_put_u64(uint8_t* buf, uint64_t value) {
    // Unrolled (vs. a byte loop) so the compiler can fuse this into a single
    // 64-bit store on little-endian targets.
    buf[0] = (uint8_t)(value);
    buf[1] = (uint8_t)(value >> 8);
    buf[2] = (uint8_t)(value >> 16);
    buf[3] = (uint8_t)(value >> 24);
    buf[4] = (uint8_t)(value >> 32);
    buf[5] = (uint8_t)(value >> 40);
    buf[6] = (uint8_t)(value >> 48);
    buf[7] = (uint8_t)(value >> 56);
}

uint16_t le_get_u16(const uint8_t* buf) {
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

uint32_t le_get_u32(const uint8_t* buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

uint64_t le_get_u64(const uint8_t* buf) {
    // Unrolled (vs. a byte loop) so the compiler can fuse this into a single
    // 64-bit load on little-endian targets.
    return  (uint64_t)buf[0]        | ((uint64_t)buf[1] << 8)  |
           ((uint64_t)buf[2] << 16) | ((uint64_t)buf[3] << 24) |
           ((uint64_t)buf[4] << 32) | ((uint64_t)buf[5] << 40) |
           ((uint64_t)buf[6] << 48) | ((uint64_t)buf[7] << 56);
}

// --- CRC32 (IEEE 802.3) ---
// Slice-by-8 table-driven CRC32 (reflected, polynomial 0xEDB88420, init/final
// 0xFFFFFFFF) — byte-for-byte identical to the classic bit-at-a-time loop, but
// it folds 8 input bytes per iteration through 8 precomputed tables instead of
// looping 8 times per byte. The CRC is recomputed over every chunk's on-disk
// bytes on each append, so this is a hot path for write/insert (and verify).
//
// The tables are derived from the same polynomial and reading the 4-byte words
// little-endian, so the result is independent of host byte order.
static uint32_t crc32_table[8][256];
static int crc32_table_ready = 0;

static void crc32_build_table(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88420u ^ (c >> 1)) : (c >> 1);
        crc32_table[0][n] = c;
    }
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = crc32_table[0][n];
        for (int k = 1; k < 8; k++) {
            c = crc32_table[0][c & 0xFFu] ^ (c >> 8);
            crc32_table[k][n] = c;
        }
    }
    crc32_table_ready = 1; // benign if two writers race: both compute the same bytes
}

uint32_t ticks_crc32(const uint8_t* data, size_t len) {
    if (!crc32_table_ready)
        crc32_build_table();

    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* p = data;

    // Consume 8 bytes per iteration through the slice-by-8 tables.
    while (len >= 8) {
        uint32_t lo = crc ^ ((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                             ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
        uint32_t hi = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                      ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        crc = crc32_table[7][lo & 0xFFu] ^
              crc32_table[6][(lo >> 8) & 0xFFu] ^
              crc32_table[5][(lo >> 16) & 0xFFu] ^
              crc32_table[4][(lo >> 24) & 0xFFu] ^
              crc32_table[3][hi & 0xFFu] ^
              crc32_table[2][(hi >> 8) & 0xFFu] ^
              crc32_table[1][(hi >> 16) & 0xFFu] ^
              crc32_table[0][(hi >> 24) & 0xFFu];
        p += 8;
        len -= 8;
    }
    // Tail: the classic table-driven byte-at-a-time step.
    while (len--)
        crc = crc32_table[0][(crc ^ *p++) & 0xFFu] ^ (crc >> 8);

    return ~crc;
}

// --- Header (de)serialization ---
void serialize_header(uint8_t* buf, const ticks_header_t* header,
                      uint64_t index_offset, uint64_t index_size) {
    memset(buf, 0, TICKS_HEADER_REGION_SIZE);
    memcpy(buf + TICKS_OFF_MAGIC, TICKS_MAGIC, TICKS_MAGIC_SIZE);
    le_put_u16(buf + TICKS_OFF_VERSION, header->version);
    buf[TICKS_OFF_ENDIANNESS] = (uint8_t)header->endianness;
    memcpy(buf + TICKS_OFF_TICKER, header->ticker, TICKS_TICKER_SIZE);
    memcpy(buf + TICKS_OFF_CURRENCY, header->currency, TICKS_CURRENCY_SIZE);
    memcpy(buf + TICKS_OFF_COUNTRY, header->country, TICKS_COUNTRY_SIZE);
    le_put_u16(buf + TICKS_OFF_ASSET_CLASS, header->asset_class);
    le_put_u16(buf + TICKS_OFF_COMPRESSION, header->compression_type);
    buf[TICKS_OFF_PRICE_SCALE] = (uint8_t)header->price_scale;
    buf[TICKS_OFF_VOLUME_SCALE] = (uint8_t)header->volume_scale;
    le_put_u16(buf + TICKS_OFF_SCHEMA_ID, header->schema_id);
    // File-level summary (0/0/0 for an empty file).
    le_put_u64(buf + TICKS_OFF_RECORD_COUNT, header->record_count);
    le_put_u64(buf + TICKS_OFF_MIN_TIMESTAMP, header->min_timestamp);
    le_put_u64(buf + TICKS_OFF_MAX_TIMESTAMP, header->max_timestamp);
    // Bytes [TICKS_OFF_RESERVED, TICKS_OFF_INDEX_OFFSET) stay zero (memset above).
    le_put_u64(buf + TICKS_OFF_INDEX_OFFSET, index_offset);
    le_put_u64(buf + TICKS_OFF_INDEX_SIZE, index_size);
}

void deserialize_header(const uint8_t* buf, ticks_header_t* header,
                        uint64_t* index_offset, uint64_t* index_size) {
    header->version = le_get_u16(buf + TICKS_OFF_VERSION);
    header->endianness = (endian_e)buf[TICKS_OFF_ENDIANNESS];
    memcpy(header->ticker, buf + TICKS_OFF_TICKER, TICKS_TICKER_SIZE);
    memcpy(header->currency, buf + TICKS_OFF_CURRENCY, TICKS_CURRENCY_SIZE);
    memcpy(header->country, buf + TICKS_OFF_COUNTRY, TICKS_COUNTRY_SIZE);
    header->asset_class = le_get_u16(buf + TICKS_OFF_ASSET_CLASS);
    header->compression_type = le_get_u16(buf + TICKS_OFF_COMPRESSION);
    header->price_scale = (int8_t)buf[TICKS_OFF_PRICE_SCALE];
    header->volume_scale = (int8_t)buf[TICKS_OFF_VOLUME_SCALE];
    header->schema_id = le_get_u16(buf + TICKS_OFF_SCHEMA_ID);
    header->record_count = le_get_u64(buf + TICKS_OFF_RECORD_COUNT);
    header->min_timestamp = le_get_u64(buf + TICKS_OFF_MIN_TIMESTAMP);
    header->max_timestamp = le_get_u64(buf + TICKS_OFF_MAX_TIMESTAMP);
    if (index_offset) *index_offset = le_get_u64(buf + TICKS_OFF_INDEX_OFFSET);
    if (index_size) *index_size = le_get_u64(buf + TICKS_OFF_INDEX_SIZE);
}

// --- Index entry (de)serialization ---
void serialize_index_entry(uint8_t* buf, const ticks_index_entry_t* entry) {
    le_put_u64(buf + 0, entry->chunk_time_base);
    le_put_u64(buf + 8, entry->chunk_last_timestamp);
    le_put_u64(buf + 16, entry->chunk_offset);
    le_put_u32(buf + 24, entry->chunk_size);
    le_put_u32(buf + 28, entry->chunk_crc32);
}

void deserialize_index_entry(const uint8_t* buf, ticks_index_entry_t* entry) {
    entry->chunk_time_base = le_get_u64(buf + 0);
    entry->chunk_last_timestamp = le_get_u64(buf + 8);
    entry->chunk_offset = le_get_u64(buf + 16);
    entry->chunk_size = le_get_u32(buf + 24);
    entry->chunk_crc32 = le_get_u32(buf + 28);
}