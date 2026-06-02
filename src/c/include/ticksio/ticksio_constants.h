#ifndef TICKS_CONSTANTS_H
#define TICKS_CONSTANTS_H

#ifdef __cplusplus
extern "C" {
#endif

// Internal constants for ticksio library

// --- Header constants ---
#define TICKS_MAGIC "TICK"
#define TICKS_MAGIC_SIZE 4
#define TICKS_VERSION 3
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
//   24     1    price_scale        (int8, base-10 exponent: real = stored * 10^scale)
//   25     1    volume_scale       (int8, base-10 exponent)
//   26     8    record_count       (uint64, total ticks in the file; 0 = empty)
//   34     8    min_timestamp      (uint64, ms of the first tick; 0 if empty)
//   42     8    max_timestamp      (uint64, ms of the last tick; 0 if empty)
//   50     6    reserved           (zero; room for forward-compatible fields)
//   56     8    index_offset       (uint64)
//   64     8    index_size         (uint64)
//   72     ...  chunk data ...
//
// record_count / min_timestamp / max_timestamp are a denormalized file-level
// summary: they let a catalog answer "how many ticks and what time span" from
// the fixed-size header alone, without seeking to and reading the index. They
// are maintained by the library on write and ignored on input to ticks_new_file.
#define TICKS_OFF_MAGIC         0
#define TICKS_OFF_VERSION       4
#define TICKS_OFF_ENDIANNESS    6
#define TICKS_OFF_TICKER        7
#define TICKS_OFF_CURRENCY      15
#define TICKS_OFF_COUNTRY       18
#define TICKS_OFF_ASSET_CLASS   20
#define TICKS_OFF_COMPRESSION   22
#define TICKS_OFF_PRICE_SCALE   24
#define TICKS_OFF_VOLUME_SCALE  25
#define TICKS_OFF_RECORD_COUNT  26
#define TICKS_OFF_MIN_TIMESTAMP 34
#define TICKS_OFF_MAX_TIMESTAMP 42
#define TICKS_OFF_RESERVED      50
#define TICKS_OFF_INDEX_OFFSET  56
#define TICKS_OFF_INDEX_SIZE    64
#define TICKS_HEADER_REGION_SIZE 72

// Each index entry on disk (little-endian, no padding). chunk_time_base and
// chunk_last_timestamp bound the chunk's time span so a range query can prune
// every chunk (including the last one) from the index alone, without decoding.
// The three column-width bytes record the *delta* widths of the chunk's columns
// and are redundant with the chunk's own header (which is authoritative); they
// are kept here so the index alone is enough for fast iteration and cross-checking.
//   0  8 chunk_time_base      (uint64)  ms of the first tick in the chunk
//   8  8 chunk_last_timestamp (uint64)  ms of the last tick in the chunk
//   16 8 chunk_offset         (uint64)
//   24 4 chunk_size           (uint32)
//   28 4 chunk_crc32          (uint32)
//   32 1 timestamp_size       (uint8)
//   33 1 price_size           (uint8)
//   34 1 volume_size          (uint8)
#define TICKS_INDEX_ENTRY_DISK_SIZE 35

// Each chunk begins with a self-describing header (little-endian, no padding)
// so a chunk can be decoded without consulting the index. Columns follow the
// header in struct-of-arrays order: all timestamp deltas, then all price
// deltas, then all volume deltas. The first record of each column is stored
// absolutely in the *_base fields; subsequent records are deltas from the
// previous record (timestamps unsigned; price/volume zig-zag-encoded signed).
//   0  4 num_records       (uint32)
//   4  8 ts_base           (uint64)  absolute ms of first tick (== chunk_time_base)
//   12 8 price_base        (uint64)  absolute price of first tick
//   20 8 volume_base       (uint64)  absolute volume of first tick
//   28 1 ts_delta_size     (uint8)   width of each timestamp delta (1/2/4/8)
//   29 1 price_delta_size  (uint8)   width of each zig-zag price delta
//   30 1 volume_delta_size (uint8)   width of each zig-zag volume delta
#define TICKS_CHUNK_HEADER_DISK_SIZE 31

// --- Chunking constants ---
#define MAX_CHUNK_SIZE 16777216 // 16 MB

// --- CSV constants ---
#define CSV_MAX_TIMESTAMP_LEN 30
#define CSV_MAX_LINE_LEN 1024
// TODO: Make this adjustable based on available memory and remove this
#define CSV_DEFAULT_CHUNK_SIZE 10000

#endif // TICKS_CONSTANTS_H