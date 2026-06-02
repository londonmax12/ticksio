#include "ticksio/ticksio_chunks.h"

#include <string.h>

#include "ticksio/ticksio_types.h"
#include "ticksio/ticksio_internal.h"
#include "ticksio/ticksio_constants.h"
#include "ticksio/ticksio_helpers.h"
#include "ticksio/ticksio_platform.h"

// Helper function to write data of a specific size to a buffer
static void write_data(uint8_t** buffer, uint64_t value, size_e size) {
    switch (size) {
        case SIZE_8BIT: {
            uint8_t val = (uint8_t)value;
            memcpy(*buffer, &val, sizeof(val));
            *buffer += sizeof(val);
            break;
        }
        case SIZE_16BIT: {
            uint16_t val = (uint16_t)value;
            memcpy(*buffer, &val, sizeof(val));
            *buffer += sizeof(val);
            break;
        }
        case SIZE_32BIT: {
            uint32_t val = (uint32_t)value;
            memcpy(*buffer, &val, sizeof(val));
            *buffer += sizeof(val);
            break;
        }
        case SIZE_64BIT: {
            uint64_t val = value;
            memcpy(*buffer, &val, sizeof(val));
            *buffer += sizeof(val);
            break;
        }
    }
}

typedef struct {
    ticks_chunk_t* chunk;
    ticks_status_e status;
} create_chunk_result;

static size_e max_size(size_e a, size_e b) {
    return a > b ? a : b;
}

static create_chunk_result create_chunk(uint64_t* const row_index, const trade_data_t* entries, uint64_t num_entries) {
    if (*row_index >= num_entries) {
        perror("ERROR: row_index out of bounds in create_chunk\n");
        return (create_chunk_result){.chunk = NULL, .status = TICKS_ERROR_INVALID_ARGUMENTS};
    }

    ticks_chunk_t* chunk = malloc(sizeof(ticks_chunk_t));
    if (chunk == NULL) {
        perror("ERROR: Unable to allocate memory for chunk structure\n");
        return (create_chunk_result){.chunk = NULL, .status = TICKS_ERROR_MEMORY_ALLOCATION};
    }

    chunk->data = malloc(MAX_CHUNK_SIZE);
    if (chunk->data == NULL) {
        free(chunk);
        perror("ERROR: Unable to allocate memory for chunk data\n");
        return (create_chunk_result){.chunk = NULL, .status = TICKS_ERROR_MEMORY_ALLOCATION};
    }

    // Each column is delta-encoded against the previous record, so the chunk's
    // column widths are governed by how much consecutive ticks *move*, not by
    // their absolute magnitude. The first record is captured absolutely in the
    // *_base fields below.
    //
    // Pass 1: "Dry run" to determine the per-column delta widths and how many
    //         records fit in the chunk's MAX_CHUNK_SIZE budget.
    // Pass 2: "Serialization" — write the self-describing chunk header followed
    //         by the three delta columns in struct-of-arrays order.

    const uint64_t start = *row_index;
    chunk->time_base = entries[start].ms_since_epoch;
    chunk->price_base = entries[start].price;
    chunk->volume_base = entries[start].volume;
    chunk->num_records = 1; // the base record is always included

    // A delta needs at least one byte, so widths start at the 1-byte minimum.
    chunk->timestamp_size = SIZE_8BIT;
    chunk->price_size = SIZE_8BIT;
    chunk->volume_size = SIZE_8BIT;

    for (uint64_t i = start + 1; i < num_entries; i++) {
        // Timestamps are non-decreasing, so their deltas are unsigned. Price and
        // volume can move either way, so their deltas are zig-zag-encoded.
        const uint64_t ts_delta = entries[i].ms_since_epoch - entries[i - 1].ms_since_epoch;
        const uint64_t price_delta = zigzag_encode((int64_t)(entries[i].price - entries[i - 1].price));
        const uint64_t volume_delta = zigzag_encode((int64_t)(entries[i].volume - entries[i - 1].volume));

        const size_e new_ts = max_size(chunk->timestamp_size, determine_min_size_uint64(ts_delta));
        const size_e new_p = max_size(chunk->price_size, determine_min_size_uint64(price_delta));
        const size_e new_v = max_size(chunk->volume_size, determine_min_size_uint64(volume_delta));

        // Records start..i contribute (i - start) deltas to the three columns.
        const uint64_t num_deltas = i - start;
        const uint64_t potential_total_size =
            TICKS_CHUNK_HEADER_DISK_SIZE + num_deltas * (uint64_t)(new_ts + new_p + new_v);

        if (potential_total_size > MAX_CHUNK_SIZE) {
            break; // This record won't fit, finalize chunk before it.
        }

        chunk->timestamp_size = new_ts;
        chunk->price_size = new_p;
        chunk->volume_size = new_v;
        chunk->num_records++;
    }

    // Serialize the self-describing chunk header.
    uint8_t* const h = chunk->data;
    le_put_u32(h + 0, chunk->num_records);
    le_put_u64(h + 4, chunk->time_base);
    le_put_u64(h + 12, chunk->price_base);
    le_put_u64(h + 20, chunk->volume_base);
    h[28] = (uint8_t)chunk->timestamp_size;
    h[29] = (uint8_t)chunk->price_size;
    h[30] = (uint8_t)chunk->volume_size;

    // Serialize the three delta columns (timestamps, then prices, then volumes).
    const uint64_t num_deltas = chunk->num_records - 1;
    uint8_t* ts_ptr = chunk->data + TICKS_CHUNK_HEADER_DISK_SIZE;
    uint8_t* price_ptr = ts_ptr + num_deltas * (uint64_t)chunk->timestamp_size;
    uint8_t* volume_ptr = price_ptr + num_deltas * (uint64_t)chunk->price_size;

    for (uint64_t i = start + 1; i < start + chunk->num_records; i++) {
        const uint64_t ts_delta = entries[i].ms_since_epoch - entries[i - 1].ms_since_epoch;
        const uint64_t price_delta = zigzag_encode((int64_t)(entries[i].price - entries[i - 1].price));
        const uint64_t volume_delta = zigzag_encode((int64_t)(entries[i].volume - entries[i - 1].volume));
        write_data(&ts_ptr, ts_delta, chunk->timestamp_size);
        write_data(&price_ptr, price_delta, chunk->price_size);
        write_data(&volume_ptr, volume_delta, chunk->volume_size);
    }

    chunk->data_size = (uint32_t)(TICKS_CHUNK_HEADER_DISK_SIZE +
        num_deltas * (uint64_t)(chunk->timestamp_size + chunk->price_size + chunk->volume_size));
    *row_index = start + chunk->num_records; // Advance the main index

    return (create_chunk_result){.chunk = chunk, .status = TICKS_OK};
}

// Appends a chunk's data to the file and adds its metadata to the in-memory index.
ticks_status_e append_chunk_and_update_index(ticks_file_t* handle, const ticks_chunk_t* chunk) {
    if (handle == NULL || chunk == NULL || handle->file_stream == NULL || chunk->data_size == 0) {
        perror("ERROR: Invalid arguments to append_chunk_and_update_index\n");
        return TICKS_ERROR_INVALID_ARGUMENTS;
    }

    // Flushing the stream serves as a robust check. If the underlying file descriptor is invalid, fflush will fail and return an error.
    // This helps confirm that the file stream has been closed externally.
    if (fflush(handle->file_stream) != 0) {
        perror("ERROR: fflush failed before writing chunk data. The file stream is likely closed");
        return TICKS_ERROR_FILE_IO;
    }

    const uint64_t chunk_write_pos = handle->index_offset;

    if (ticks_fseek64(handle->file_stream, (int64_t)chunk_write_pos, SEEK_SET) != 0) {
        perror("ERROR: ticks_fseek64 before chunk write failed");
        return TICKS_ERROR_FILE_IO;
    }

    if (fwrite(chunk->data, 1, chunk->data_size, handle->file_stream) != chunk->data_size) {
        perror("FATAL ERROR on fwrite (chunk data)");
        return TICKS_ERROR_FILE_IO;
    }

    const ticks_index_entry_t new_index_entry = {
        .chunk_time_base = chunk->time_base,
        .chunk_offset = chunk_write_pos,
        .chunk_size = chunk->data_size,
        .chunk_crc32 = ticks_crc32(chunk->data, chunk->data_size),
        .timestamp_size = chunk->timestamp_size,
        .price_size = chunk->price_size,
        .volume_size = chunk->volume_size
    };

    // Grow the in-memory index array with amortized O(1) doubling rather than
    // reallocating on every single chunk.
    if (handle->index.num_entries == handle->index_capacity) {
        uint32_t new_capacity = (handle->index_capacity == 0) ? 16 : handle->index_capacity * 2;
        ticks_index_entry_t* new_entries =
            realloc(handle->index.entries, (size_t)new_capacity * sizeof(ticks_index_entry_t));
        if (new_entries == NULL) {
            // On failure the original handle->index.entries pointer is still valid.
            perror("ERROR: Unable to allocate memory for index entries\n");
            return TICKS_ERROR_MEMORY_ALLOCATION;
        }
        handle->index.entries = new_entries;
        handle->index_capacity = new_capacity;
    }

    // Add the new entry and increment the count.
    handle->index.entries[handle->index.num_entries] = new_index_entry;
    handle->index.num_entries++;

    // The next chunk (or the index) will be written immediately after this one.
    handle->index_offset = chunk_write_pos + chunk->data_size;

    // Persist the updated index_offset at its fixed header location (LE).
    uint8_t off_buf[sizeof(uint64_t)];
    le_put_u64(off_buf, handle->index_offset);
    if (ticks_fseek64(handle->file_stream, TICKS_OFF_INDEX_OFFSET, SEEK_SET) != 0) {
        perror("ERROR: ticks_fseek64 before index_offset update failed");
        return TICKS_ERROR_FILE_IO;
    }
    if (fwrite(off_buf, 1, sizeof(off_buf), handle->file_stream) != sizeof(off_buf)) {
        perror("ERROR: fwrite (index_offset update)");
        return TICKS_ERROR_FILE_IO;
    }

    return TICKS_OK;
}


ticks_status_e create_chunks(ticks_file_t* handle, const trade_data_t* entries, uint64_t num_entries)
{
    uint64_t row_index = 0;

    while (row_index < num_entries) {
        create_chunk_result result = create_chunk(&row_index, entries, num_entries);
        ticks_chunk_t* chunk = result.chunk;
        if (chunk == NULL || result.status != TICKS_OK) {
            if (result.status == TICKS_ERROR_EMPTY_CHUNK) {
                continue;
            } 
            else if (result.status != TICKS_OK) {
                perror("ERROR: create_chunk failed\n");
                return result.status;
            }
            continue;
        }

        ticks_status_e append_chunk_result = append_chunk_and_update_index(handle, chunk);
        if (append_chunk_result != TICKS_OK) {
            free(chunk->data);
            free(chunk);
            perror("ERROR: append_chunk_and_update_index failed\n");
            return append_chunk_result;
        }
        
        free(chunk->data);
        free(chunk);
    }

    return TICKS_OK;
}

