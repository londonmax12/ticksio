#ifndef TICKSIO_INTERNAL_H
#define TICKSIO_INTERNAL_H

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

#include "ticksio/types.h"

// --- INTERNAL (HIDDEN) STRUCTURE DEFINITION ---
// This is defined ONLY here in the .c file.
// Its contents are invisible to external code.
struct ticks_file_t_internal {
    FILE *file_stream;  // The hidden file pointer
    ticks_header_t header; // The hidden file header (contains all data)
    uint64_t index_offset; // Byte offset where the index data starts in the file
    uint64_t index_size;   // Size of the index data in bytes
    ticks_index_t index;   // The in-memory index structure
};

#endif // TICKSIO_INTERNAL_H