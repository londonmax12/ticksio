#include "ticksio/chunks.h"

#include "ticksio/types.h"
#include "ticksio/ticksio_internal.h"
#include "ticksio/constants.h"

ticks_chunk_t* create_chunk(uint64_t* const row_index, const trade_data_t* entries, uint64_t num_entries) {
    ticks_chunk_t* chunk = malloc(sizeof(ticks_chunk_t));
    if (chunk == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    chunk->data = malloc(MAX_CHUNK_SIZE);
    if (chunk->data == NULL) {
        free(chunk);
        errno = ENOMEM;
        return NULL;
    }

    chunk->data_size = 0;
    chunk->num_records = 0;
    chunk->time_base = entries[*row_index].ms_since_epoch;
    
    // Get size needed for timestamp deltas
    // We use the max needed size here for simplicity; can be optimized later
    // TODO: ^^
    chunk->timestamp_size = SIZE_64BIT; // Placeholder, will be updated

    chunk->price_size = determine_min_size_uint64(entries[*row_index].price);
    chunk->volume_size = determine_min_size_uint64(entries[*row_index].volume);

    const uint64_t chunk_base_time = entries[*row_index].ms_since_epoch;
    
    for (; *row_index < num_entries; ) {
        // Determine sizes needed
        const uint64_t time_delta = entries[*row_index].ms_since_epoch - chunk_base_time;
        size_e timestamp_size = determine_min_size_uint64(time_delta);
        size_e price_size = determine_min_size_uint64(entries[*row_index].price);
        size_e volume_size = determine_min_size_uint64(entries[*row_index].volume);
        
        // TODO: Check if it's worth increasing chunk's sizes rather than starting a new chunk
        // TODO: Check if it's worth making a making a new chunk if most future entries can fit in a smaller type
        if (timestamp_size > chunk->timestamp_size || price_size > chunk->price_size || volume_size > chunk->volume_size) {
            printf("Chunk full: size increase needed (ts: %d -> %d, price: %d -> %d, vol: %d -> %d)\n",
                chunk->timestamp_size, timestamp_size,
                chunk->price_size, price_size,
                chunk->volume_size, volume_size);
            printf("Old entry: time delta %llu, price %llu, volume %llu\n",
                time_delta, entries[*row_index - 1].price, entries[*row_index - 1].volume);
            printf("New entry: time delta %llu, price %llu, volume %llu\n",
                time_delta, entries[*row_index].price, entries[*row_index].volume);
            
            break; // Would exceed chunk's max sizes
        }
        
        const uint64_t new_size = chunk->data_size + chunk->timestamp_size + chunk->price_size + chunk->volume_size;
        if (new_size > MAX_CHUNK_SIZE) {
            printf("Chunk full: max size exceeded (%u + %u > %u)\n", chunk->data_size, 
                chunk->timestamp_size + chunk->price_size + chunk->volume_size, MAX_CHUNK_SIZE);
            break; // Would exceed max chunk size
        }

        chunk->num_records++;
        chunk->data_size += timestamp_size + price_size + volume_size;
 
        *row_index = *row_index + 1;
    }

    return chunk;
}

int create_chunks(ticks_file_t* handle, const trade_data_t* entries, uint64_t num_entries)
{
    uint64_t row_index = 0;

    // Make new chunks
    while (row_index != num_entries) {
        ticks_chunk_t* chunk = create_chunk(&row_index, entries, num_entries);
        if (chunk == NULL) {
            return EXIT_FAILURE; // errno set by create_chunk
        }

        printf("Created chunk with %u records, data size %u bytes\n", chunk->num_records, chunk->data_size);

        // Chunk cleanup
        free(chunk->data);
        free(chunk);
    }

    return EXIT_SUCCESS;
}
