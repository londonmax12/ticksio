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

// Rewrite the file-level summary (record_count, min/max timestamp) at its fixed
// header location. The three fields are contiguous on disk (see the layout in
// ticksio_constants.h), so they go out in a single write.
static ticks_status_e write_summary(struct ticks_file_t_internal* handle) {
    uint8_t buf[3 * sizeof(uint64_t)];
    le_put_u64(buf + 0, handle->header.record_count);
    le_put_u64(buf + 8, handle->header.min_timestamp);
    le_put_u64(buf + 16, handle->header.max_timestamp);
    if (ticks_fseek64(handle->file_stream, TICKS_OFF_RECORD_COUNT, SEEK_SET) != 0)
        return TICKS_ERROR_FILE_IO;
    if (fwrite(buf, 1, sizeof(buf), handle->file_stream) != sizeof(buf))
        return TICKS_ERROR_FILE_IO;
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

    // Update the file-level summary. min_timestamp is fixed by the very first
    // tick ever written; max_timestamp and record_count grow with each batch.
    if (!handle->has_data)
        handle->header.min_timestamp = data[0].ms_since_epoch;
    handle->header.max_timestamp = data[num_entries - 1].ms_since_epoch;
    handle->header.record_count += num_entries;
    ticks_status_e summary_result = write_summary(handle);
    if (summary_result != TICKS_OK)
        return summary_result;

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

    // Recompute the file-level summary as we go so it can be cross-checked
    // against the header's denormalized copy. record_count comes from each
    // chunk's own num_records field (offset 0 of the chunk header).
    uint64_t record_count = 0;

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
        record_count += le_get_u32(buf + 0);
    }

    free(buf);

    // Cross-check the header summary against the index/chunks. min/max come from
    // the time-ordered index extremes; record_count from the summed chunk headers.
    const uint64_t min_ts = handle->index.entries[0].chunk_time_base;
    const uint64_t max_ts = handle->index.entries[handle->index.num_entries - 1].chunk_last_timestamp;
    if (handle->header.record_count != record_count ||
        handle->header.min_timestamp != min_ts ||
        handle->header.max_timestamp != max_ts)
        return TICKS_ERROR_CORRUPT_DATA;

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
    // Stored tick timestamps are in milliseconds, but the public range is given
    // in seconds (validated against time(NULL) above). Convert once up front.
    iterator->from_ms = (uint64_t)from * 1000u;
    iterator->to_ms = (uint64_t)to * 1000u;
    iterator->current_chunk = 0;
    iterator->current_record_in_chunk = 0;
    iterator->chunk_loaded = 0;

    *out_iterator = iterator;

    return TICKS_OK;
}

// Read a little-endian unsigned value of the given width (1/2/4/8 bytes).
static uint64_t iter_read_uint(const uint8_t* p, size_e size) {
    switch (size) {
        case SIZE_8BIT:  return p[0];
        case SIZE_16BIT: return le_get_u16(p);
        case SIZE_32BIT: return le_get_u32(p);
        default:         return le_get_u64(p);
    }
}

// Load chunk `i` into the iterator's buffer and reset the decode cursor to its
// first record (cur_* = the chunk's absolute base values).
static ticks_status_e iter_load_chunk(ticks_iterator_t* it, uint32_t i) {
    const ticks_index_entry_t* e = &it->file_handle->index.entries[i];

    if (e->chunk_size < TICKS_CHUNK_HEADER_DISK_SIZE)
        return TICKS_ERROR_INVALID_FORMAT;

    if (it->chunk_buf == NULL || it->chunk_buf_cap < e->chunk_size) {
        uint8_t* nb = realloc(it->chunk_buf, e->chunk_size);
        if (nb == NULL)
            return TICKS_ERROR_MEMORY_ALLOCATION;
        it->chunk_buf = nb;
        it->chunk_buf_cap = e->chunk_size;
    }

    if (ticks_fseek64(it->file_handle->file_stream, (int64_t)e->chunk_offset, SEEK_SET) != 0)
        return TICKS_ERROR_FILE_IO;
    if (fread(it->chunk_buf, 1, e->chunk_size, it->file_handle->file_stream) != e->chunk_size)
        return TICKS_ERROR_FILE_IO;

    const uint8_t* b = it->chunk_buf;
    it->chunk_nrec = le_get_u32(b + 0);
    if (it->chunk_nrec == 0)
        return TICKS_ERROR_INVALID_FORMAT;
    it->cur_t = le_get_u64(b + 4);
    it->cur_p = le_get_u64(b + 12);
    it->cur_v = le_get_u64(b + 20);
    it->ts_w = (size_e)b[28];
    it->price_w = (size_e)b[29];
    it->volume_w = (size_e)b[30];
    it->ts_col = b + TICKS_CHUNK_HEADER_DISK_SIZE;
    it->price_col = it->ts_col + (uint64_t)(it->chunk_nrec - 1) * it->ts_w;
    it->volume_col = it->price_col + (uint64_t)(it->chunk_nrec - 1) * it->price_w;

    it->current_chunk = i;
    it->current_record_in_chunk = 0;
    it->chunk_loaded = 1;
    return TICKS_OK;
}

// Advance the decode cursor from record k to k+1, folding in delta k.
static void iter_advance_record(ticks_iterator_t* it) {
    const uint32_t k = it->current_record_in_chunk;
    if (k + 1 < it->chunk_nrec) {
        it->cur_t += iter_read_uint(it->ts_col + (uint64_t)k * it->ts_w, it->ts_w);
        it->cur_p = (uint64_t)((int64_t)it->cur_p +
            zigzag_decode(iter_read_uint(it->price_col + (uint64_t)k * it->price_w, it->price_w)));
        it->cur_v = (uint64_t)((int64_t)it->cur_v +
            zigzag_decode(iter_read_uint(it->volume_col + (uint64_t)k * it->volume_w, it->volume_w)));
    }
    it->current_record_in_chunk++;
}

ticks_status_e ticks_iterator_next(ticks_iterator_t* iterator, trade_data_t* out_record)
{
    if (iterator == NULL || out_record == NULL)
        return TICKS_ERROR_INVALID_ARGUMENTS;

    const ticks_index_t* idx = &iterator->file_handle->index;

    for (;;) {
        // Ensure a chunk with un-emitted records is loaded.
        if (!iterator->chunk_loaded ||
            iterator->current_record_in_chunk >= iterator->chunk_nrec) {

            uint32_t i = iterator->chunk_loaded ? iterator->current_chunk + 1 : 0;
            for (; i < idx->num_entries; i++) {
                const ticks_index_entry_t* e = &idx->entries[i];
                // Skip chunks lying entirely before the range. Uses last_timestamp,
                // so even the final chunk is pruned here without being decoded.
                if (e->chunk_last_timestamp < iterator->from_ms)
                    continue;
                // Chunks are time-ordered; once one starts at/after `to`, no later
                // chunk can qualify either.
                if (e->chunk_time_base >= iterator->to_ms)
                    return TICKS_EOF;
                break;
            }
            if (i >= idx->num_entries)
                return TICKS_EOF;

            ticks_status_e st = iter_load_chunk(iterator, i);
            if (st != TICKS_OK)
                return st;
        }

        // Emit the next qualifying record from the loaded chunk.
        while (iterator->current_record_in_chunk < iterator->chunk_nrec) {
            const uint64_t t = iterator->cur_t;
            const uint64_t p = iterator->cur_p;
            const uint64_t v = iterator->cur_v;

            // `to` is exclusive and the stream is sorted, so this ends iteration.
            if (t >= iterator->to_ms)
                return TICKS_EOF;

            const int emit = (t >= iterator->from_ms);
            iter_advance_record(iterator);

            if (emit) {
                out_record->ms_since_epoch = t;
                out_record->price = p;
                out_record->volume = v;
                return TICKS_OK;
            }
        }
        // Chunk exhausted without hitting `to`; loop to find the next chunk.
    }
}

ticks_status_e ticks_iterator_destroy(ticks_iterator_t *iterator)
{
    if (iterator == NULL)
        return TICKS_ERROR_INVALID_ARGUMENTS;

    free(iterator->chunk_buf);
    free(iterator);

    return TICKS_OK;
}