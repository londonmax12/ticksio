#include "ticksio/ticksio.h"

#include "ticksio/ticksio_internal.h"
#include "ticksio/ticksio_chunks.h"
#include "ticksio/ticksio_index.h"
#include "ticksio/ticksio_helpers.h"
#include "ticksio/ticksio_constants.h"
#include "ticksio/ticksio_platform.h"

// Helper function to write the magic + header region using explicit
// little-endian serialization (no struct/padding written to disk).
static ticks_status_e write_initial_data(FILE *file, struct ticks_file_t_internal* handle) {
    // The chunk data begins immediately after the fixed-size header region.
    handle->index_offset = TICKS_HEADER_REGION_SIZE;
    handle->index_size = 0;

    uint8_t buf[TICKS_HEADER_REGION_SIZE];
    serialize_header(buf, &handle->header, handle->index_offset, handle->index_size);

    if (ticks_fseek64(file, 0, SEEK_SET) != 0)
        return TICKS_ERROR_FILE_IO;
    if (fwrite(buf, 1, TICKS_HEADER_REGION_SIZE, file) != TICKS_HEADER_REGION_SIZE)
        return TICKS_ERROR_FILE_IO;

    handle->index.num_entries = 0;
    handle->index.entries = NULL;
    handle->index_capacity = 0;

    return TICKS_OK;
}

// Helper function to read the index table
static ticks_status_e read_index_table(FILE *file, struct ticks_file_t_internal* handle) {
    if (!file || !handle || handle->index_offset == 0) {
        return TICKS_ERROR_INVALID_ARGUMENTS;
    }

    // Initialize index entries to NULL
    handle->index.entries = NULL;
    handle->index.num_entries = 0;
    handle->index_capacity = 0;

    if (handle->index_size == 0)
        return TICKS_ERROR_INVALID_FORMAT; // No entries to read

    // The index is a packed array of fixed-size, little-endian entries.
    if (handle->index_size % TICKS_INDEX_ENTRY_DISK_SIZE != 0)
        return TICKS_ERROR_INVALID_FORMAT;

    uint32_t num_entries = (uint32_t)(handle->index_size / TICKS_INDEX_ENTRY_DISK_SIZE);

    // Read the raw index bytes into a temporary buffer, then deserialize.
    uint8_t* raw = malloc(handle->index_size);
    if (raw == NULL)
        return TICKS_ERROR_MEMORY_ALLOCATION;

    handle->index.entries = malloc((size_t)num_entries * sizeof(ticks_index_entry_t));
    if (handle->index.entries == NULL) {
        free(raw);
        return TICKS_ERROR_MEMORY_ALLOCATION;
    }

    // Move file pointer to the index offset (64-bit safe)
    if (ticks_fseek64(file, (int64_t)handle->index_offset, SEEK_SET) != 0) {
        free(raw);
        free(handle->index.entries);
        handle->index.entries = NULL;
        return TICKS_ERROR_FILE_IO;
    }

    if (fread(raw, 1, handle->index_size, file) != handle->index_size) {
        free(raw);
        free(handle->index.entries);
        handle->index.entries = NULL;
        return TICKS_ERROR_FILE_IO;
    }

    for (uint32_t i = 0; i < num_entries; i++)
        deserialize_index_entry(raw + (size_t)i * TICKS_INDEX_ENTRY_DISK_SIZE,
                                &handle->index.entries[i]);

    handle->index.num_entries = num_entries;
    handle->index_capacity = num_entries;

    free(raw);
    return TICKS_OK;
}

// --- API Implementation ---
ticks_status_e ticks_new_file(const char* filename, ticks_header_t* header, ticks_file_t** out_handle) {
    if (filename == NULL || header == NULL) 
        return TICKS_ERROR_INVALID_ARGUMENTS;

    // Allocate memory for the internal handle structure and zero memory
    struct ticks_file_t_internal* handle = malloc(sizeof(struct ticks_file_t_internal));
    memset(handle, 0, sizeof(struct ticks_file_t_internal));

    if (handle == NULL) {
        printf("Failed to allocate memory: %s\n", strerror(errno));
        return TICKS_ERROR_MEMORY_ALLOCATION;
    }

    // Open the file for writing (binary mode)
    handle->file_stream = fopen(filename, "wb");
    if (handle->file_stream == NULL) {
        printf("Failed to open file: %s\n", strerror(errno));
        free(handle);
        return TICKS_ERROR_FILE_IO;
    }

    // Store a copy of the header internally. The on-disk byte order is always
    // little-endian (see serialize_header), so the recorded endianness reflects
    // that rather than the host's native order.
    handle->header.version = TICKS_VERSION;
    handle->header.endianness = ENDIAN_LITTLE;
    handle->header.asset_class = header->asset_class;
    strncpy(handle->header.ticker, header->ticker, TICKS_TICKER_SIZE);
    strncpy(handle->header.currency, header->currency, TICKS_CURRENCY_SIZE);
    strncpy(handle->header.country, header->country, TICKS_COUNTRY_SIZE);
    handle->header.compression_type = header->compression_type;
    handle->header.price_scale = header->price_scale;
    handle->header.volume_scale = header->volume_scale;

    // Write data to the file
    if (write_initial_data(handle->file_stream, (struct ticks_file_t_internal*)handle) != 0) {
        printf("Failed to write initial data: %s\n", strerror(errno));
        fclose(handle->file_stream);
        free(handle);
        return TICKS_ERROR_FILE_IO;
    }
 
    handle->mode = FILE_MODE_WRITE;
    *out_handle = (ticks_file_t*)handle;

    return TICKS_OK;
}

ticks_status_e ticks_open(const char* filename, const char* mode, ticks_file_t** out_handle) {
    if (filename == NULL)
       return TICKS_ERROR_INVALID_ARGUMENTS;

    // Allocate memory for the internal handle structure
    struct ticks_file_t_internal* handle = malloc(sizeof(struct ticks_file_t_internal));
    if (handle == NULL)
        return TICKS_ERROR_MEMORY_ALLOCATION;

    // Zero the handle so partially-initialized fields are well-defined.
    memset(handle, 0, sizeof(struct ticks_file_t_internal));

    // Open the file in specified mode
    handle->file_stream = fopen(filename, mode);
    if (handle->file_stream == NULL) {
        free(handle);
        return TICKS_ERROR_FILE_IO;
    }

    // Read the fixed-size header region (magic + header + index pointers) in one go.
    uint8_t region[TICKS_HEADER_REGION_SIZE];
    if (fread(region, 1, TICKS_HEADER_REGION_SIZE, handle->file_stream) != TICKS_HEADER_REGION_SIZE) {
        // File too short or read error
        fclose(handle->file_stream);
        free(handle);
        return TICKS_ERROR_INVALID_FORMAT;
    }

    // Validate the magic number.
    if (memcmp(region + TICKS_OFF_MAGIC, TICKS_MAGIC, TICKS_MAGIC_SIZE) != 0) {
        fclose(handle->file_stream);
        free(handle);
        return TICKS_ERROR_INVALID_FORMAT;
    }

    // Validate the format version.
    uint16_t version = le_get_u16(region + TICKS_OFF_VERSION);
    if (version == 0 || version > TICKS_VERSION) {
        fclose(handle->file_stream);
        free(handle);
        return TICKS_ERROR_INVALID_FORMAT;
    }

    // Deserialize the header fields and index pointers from the region buffer.
    deserialize_header(region, &handle->header, &handle->index_offset, &handle->index_size);

    // Read the Index Table into memory (absence of entries is not fatal).
    read_index_table(handle->file_stream, handle);

    *out_handle = (ticks_file_t*)handle;
    return TICKS_OK;
}

ticks_status_e ticks_open_read(const char* filename, ticks_file_t** out_handle) {
    ticks_file_t* handle = NULL;
    ticks_status_e open_status = ticks_open(filename, "rb", &handle);
    if (open_status != TICKS_OK) {
        return open_status;
    }

    handle->mode = FILE_MODE_READ;
    
    *out_handle = handle;

    return TICKS_OK;
}

ticks_status_e ticks_open_write(const char* filename, ticks_file_t** out_handle) {
    ticks_file_t* handle = NULL;
    ticks_status_e open_status = ticks_open(filename, "rb+", &handle);
    if (open_status != TICKS_OK) {
        return open_status;
    }

    handle->mode = FILE_MODE_WRITE;

    *out_handle = handle;

    return TICKS_OK;
}

ticks_status_e ticks_close(ticks_file_t *handle) {
    if (handle == NULL)
        return TICKS_ERROR_INVALID_ARGUMENTS;
    
    // Try to close the internal file stream if it's open
    int status = (handle->file_stream != NULL) ? fclose(handle->file_stream) : 0;
    if (status != 0) {
        // fclose failed, errno is set by fclose
        free(handle);
        return TICKS_ERROR_FILE_IO;
    }

    // Free index entries if allocated
    if (handle->index.entries != NULL)
        free(handle->index.entries);
    
    // Free the dynamically allocated handle structure
    free(handle);

    return TICKS_OK;
}

ticks_status_e ticks_get_header(ticks_file_t* handle, ticks_header_t* out_asset_class) {
    if (handle == NULL || out_asset_class == NULL)
        return TICKS_ERROR_INVALID_ARGUMENTS;

    *out_asset_class = handle->header;
    
    return TICKS_OK;
}

ticks_status_e ticks_get_index_offset(ticks_file_t *handle, uint64_t *out_offset) {
    if (handle == NULL || out_offset == NULL)
        return TICKS_ERROR_INVALID_ARGUMENTS;

    *out_offset = handle->index_offset;
    
    return TICKS_OK;
}

ticks_status_e ticks_get_index_size(ticks_file_t *handle, uint64_t *out_size) {
    if (handle == NULL || out_size == NULL)
        return TICKS_ERROR_INVALID_ARGUMENTS;

    *out_size = handle->index_size;
    
    return TICKS_OK;
}

ticks_status_e ticks_add_data(ticks_file_t* handle, trade_data_t* data, uint64_t num_entries) {
    if (handle == NULL || data == NULL || num_entries == 0 || handle->file_stream == NULL)
        return TICKS_ERROR_INVALID_ARGUMENTS;

    // Timestamps are delta-encoded against the previous tick as unsigned values,
    // so the data must be non-decreasing in time. Validate the batch (and its
    // continuity with previously written data) before writing anything.
    uint64_t prev = handle->has_data ? handle->last_timestamp : 0;
    for (uint64_t i = 0; i < num_entries; i++) {
        if ((handle->has_data || i > 0) && data[i].ms_since_epoch < prev)
            return TICKS_ERROR_UNSORTED_DATA;
        prev = data[i].ms_since_epoch;
    }

    // Create chunks from the provided data
    ticks_status_e create_chunks_result = create_chunks(handle, data, num_entries);
    if (create_chunks_result != TICKS_OK)
        return create_chunks_result;
  
    ticks_status_e create_index_result = create_index(handle);
    if (create_index_result != TICKS_OK)
        return create_index_result;

    // Record the high-water timestamp so subsequent calls stay ordered.
    handle->last_timestamp = data[num_entries - 1].ms_since_epoch;
    handle->has_data = 1;

    return TICKS_OK;
}

ticks_status_e ticks_verify(ticks_file_t* handle) {
    if (handle == NULL || handle->file_stream == NULL)
        return TICKS_ERROR_INVALID_ARGUMENTS;

    if (handle->index.num_entries == 0 || handle->index.entries == NULL)
        return TICKS_OK; // Nothing stored, nothing to corrupt.

    // Reuse a single buffer sized to the largest chunk to avoid per-chunk allocs.
    uint32_t max_chunk = 0;
    for (uint32_t i = 0; i < handle->index.num_entries; i++) {
        if (handle->index.entries[i].chunk_size > max_chunk)
            max_chunk = handle->index.entries[i].chunk_size;
    }
    if (max_chunk == 0)
        return TICKS_OK;

    uint8_t* buf = malloc(max_chunk);
    if (buf == NULL)
        return TICKS_ERROR_MEMORY_ALLOCATION;

    for (uint32_t i = 0; i < handle->index.num_entries; i++) {
        const ticks_index_entry_t* e = &handle->index.entries[i];

        if (ticks_fseek64(handle->file_stream, (int64_t)e->chunk_offset, SEEK_SET) != 0) {
            free(buf);
            return TICKS_ERROR_FILE_IO;
        }
        if (fread(buf, 1, e->chunk_size, handle->file_stream) != e->chunk_size) {
            free(buf);
            return TICKS_ERROR_FILE_IO;
        }
        if (ticks_crc32(buf, e->chunk_size) != e->chunk_crc32) {
            free(buf);
            return TICKS_ERROR_CORRUPT_DATA;
        }
    }

    free(buf);
    return TICKS_OK;
}

const char* ticks_status_to_string(ticks_status_e status)
{
    switch (status) {
        case TICKS_OK:
            return "Success";
        case TICKS_EOF:
            return "End of File";
        case TICKS_ERROR_UNKNOWN:
            return "Unknown Error";
        case TICKS_ERROR_INVALID_ARGUMENTS:
            return "Invalid Arguments";
        case TICKS_ERROR_FILE_IO:
            return "File I/O Error";    
        case TICKS_ERROR_MEMORY_ALLOCATION:
            return "Memory Allocation Error";
        case TICKS_ERROR_INVALID_FORMAT:
            return "Invalid Format";
        case TICKS_ERROR_EMPTY_CHUNK:
            return "Empty Chunk";
        case TICKS_ERROR_UNSORTED_DATA:
            return "Unsorted Data (timestamps must be non-decreasing)";
        case TICKS_ERROR_CORRUPT_DATA:
            return "Corrupt Data (checksum mismatch)";
        default:
            return "Unrecognized Status Code";   
    }
}


ticks_status_e ticks_iterator_create(ticks_file_t *handle, time_t from, time_t to, ticks_iterator_t** out_iterator)
{
    if (handle == NULL || out_iterator == NULL)
        return TICKS_ERROR_INVALID_ARGUMENTS;

    time_t now = time(NULL);
    if (from >= to || from < 0 || to <= 0 || from > now || to > now)
        return TICKS_ERROR_INVALID_ARGUMENTS;

    ticks_iterator_t* iterator = malloc(sizeof(ticks_iterator_t));
    if (iterator == NULL)
        return TICKS_ERROR_MEMORY_ALLOCATION;
    
    memset(iterator, 0, sizeof(ticks_iterator_t));
    iterator->file_handle = handle;
    iterator->from = from;
    iterator->to = to;
    iterator->current_chunk = 0;
    iterator->current_record_in_chunk = 0;

    *out_iterator = iterator;

    return TICKS_OK;
}

ticks_status_e ticks_iterator_destroy(ticks_iterator_t *iterator)
{
    if (iterator == NULL)
        return TICKS_ERROR_INVALID_ARGUMENTS;

    free(iterator);
    
    return TICKS_OK;
}