#ifndef TICKS_CONSTANTS_H
#define TICKS_CONSTANTS_H

#ifdef __cplusplus
extern "C" {
#endif

// Internal constants for ticksio library

// --- Header constants ---
#define TICKS_MAGIC "TICK"
#define TICKS_MAGIC_SIZE 4
#define TICKS_VERSION 5
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
//   50     2    schema_id          (uint16, record schema: 0=trade, 1=quote)
//   52     4    reserved           (zero; room for forward-compatible fields)
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
#define TICKS_OFF_SCHEMA_ID     50
#define TICKS_OFF_RESERVED      52
#define TICKS_OFF_INDEX_OFFSET  56
#define TICKS_OFF_INDEX_SIZE    64
#define TICKS_HEADER_REGION_SIZE 72

// Each index entry on disk (little-endian, no padding). chunk_time_base and
// chunk_last_timestamp bound the chunk's time span so a range query can prune
// every chunk (including the last one) from the index alone, without decoding.
// Per-column widths are NOT stored here — they live in the self-describing
// chunk header (authoritative), which keeps this entry fixed-size for any schema.
//   0  8 chunk_time_base      (uint64)  ms of the first tick in the chunk
//   8  8 chunk_last_timestamp (uint64)  ms of the last tick in the chunk
//   16 8 chunk_offset         (uint64)
//   24 4 chunk_size           (uint32)
//   28 4 chunk_crc32          (uint32)
#define TICKS_INDEX_ENTRY_DISK_SIZE 32

// Each chunk begins with a self-describing header (little-endian, no padding)
// so a chunk can be decoded without consulting the index or the schema registry.
// The header is variable-length: a fixed prefix, then one descriptor per column,
// then the column data regions in column order (all of column 0's deltas, then
// all of column 1's, …). The first record of each column is stored absolutely
// in its descriptor's `base`; subsequent records are deltas of `width` bytes,
// encoded per `enc` (unsigned for the timestamp column, zig-zag for the rest).
//
//   prefix (TICKS_CHUNK_HEADER_BASE_SIZE bytes):
//     0  4 num_records  (uint32)
//     4  1 num_columns  (uint8)
//   then num_columns descriptors (TICKS_CHUNK_COL_DESC_SIZE bytes each):
//     +0 8 base   (uint64)  absolute value of the column's first record
//     +8 1 width  (uint8)   per-record delta width (1/2/4/8)
//     +9 1 enc    (uint8)   col_encoding_e
//   then the column data regions: column j is (num_records - 1) * width[j] bytes.
#define TICKS_CHUNK_HEADER_BASE_SIZE 5
#define TICKS_CHUNK_COL_DESC_SIZE 10
// Total chunk-header size for a record with `ncols` columns.
#define TICKS_CHUNK_HEADER_SIZE(ncols) \
    ((uint32_t)TICKS_CHUNK_HEADER_BASE_SIZE + (uint32_t)(ncols) * (uint32_t)TICKS_CHUNK_COL_DESC_SIZE)

// --- Chunking constants ---
#define MAX_CHUNK_SIZE 16777216 // 16 MB

// --- Compression constants (format v5+) ---
// When a file's compression_type names a codec (e.g. COMPRESSION_ZSTD), each
// chunk's self-describing columnar bytes are compressed and wrapped in a small
// frame on disk: a fixed prefix carrying the uncompressed payload size, followed
// by the codec's compressed payload.
//
//   0  4  uncompressed_size (uint32, LE)  bytes of the columnar payload once decoded
//   4  .. compressed payload (e.g. a single zstd frame)
//
// The index entry's chunk_size is this framed on-disk size and chunk_crc32 is
// taken over the framed bytes, so ticks_verify checks integrity WITHOUT
// decompressing. A file with COMPRESSION_NONE writes chunks verbatim (no frame)
// and is byte-identical to v4. The columnar chunk header (above) is unchanged.
#define TICKS_COMPRESS_FRAME_HEADER_SIZE 4
// ZSTD compression level used by the writer (1=fastest .. 19=smallest; 3 is the
// library default and a good speed/ratio balance for delta-encoded tick data).
#define TICKS_ZSTD_LEVEL 3

// --- CSV constants ---
#define CSV_MAX_TIMESTAMP_LEN 30
#define CSV_MAX_LINE_LEN 1024
// TODO: Make this adjustable based on available memory and remove this
#define CSV_DEFAULT_CHUNK_SIZE 10000

#endif // TICKS_CONSTANTS_H