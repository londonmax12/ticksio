#ifndef TICKSIO_INTERNAL_H
#define TICKSIO_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>

#include "ticksio/ticksio_types.h"

enum file_mode_e {
    FILE_MODE_READ,
    FILE_MODE_WRITE
};

// --- Internal handle structure definition ---
struct ticks_file_t_internal {
    FILE *file_stream;  // The hidden file pointer
    ticks_header_t header; // The hidden file header
    uint64_t index_offset; // Byte offset where the index data starts in the file
    uint64_t index_size;   // Size of the index data in bytes
    ticks_index_t index;   // The in-memory index structure
    uint32_t index_capacity; // Allocated capacity of index.entries (for amortized growth)
    ticks_chunk_t* chunks; // The in-memory chunk structures
    uint32_t num_chunks;   // Number of chunks in the chunks array
    uint64_t last_timestamp; // Timestamp of the most recently written tick (ordering check)
    int has_data;          // Whether any tick has been written yet
    enum file_mode_e mode;    // File mode (read or write)
};

struct ticks_iterator_t_internal {
    ticks_file_t* file_handle;
    time_t from;          // inclusive range start (seconds since epoch)
    time_t to;            // exclusive range end (seconds since epoch)
    uint64_t from_ms;     // `from`/`to` in milliseconds, to match stored tick timestamps
    uint64_t to_ms;
    uint32_t current_chunk;            // index of the loaded chunk (valid when chunk_loaded)
    uint32_t current_record_in_chunk;  // next record to emit within the loaded chunk
    int chunk_loaded;                  // whether the decode state below is populated

    // Raw bytes and decode cursor for the currently loaded chunk. Records are
    // reconstructed sequentially: cur_* hold the running absolute values of the
    // record at current_record_in_chunk; the column pointers point into chunk_buf.
    uint8_t* chunk_buf;
    uint32_t chunk_buf_cap;
    uint32_t chunk_nrec;
    const uint8_t* ts_col;
    const uint8_t* price_col;
    const uint8_t* volume_col;
    size_e ts_w;
    size_e price_w;
    size_e volume_w;
    uint64_t cur_t;
    uint64_t cur_p;
    uint64_t cur_v;
};
#endif // TICKSIO_INTERNAL_H