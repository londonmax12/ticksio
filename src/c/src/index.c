#include "ticksio/index.h"

ticks_status_e create_index(ticks_file_t* handle) {
    if (handle == NULL || handle->file_stream == NULL) {
        perror("ERROR: Invalid handle in create_index\n");
        return TICKS_ERROR_INVALID_ARGUMENTS;
    }

    // Make sure index entries are allocated and initialized
    if (handle->index.entries == NULL || handle->index.num_entries == 0) {
        perror("WARN: No index entries to write in create_index\n");
        return TICKS_ERROR_INVALID_FORMAT;
    }

    // Calculate index size
    handle->index_size = handle->index.num_entries * sizeof(ticks_index_entry_t);

    // Write index entries to file
    if (_fseeki64(handle->file_stream, handle->index_offset, SEEK_SET) != 0) {
        perror("ERROR: _fseeki64 to index_offset failed");
        return TICKS_ERROR_FILE_IO;
    }
    if (fwrite(handle->index.entries, 1, handle->index_size, handle->file_stream) != handle->index_size) {
        perror("ERROR: fwrite of index entries failed");
        return TICKS_ERROR_FILE_IO;
    }

    // Update index_size in file header
    if (_fseeki64(handle->file_stream, 4 + sizeof(ticks_header_t) + sizeof(uint64_t), SEEK_SET) != 0) {
        perror("ERROR: _fseeki64 before index_size update failed");
        return TICKS_ERROR_FILE_IO;
    }
    if (fwrite(&handle->index_size, 1, sizeof(uint64_t), handle->file_stream) != sizeof(uint64_t)) {
        perror("ERROR: fwrite (index_size update)");
        return TICKS_ERROR_FILE_IO;
    }

    return TICKS_OK;
}