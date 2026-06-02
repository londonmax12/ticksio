#include "ticksio/ticksio_index.h"

#include <stdlib.h>

#include "ticksio/ticksio_constants.h"
#include "ticksio/ticksio_helpers.h"
#include "ticksio/ticksio_platform.h"

ticks_status_e create_index(ticks_file_t* handle) {
    if (handle == NULL || handle->file_stream == NULL) {
        fprintf(stderr, "ERROR: Invalid handle in create_index\n");
        return TICKS_ERROR_INVALID_ARGUMENTS;
    }

    // Make sure index entries are allocated and initialized
    if (handle->index.entries == NULL || handle->index.num_entries == 0) {
        fprintf(stderr, "WARN: No index entries to write in create_index\n");
        return TICKS_ERROR_INVALID_FORMAT;
    }

    // Index size on disk = entry count * the packed (padding-free) entry size.
    handle->index_size = (uint64_t)handle->index.num_entries * TICKS_INDEX_ENTRY_DISK_SIZE;

    // Serialize all entries into a single little-endian buffer.
    uint8_t* buf = malloc(handle->index_size);
    if (buf == NULL) {
        perror("ERROR: Unable to allocate memory for index serialization\n");
        return TICKS_ERROR_MEMORY_ALLOCATION;
    }
    for (uint32_t i = 0; i < handle->index.num_entries; i++)
        serialize_index_entry(buf + (size_t)i * TICKS_INDEX_ENTRY_DISK_SIZE,
                              &handle->index.entries[i]);

    // Write index entries to file (64-bit safe seek).
    if (ticks_fseek64(handle->file_stream, (int64_t)handle->index_offset, SEEK_SET) != 0) {
        perror("ERROR: ticks_fseek64 to index_offset failed");
        free(buf);
        return TICKS_ERROR_FILE_IO;
    }
    if (fwrite(buf, 1, handle->index_size, handle->file_stream) != handle->index_size) {
        perror("ERROR: fwrite of index entries failed");
        free(buf);
        return TICKS_ERROR_FILE_IO;
    }
    free(buf);

    // Update index_size at its fixed header location (LE).
    uint8_t size_buf[sizeof(uint64_t)];
    le_put_u64(size_buf, handle->index_size);
    if (ticks_fseek64(handle->file_stream, TICKS_OFF_INDEX_SIZE, SEEK_SET) != 0) {
        perror("ERROR: ticks_fseek64 before index_size update failed");
        return TICKS_ERROR_FILE_IO;
    }
    if (fwrite(size_buf, 1, sizeof(size_buf), handle->file_stream) != sizeof(size_buf)) {
        perror("ERROR: fwrite (index_size update)");
        return TICKS_ERROR_FILE_IO;
    }

    return TICKS_OK;
}