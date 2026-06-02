#ifndef TICKS_CONSTANTS_H
#define TICKS_CONSTANTS_H

#ifdef __cplusplus
extern "C" {
#endif

// Internal constants for ticksio library

// --- Header constants ---
#define TICKS_MAGIC "TICK"
#define TICKS_MAGIC_SIZE 4
#define TICKS_VERSION 1
#define TICKS_TICKER_SIZE 8
#define TICKS_CURRENCY_SIZE 3
#define TICKS_COUNTRY_SIZE 2

// --- On-disk layout ---
// All multi-byte values are stored little-endian at the fixed byte offsets
// below. Structs are NEVER written/read directly (no compiler padding leaks
// into the file), so the format is portable across compilers/architectures.
//
//   offset size field
//   0      4    magic "TICK"
//   4      2    version            (uint16)
//   6      1    endianness         (uint8, informational; on disk always LE)
//   7      8    ticker             (char[8])
//   15     3    currency           (char[3])
//   18     2    country            (char[2])
//   20     2    asset_class        (uint16)
//   22     2    compression_type   (uint16)
//   24     8    index_offset       (uint64)
//   32     8    index_size         (uint64)
//   40     ...  chunk data ...
#define TICKS_OFF_MAGIC         0
#define TICKS_OFF_VERSION       4
#define TICKS_OFF_ENDIANNESS    6
#define TICKS_OFF_TICKER        7
#define TICKS_OFF_CURRENCY      15
#define TICKS_OFF_COUNTRY       18
#define TICKS_OFF_ASSET_CLASS   20
#define TICKS_OFF_COMPRESSION   22
#define TICKS_OFF_INDEX_OFFSET  24
#define TICKS_OFF_INDEX_SIZE    32
#define TICKS_HEADER_REGION_SIZE 40

// Each index entry on disk (little-endian, no padding):
//   0  8 chunk_time_base (uint64)
//   8  8 chunk_offset    (uint64)
//   16 4 chunk_size      (uint32)
//   20 4 chunk_crc32     (uint32)
//   24 1 timestamp_size  (uint8)
//   25 1 price_size      (uint8)
//   26 1 volume_size     (uint8)
#define TICKS_INDEX_ENTRY_DISK_SIZE 27

// --- Chunking constants ---
#define MAX_CHUNK_SIZE 16777216 // 16 MB

// --- CSV constants ---
#define CSV_MAX_TIMESTAMP_LEN 30
#define CSV_MAX_LINE_LEN 1024
// TODO: Make this adjustable based on available memory and remove this
#define CSV_DEFAULT_CHUNK_SIZE 10000

#endif // TICKS_CONSTANTS_H