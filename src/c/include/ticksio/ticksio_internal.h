#ifndef TICKSIO_INTERNAL_H
#define TICKSIO_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

#include "ticksio/types.h"

enum file_mode_e {
    FILE_MODE_READ,
    FILE_MODE_WRITE
};

// --- INTERNAL STRUCTURE DEFINITION ---
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

#endif // TICKSIO_INTERNAL_H