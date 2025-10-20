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
    ticks_chunk_t* chunks; // The in-memory chunk structures
    uint32_t num_chunks;   // Number of chunks in the chunks array
    enum file_mode_e mode;    // File mode (read or write)
};

struct ticks_iterator_t_internal {
    ticks_file_t* file_handle;
    time_t from;
    time_t to;
    uint32_t current_chunk;
    uint32_t current_record_in_chunk;
};
#endif // TICKSIO_INTERNAL_H