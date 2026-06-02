#ifndef TICKS_TYPES_H
#define TICKS_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "ticksio_constants.h"

// --- Misc types ---
// A trade tick (time-and-sales): one executed trade. This is the SCHEMA_TRADE
// record. NB: every record struct is a flat run of uint64 fields with the
// timestamp first, so it can be viewed generically as `const uint64_t*` with a
// stride of (number of columns). See TICKS_MAX_COLUMNS and the schema registry.
typedef struct {
    uint64_t ms_since_epoch;
    uint64_t price;
    uint64_t volume;
} trade_data_t;

// A quote tick (top-of-book / BBO): one change to the best bid or offer. This
// is the SCHEMA_QUOTE record. Same flat-uint64 layout rule as trade_data_t.
typedef struct {
    uint64_t ms_since_epoch;
    uint64_t bid;
    uint64_t ask;
    uint64_t bid_size;
    uint64_t ask_size;
} quote_data_t;

typedef uint8_t size_e;
enum {
    SIZE_8BIT = 1,
    SIZE_16BIT = 2,
    SIZE_32BIT = 4,
    SIZE_64BIT = 8
};

// Upper bound on columns per record, used to size fixed in-memory buffers.
// (SCHEMA_QUOTE, the widest schema today, uses 5.)
#define TICKS_MAX_COLUMNS 8

// How a column's per-record deltas are encoded on disk.
typedef uint8_t col_encoding_e;
enum {
    COL_ENC_DELTA_UNSIGNED = 0, // non-decreasing column (timestamps): raw unsigned delta
    COL_ENC_DELTA_ZIGZAG = 1    // signed column (prices, sizes): zig-zag-encoded delta
};

// Identifies the record schema stored in a file: how many columns each record
// has and what they mean. Column 0 is always the timestamp (ms since epoch).
typedef uint16_t schema_id_e;
enum {
    SCHEMA_TRADE = 0, // trade_data_t: {timestamp, price, volume}
    SCHEMA_QUOTE = 1  // quote_data_t: {timestamp, bid, ask, bid_size, ask_size}
};

// Describes one schema: its column count and the per-column encodings. Looked up
// by id via ticks_schema_lookup() (see ticksio_schema.h).
typedef struct {
    schema_id_e id;
    const char* name;
    uint8_t num_columns;
    const col_encoding_e* encodings; // length == num_columns; [0] must be unsigned (timestamp)
} ticks_schema_t;

// --- Header structures ---
typedef uint16_t compression_type_e;
enum {
    COMPRESSION_NONE = 0,
    COMPRESSION_ZSTD = 1,
    COMPRESSION_LZ4 = 2
};
typedef uint16_t asset_class_e;
enum {
    ASSET_CLASS_UNDEFINED = 0,
    ASSET_CLASS_STOCK = 1,
    ASSET_CLASS_OPTION = 2,
    ASSET_CLASS_FUTURE = 3,
    ASSET_CLASS_FOREX = 4,
    ASSET_CLASS_CRYPTO = 5
};
typedef uint8_t endian_e;
enum {
    ENDIAN_UNDEFINED = 0,
    ENDIAN_LITTLE = 1,
    ENDIAN_BIG = 2
};
typedef struct {
    uint16_t version;
    char ticker[TICKS_TICKER_SIZE];
    char currency[TICKS_CURRENCY_SIZE];
    asset_class_e asset_class;
    char country[TICKS_COUNTRY_SIZE];
    compression_type_e compression_type;
    endian_e endianness;
    // Record schema for the file (SCHEMA_TRADE, SCHEMA_QUOTE, …). Fixes the
    // number and meaning of the columns in every chunk. Set at ticks_new_file.
    schema_id_e schema_id;
    // Base-10 exponents giving the real-world value of the integer columns:
    // real_price = price * 10^price_scale (e.g. price_scale = -2 → prices are
    // stored in cents). Both default to 0 (stored values are the real values).
    // price_scale applies to all "price-like" columns (price, bid, ask);
    // volume_scale applies to all "size-like" columns (volume, bid_size, ask_size).
    int8_t price_scale;
    int8_t volume_scale;
    // File-level summary, maintained by the library (ignored on input to
    // ticks_new_file). Lets a catalog read just the header to learn how many
    // ticks the file holds and the time span they cover, without touching the
    // index. For an empty file all three are 0.
    uint64_t record_count;  // total number of ticks across all chunks
    uint64_t min_timestamp; // ms of the first tick (== first chunk's time base)
    uint64_t max_timestamp; // ms of the last tick (== last chunk's last timestamp)
} ticks_header_t;

// --- Index structures ---
// The index is a pure accelerator: just enough to locate a chunk, bound its
// time span for range pruning, and check its integrity. Per-column widths live
// in the self-describing chunk header (which is authoritative), so they are no
// longer duplicated here — that keeps the index entry fixed-size regardless of
// how many columns the file's schema has.
typedef struct {
    uint64_t chunk_time_base;      // ms of the first tick in the chunk
    uint64_t chunk_last_timestamp; // ms of the last tick in the chunk
    uint64_t chunk_offset;
    uint32_t chunk_size;
    uint32_t chunk_crc32; // CRC32 (IEEE) of the chunk's on-disk bytes
} ticks_index_entry_t;
typedef struct {
    uint32_t num_entries;
    ticks_index_entry_t* entries;
} ticks_index_t;

// --- Chunk structures ---
// A chunk holds `num_columns` columns (per the file's schema; column 0 is the
// timestamp). Each column is delta-encoded against the previous record: the
// first record's value is stored absolutely in bases[], and each subsequent
// record stores a delta of widths[] bytes, encoded per encs[]. The width is
// therefore governed by how much consecutive ticks *move*, not their magnitude.
typedef struct {
    uint64_t time_base;      // absolute ms of the first tick (== bases[0])
    uint64_t last_timestamp; // absolute ms of the last tick (index only; not on the chunk header)
    uint32_t num_records;
    uint8_t  num_columns;
    uint64_t bases[TICKS_MAX_COLUMNS];      // absolute value of each column's first record
    size_e   widths[TICKS_MAX_COLUMNS];     // per-record delta width of each column (1/2/4/8)
    col_encoding_e encs[TICKS_MAX_COLUMNS]; // encoding of each column's deltas
    uint8_t* data;
    uint32_t data_size;
} ticks_chunk_t;

/* 
* @brief Error codes for ticksio operations (0 = success, negative = error)
*/
typedef enum {
    TICKS_OK = 0,
    TICKS_EOF = -1,
    TICKS_ERROR_UNKNOWN = -2,
    TICKS_ERROR_INVALID_ARGUMENTS = -3,
    TICKS_ERROR_FILE_IO = -4,
    TICKS_ERROR_MEMORY_ALLOCATION = -5,
    TICKS_ERROR_INVALID_FORMAT = -6,
    TICKS_ERROR_EMPTY_CHUNK = -7,
    TICKS_ERROR_UNSORTED_DATA = -8,
    TICKS_ERROR_CORRUPT_DATA = -9,
    TICKS_ERROR_SCHEMA_MISMATCH = -10
} ticks_status_e;

// Opaque ticks file handle type
typedef struct ticks_file_t_internal ticks_file_t;
// Opaque ticks file iterator type
typedef struct ticks_iterator_t_internal ticks_iterator_t;

#endif // TICKS_TYPES_H
