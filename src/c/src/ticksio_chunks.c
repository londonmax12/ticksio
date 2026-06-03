#include "ticksio/ticksio_chunks.h"

#include <string.h>

#include "ticksio/ticksio_types.h"
#include "ticksio/ticksio_internal.h"
#include "ticksio/ticksio_constants.h"
#include "ticksio/ticksio_helpers.h"
#include "ticksio/ticksio_compress.h"
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

// Compute the delta for column j between two adjacent records, already encoded
// to the unsigned value that gets packed on disk (raw for monotonic columns,
// zig-zag for signed ones).
static uint64_t column_delta(const uint64_t* cur, const uint64_t* prev, uint8_t j, col_encoding_e enc) {
    if (enc == COL_ENC_DELTA_UNSIGNED)
        return cur[j] - prev[j];
    return zigzag_encode((int64_t)(cur[j] - prev[j]));
}

// Builds one chunk starting at *row_index. `values` is row-major: record i,
// column j lives at values[i * schema->num_columns + j], and column 0 is the
// timestamp. Each column is delta-encoded against the previous record, so its
// width is governed by how much consecutive ticks *move*, not their magnitude.
//
// Pass 1: "Dry run" to determine the per-column delta widths and how many
//         records fit in the chunk's MAX_CHUNK_SIZE budget.
// Pass 2: "Serialization" — write the self-describing chunk header (a descriptor
//         per column) followed by the column data regions in column order.
static create_chunk_result create_chunk(uint64_t* const row_index, const uint64_t* values,
                                        uint64_t num_entries, const ticks_schema_t* schema) {
    if (*row_index >= num_entries) {
        fprintf(stderr, "ERROR: row_index out of bounds in create_chunk\n");
        return (create_chunk_result){.chunk = NULL, .status = TICKS_ERROR_INVALID_ARGUMENTS};
    }

    const uint8_t ncols = schema->num_columns;

    ticks_chunk_t* chunk = malloc(sizeof(ticks_chunk_t));
    if (chunk == NULL) {
        perror("ERROR: Unable to allocate memory for chunk structure\n");
        return (create_chunk_result){.chunk = NULL, .status = TICKS_ERROR_MEMORY_ALLOCATION};
    }
    // chunk->data is allocated to the chunk's exact serialized size once the
    // dry-run pass below has settled num_records and the per-column widths —
    // rather than eagerly grabbing MAX_CHUNK_SIZE (16 MB) for every chunk, which
    // was wasteful for the small batches a per-file ingest produces.
    chunk->data = NULL;

    const uint64_t start = *row_index;
    const uint64_t* const base_rec = values + start * ncols;

    chunk->num_columns = ncols;
    chunk->num_records = 1; // the base record is always included
    for (uint8_t j = 0; j < ncols; j++) {
        chunk->bases[j] = base_rec[j];
        chunk->widths[j] = SIZE_8BIT; // a delta needs at least one byte
        chunk->encs[j] = schema->encodings[j];
    }
    chunk->time_base = chunk->bases[0];

    const uint32_t header_size = TICKS_CHUNK_HEADER_SIZE(ncols);

    for (uint64_t i = start + 1; i < num_entries; i++) {
        const uint64_t* cur = values + i * ncols;
        const uint64_t* prev = values + (i - 1) * ncols;

        // Widen each column as needed to fit this record's deltas.
        size_e new_widths[TICKS_MAX_COLUMNS];
        uint64_t sum_widths = 0;
        for (uint8_t j = 0; j < ncols; j++) {
            const uint64_t delta = column_delta(cur, prev, j, chunk->encs[j]);
            new_widths[j] = max_size(chunk->widths[j], determine_min_size_uint64(delta));
            sum_widths += new_widths[j];
        }

        // Records start..i contribute (i - start) deltas to every column.
        const uint64_t num_deltas = i - start;
        const uint64_t potential_total_size = header_size + num_deltas * sum_widths;
        if (potential_total_size > MAX_CHUNK_SIZE)
            break; // This record won't fit, finalize chunk before it.

        for (uint8_t j = 0; j < ncols; j++)
            chunk->widths[j] = new_widths[j];
        chunk->num_records++;
    }

    // Last tick's absolute timestamp (column 0). Stored in the index entry (not
    // the chunk header) so range queries can bound every chunk's time span
    // without decoding — including the final chunk, which has no following base.
    chunk->last_timestamp = values[(start + chunk->num_records - 1) * ncols + 0];

    // Now that num_records and the per-column widths are final, the serialized
    // size is exactly header + (num_records - 1) deltas per column. Allocate that
    // precisely. (The dry-run loop above already guaranteed it is <= MAX_CHUNK_SIZE.)
    uint64_t total_delta_bytes = 0;
    for (uint8_t j = 0; j < ncols; j++)
        total_delta_bytes += (uint64_t)(chunk->num_records - 1) * chunk->widths[j];
    const uint64_t exact_size = (uint64_t)header_size + total_delta_bytes;
    chunk->data = malloc(exact_size);
    if (chunk->data == NULL) {
        free(chunk);
        perror("ERROR: Unable to allocate memory for chunk data\n");
        return (create_chunk_result){.chunk = NULL, .status = TICKS_ERROR_MEMORY_ALLOCATION};
    }

    // Serialize the self-describing chunk header: fixed prefix, then one
    // descriptor (base + width + encoding) per column.
    uint8_t* const h = chunk->data;
    le_put_u32(h + 0, chunk->num_records);
    h[4] = ncols;
    uint8_t* desc = h + TICKS_CHUNK_HEADER_BASE_SIZE;
    for (uint8_t j = 0; j < ncols; j++) {
        le_put_u64(desc + 0, chunk->bases[j]);
        desc[8] = (uint8_t)chunk->widths[j];
        desc[9] = (uint8_t)chunk->encs[j];
        desc += TICKS_CHUNK_COL_DESC_SIZE;
    }

    // Serialize the column data regions in column order (all of column 0's
    // deltas, then column 1's, …).
    const uint64_t num_deltas = chunk->num_records - 1;
    uint8_t* col_ptr[TICKS_MAX_COLUMNS];
    uint8_t* p = chunk->data + header_size;
    for (uint8_t j = 0; j < ncols; j++) {
        col_ptr[j] = p;
        p += num_deltas * (uint64_t)chunk->widths[j];
    }
    for (uint64_t i = start + 1; i < start + chunk->num_records; i++) {
        const uint64_t* cur = values + i * ncols;
        const uint64_t* prev = values + (i - 1) * ncols;
        for (uint8_t j = 0; j < ncols; j++)
            write_data(&col_ptr[j], column_delta(cur, prev, j, chunk->encs[j]), chunk->widths[j]);
    }

    chunk->data_size = (uint32_t)(p - chunk->data);
    *row_index = start + chunk->num_records; // Advance the main index

    return (create_chunk_result){.chunk = chunk, .status = TICKS_OK};
}

// Appends a chunk's data to the file and adds its metadata to the in-memory index.
ticks_status_e append_chunk_and_update_index(ticks_file_t* handle, const ticks_chunk_t* chunk) {
    if (handle == NULL || chunk == NULL || handle->file_stream == NULL || chunk->data_size == 0) {
        fprintf(stderr, "ERROR: Invalid arguments to append_chunk_and_update_index\n");
        return TICKS_ERROR_INVALID_ARGUMENTS;
    }

    const uint64_t chunk_write_pos = handle->index_offset;

    // Choose what actually lands on disk. With no compression the chunk's
    // self-describing columnar bytes are written verbatim; with a codec they are
    // compressed into a frame first (see ticksio_compress). The index entry's
    // size and CRC always describe the on-disk bytes, so range pruning and
    // ticks_verify work without decompressing.
    const compression_type_e ctype = handle->header.compression_type;
    const uint8_t* disk_data = chunk->data;
    uint32_t disk_size = chunk->data_size;
    uint8_t* compressed = NULL;

    if (ctype != COMPRESSION_NONE) {
        const size_t bound = ticks_compress_bound(ctype, chunk->data_size);
        compressed = malloc(bound);
        if (compressed == NULL) {
            perror("ERROR: Unable to allocate compression buffer\n");
            return TICKS_ERROR_MEMORY_ALLOCATION;
        }
        size_t out_size = 0;
        ticks_status_e cs = ticks_compress_chunk(ctype, chunk->data, chunk->data_size,
                                                 compressed, bound, &out_size);
        if (cs != TICKS_OK) {
            free(compressed);
            fprintf(stderr, "ERROR: chunk compression failed (%d)\n", cs);
            return cs;
        }
        disk_data = compressed;
        disk_size = (uint32_t)out_size;
    }

    if (ticks_fseek64(handle->file_stream, (int64_t)chunk_write_pos, SEEK_SET) != 0) {
        free(compressed);
        perror("ERROR: ticks_fseek64 before chunk write failed");
        return TICKS_ERROR_FILE_IO;
    }

    if (fwrite(disk_data, 1, disk_size, handle->file_stream) != disk_size) {
        free(compressed);
        perror("FATAL ERROR on fwrite (chunk data)");
        return TICKS_ERROR_FILE_IO;
    }

    const ticks_index_entry_t new_index_entry = {
        .chunk_time_base = chunk->time_base,
        .chunk_last_timestamp = chunk->last_timestamp,
        .chunk_offset = chunk_write_pos,
        .chunk_size = disk_size,
        .chunk_crc32 = ticks_crc32(disk_data, disk_size)
    };

    free(compressed); // CRC computed above; on-disk bytes no longer needed

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

    // The next chunk (or the index) will be written immediately after this one
    // (disk_size, not the uncompressed size, when a codec is in use). This is
    // tracked in memory only — the header's index_offset, index_size, and
    // file-level summary are all persisted once at ticks_close (see
    // finalize_header). Rewriting them after every chunk forced a seek back to
    // the header region between each append, which defeated the stream's
    // write-behind buffering and dominated streaming-insert time. The index
    // itself is likewise written only at close, so a mid-session crash already
    // leaves no usable index on disk; deferring these header fields too does not
    // weaken the (already non-atomic) crash story — see docs/ticks-format.md §3.1.
    handle->index_offset = chunk_write_pos + disk_size;

    return TICKS_OK;
}


ticks_status_e create_chunks(ticks_file_t* handle, const uint64_t* values, uint64_t num_entries,
                             const ticks_schema_t* schema)
{
    uint64_t row_index = 0;

    while (row_index < num_entries) {
        const uint64_t row_index_before = row_index;
        create_chunk_result result = create_chunk(&row_index, values, num_entries, schema);
        ticks_chunk_t* chunk = result.chunk;
        if (chunk == NULL || result.status != TICKS_OK) {
            if (result.status != TICKS_OK) {
                fprintf(stderr, "ERROR: create_chunk failed (%d)\n", result.status);
                return result.status;
            }
            // status == OK but no chunk produced: a successful call must always
            // consume at least one row, otherwise this loop cannot terminate.
            if (row_index <= row_index_before) {
                fprintf(stderr, "ERROR: create_chunk made no progress\n");
                return TICKS_ERROR_UNKNOWN;
            }
            continue;
        }

        ticks_status_e append_chunk_result = append_chunk_and_update_index(handle, chunk);
        if (append_chunk_result != TICKS_OK) {
            free(chunk->data);
            free(chunk);
            fprintf(stderr, "ERROR: append_chunk_and_update_index failed (%d)\n", append_chunk_result);
            return append_chunk_result;
        }
        
        free(chunk->data);
        free(chunk);
    }

    return TICKS_OK;
}

