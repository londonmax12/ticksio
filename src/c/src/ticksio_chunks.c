#include "ticksio/ticksio_chunks.h"

#include <string.h>

#include "ticksio/ticksio_types.h"
#include "ticksio/ticksio_internal.h"
#include "ticksio/ticksio_constants.h"

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

    // Pass 1: "Dry run" to determine the final data types and number of records for the chunk.
    // Pass 2: "Serialization" to write the records using the determined data types.

    chunk->time_base = entries[*row_index].ms_since_epoch;
    chunk->num_records = 0;
    chunk->timestamp_size = SIZE_8BIT;
    chunk->price_size = SIZE_8BIT;
    chunk->volume_size = SIZE_8BIT;
    
    const uint64_t start_row_index = *row_index;
    uint64_t temp_row_index = *row_index;

    // Determine optimal sizes and record count for this chunk.
    for (; temp_row_index < num_entries; temp_row_index++) {
        const uint64_t time_delta = entries[temp_row_index].ms_since_epoch - chunk->time_base;
        size_e needed_ts_size = determine_min_size_uint64(time_delta);
        size_e needed_p_size = determine_min_size_uint64(entries[temp_row_index].price);
        size_e needed_v_size = determine_min_size_uint64(entries[temp_row_index].volume);
        
        size_e new_ts_size = (needed_ts_size > chunk->timestamp_size) ? needed_ts_size : chunk->timestamp_size;
        size_e new_p_size = (needed_p_size > chunk->price_size) ? needed_p_size : chunk->price_size;
        size_e new_v_size = (needed_v_size > chunk->volume_size) ? needed_v_size : chunk->volume_size;
        
        uint64_t potential_total_size = (chunk->num_records + 1) * (uint64_t)(new_ts_size + new_p_size + new_v_size);
        
        if (potential_total_size > MAX_CHUNK_SIZE) {
            break; // This record won't fit, finalize chunk before it.
        }
        
        chunk->timestamp_size = new_ts_size;
        chunk->price_size = new_p_size;
        chunk->volume_size = new_v_size;
        chunk->num_records++;
    }

    // If no records could be added, handle gracefully.
    if (chunk->num_records == 0) {
        if (*row_index == start_row_index) {
            (*row_index)++; // Advance to prevent infinite loop.
        }

        free(chunk->data);
        free(chunk);
        perror("ERROR: Unable to fit any records into chunk due to size constraints\n");
        return (create_chunk_result){.chunk = NULL, .status = TICKS_ERROR_EMPTY_CHUNK};
    }

    // Serialize the records using the determined optimal sizes.
    uint8_t* data_ptr = chunk->data;
    for (uint64_t i = start_row_index; i < start_row_index + chunk->num_records; ++i) {
        const uint64_t time_delta = entries[i].ms_since_epoch - chunk->time_base;
        write_data(&data_ptr, time_delta, chunk->timestamp_size);
        write_data(&data_ptr, entries[i].price, chunk->price_size);
        write_data(&data_ptr, entries[i].volume, chunk->volume_size);
    }
    
    chunk->data_size = data_ptr - chunk->data;
    *row_index = start_row_index + chunk->num_records; // Advance the main index

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

    if (_fseeki64(handle->file_stream, chunk_write_pos, SEEK_SET) != 0) {
        perror("ERROR: _fseeki64 before chunk write failed");
        return TICKS_ERROR_FILE_IO;
    }

    if (fwrite(chunk->data, 1, chunk->data_size, handle->file_stream) != chunk->data_size) {
        perror("FATAL ERROR on fwrite (chunk data)");
        return TICKS_ERROR_FILE_IO;
    }
    
    handle->index_offset = chunk_write_pos + chunk->data_size;
    
    const ticks_index_entry_t new_index_entry = {
        .chunk_time_base = chunk->time_base,
        .chunk_offset = chunk_write_pos,
        .chunk_size = chunk->data_size,
        .timestamp_size = chunk->timestamp_size,
        .price_size = chunk->price_size,
        .volume_size = chunk->volume_size
    };
    
    // TODO: This approach resizes the array for every single chunk, which is inefficient
    // for a large number of chunks. It is necessary because the ticks_file_t_internal
    // struct does not have a field to track the allocated capacity of the index array,
    // preventing a more efficient growth strategy (e.g., doubling capacity).
    ticks_index_entry_t* new_entries = realloc(handle->index.entries, (handle->index.num_entries + 1) * sizeof(ticks_index_entry_t));

    if (new_entries == NULL) {
        // If realloc fails, the original handle->index.entries pointer is still valid.
        perror("ERROR: Unable to allocate memory for index entries\n");
        return TICKS_ERROR_MEMORY_ALLOCATION;
    }
    handle->index.entries = new_entries;

    // Add the new entry to the now-larger array and increment the count.
    handle->index.entries[handle->index.num_entries] = new_index_entry;
    handle->index.num_entries++;

    // Get the current file position to update index_offset
    long current_pos_long = ftell(handle->file_stream);
    if (current_pos_long == -1L) {
        perror("ERROR: ftell failed after writing chunk data");
        return TICKS_ERROR_FILE_IO;
    }
    handle->index_offset = (uint64_t)current_pos_long;

    // Write new index_offset
    if (_fseeki64(handle->file_stream, 4 + sizeof(ticks_header_t), SEEK_SET) != 0) {
        perror("ERROR: _fseeki64 before index_offset update failed");
        return TICKS_ERROR_FILE_IO;
    }
    if (fwrite(&handle->index_offset, 1, sizeof(uint64_t), handle->file_stream) != sizeof(uint64_t)) {
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

