#ifndef TICKS_TYPES_H
#define TICKS_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "constants.h"

// --- Misc types ---
typedef struct {
    uint64_t ms_since_epoch;
    uint64_t price;
    uint64_t volume;
} trade_data_t;
typedef uint8_t size_e;
enum {
    SIZE_8BIT = 1,
    SIZE_16BIT = 2,
    SIZE_32BIT = 4,
    SIZE_64BIT = 8
};

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
    char ticker[TICKS_TICKER_SIZE];
    char currency[TICKS_CURRENCY_SIZE];
    asset_class_e asset_class;
    char country[TICKS_COUNTRY_SIZE];
    compression_type_e compression_type;
    endian_e endianness;
} ticks_header_t;

// --- Index structures ---
typedef struct {
    uint64_t chunk_time_base;
    uint64_t chunk_offset;
    uint32_t chunk_size;
    size_e timestamp_size;
    size_e price_size;
    size_e volume_size;
} ticks_index_entry_t;
typedef struct {
    uint32_t num_entries;
    ticks_index_entry_t* entries;
} ticks_index_t;

// --- Chunk structures ---
typedef struct {
    uint64_t time_base;
    uint32_t num_records;
    size_e timestamp_size;
    size_e price_size;
    size_e volume_size;
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
    TICKS_ERROR_EMPTY_CHUNK = -7
} ticks_status_e;

// Opaque ticks file handle type
typedef struct ticks_file_t_internal ticks_file_t;

#endif // TICKS_TYPES_H
