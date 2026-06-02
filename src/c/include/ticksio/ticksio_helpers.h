#ifndef TICKSIO_HELPERS_H
#define TICKSIO_HELPERS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#include "ticksio/ticksio_types.h"

size_e determine_min_size_uint64(uint64_t value);
int is_little_endian();

// --- Zig-zag (de)coding ---
// Map a signed delta onto an unsigned value whose magnitude tracks |delta|, so
// small positive *and* negative deltas pack into few bytes:
//   0 -> 0, -1 -> 1, 1 -> 2, -2 -> 3, ...
uint64_t zigzag_encode(int64_t value);
int64_t  zigzag_decode(uint64_t value);

// --- Little-endian (de)serialization primitives ---
// These read/write fixed-width values at an explicit byte offset, independent
// of the host's native endianness, so the on-disk format is portable.
void le_put_u16(uint8_t* buf, uint16_t value);
void le_put_u32(uint8_t* buf, uint32_t value);
void le_put_u64(uint8_t* buf, uint64_t value);
uint16_t le_get_u16(const uint8_t* buf);
uint32_t le_get_u32(const uint8_t* buf);
uint64_t le_get_u64(const uint8_t* buf);

// CRC32 (IEEE 802.3, polynomial 0xEDB88420) over `len` bytes.
uint32_t ticks_crc32(const uint8_t* data, size_t len);

// --- Record (de)serialization ---
// Serialize/deserialize the fixed-size header region (TICKS_HEADER_REGION_SIZE
// bytes including magic) and index entries (TICKS_INDEX_ENTRY_DISK_SIZE bytes).
void serialize_header(uint8_t* buf, const ticks_header_t* header,
                      uint64_t index_offset, uint64_t index_size);
void deserialize_header(const uint8_t* buf, ticks_header_t* header,
                        uint64_t* index_offset, uint64_t* index_size);
void serialize_index_entry(uint8_t* buf, const ticks_index_entry_t* entry);
void deserialize_index_entry(const uint8_t* buf, ticks_index_entry_t* entry);

#endif // TICKSIO_HELPERS_H