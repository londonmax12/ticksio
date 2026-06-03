#include "ticksio/ticksio.h"

#include "ticksio/ticksio_internal.h"
#include "ticksio/ticksio_chunks.h"
#include "ticksio/ticksio_index.h"
#include "ticksio/ticksio_compress.h"
#include "ticksio/ticksio_helpers.h"
#include "ticksio/ticksio_schema.h"
#include "ticksio/ticksio_constants.h"
#include "ticksio/ticksio_platform.h"

// The typed record structs are passed to the generic column codec as
// `(const uint64_t*)` with a stride of (number of columns). That is only valid
// if each struct is a tightly packed run of uint64 fields — assert it at compile
// time (negative array size fails the build if the layout ever changes).
typedef char ticks_trade_layout_check[(sizeof(trade_data_t) == 3 * sizeof(uint64_t)) ? 1 : -1];
typedef char ticks_quote_layout_check[(sizeof(quote_data_t) == 5 * sizeof(uint64_t)) ? 1 : -1];

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

// Re-serialize and rewrite the entire fixed header region at offset 0 with the
// session's final values: the file-level summary (record_count, min/max
// timestamp) plus the index_offset / index_size pointers. Called once at
// ticks_close. During an active write session these fields are maintained in
// memory only — persisting them per chunk/per batch forced a seek back to the
// header between sequential appends, which dominated streaming-insert time.
static ticks_status_e finalize_header(struct ticks_file_t_internal* handle) {
    uint8_t buf[TICKS_HEADER_REGION_SIZE];
    serialize_header(buf, &handle->header, handle->index_offset, handle->index_size);
    if (ticks_fseek64(handle->file_stream, 0, SEEK_SET) != 0)
        return TICKS_ERROR_FILE_IO;
    if (fwrite(buf, 1, TICKS_HEADER_REGION_SIZE, handle->file_stream) != TICKS_HEADER_REGION_SIZE)
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

    // The schema is fixed for the life of the file and must be one we recognize.
    if (ticks_schema_lookup(header->schema_id) == NULL)
        return TICKS_ERROR_INVALID_ARGUMENTS;

    // The compression codec must be one this build can also decode, so the file
    // we create can always be read back.
    if (!ticks_compression_supported(header->compression_type))
        return TICKS_ERROR_INVALID_ARGUMENTS;

    // Allocate memory for the internal handle structure and zero memory
    struct ticks_file_t_internal* handle = malloc(sizeof(struct ticks_file_t_internal));
    if (handle == NULL) {
        printf("Failed to allocate memory: %s\n", strerror(errno));
        return TICKS_ERROR_MEMORY_ALLOCATION;
    }
    memset(handle, 0, sizeof(struct ticks_file_t_internal));

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
    handle->header.schema_id = header->schema_id;
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

    // Validate the format version. Reject anything newer than we know about, but
    // also anything older than TICKS_MIN_READ_VERSION: pre-v4 files used header,
    // index, and chunk layouts incompatible with this reader, so accepting them
    // would silently misparse rather than fail cleanly.
    uint16_t version = le_get_u16(region + TICKS_OFF_VERSION);
    if (version < TICKS_MIN_READ_VERSION || version > TICKS_VERSION) {
        fclose(handle->file_stream);
        free(handle);
        return TICKS_ERROR_INVALID_FORMAT;
    }

    // Deserialize the header fields and index pointers from the region buffer.
    deserialize_header(region, &handle->header, &handle->index_offset, &handle->index_size);

    // Reject files whose compression codec this build cannot decode, rather than
    // failing later mid-decode.
    if (!ticks_compression_supported(handle->header.compression_type)) {
        fclose(handle->file_stream);
        free(handle);
        return TICKS_ERROR_INVALID_FORMAT;
    }

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

    // For a write handle, the index and the header's summary/index pointers live
    // only in memory during the session (appends no longer rewrite them each
    // time — see add_records). Serialize the index to disk now, in a single
    // pass, then rewrite the header region with the final summary and index
    // pointers, before closing the stream. A read handle's in-memory index is
    // just a cache of what is already on disk, so nothing is written back.
    ticks_status_e flush_status = TICKS_OK;
    if (handle->mode == FILE_MODE_WRITE && handle->file_stream != NULL &&
        handle->index.num_entries > 0) {
        flush_status = create_index(handle);
        if (flush_status == TICKS_OK)
            flush_status = finalize_header(handle);
    }

    // Try to close the internal file stream if it's open. Whatever the result,
    // every owned allocation (the index array and the handle itself) must still
    // be released — a failed fclose does not exempt us from freeing them.
    int status = (handle->file_stream != NULL) ? fclose(handle->file_stream) : 0;

    free(handle->index.entries); // free(NULL) is a no-op when none was allocated
    free(handle);

    // Surface an index-flush failure first (the file would otherwise be missing
    // its index); otherwise report any fclose error.
    if (flush_status != TICKS_OK)
        return flush_status;
    return (status != 0) ? TICKS_ERROR_FILE_IO : TICKS_OK;
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

// Generic add path shared by every schema. `values` is row-major: record i,
// column j at values[i*ncols + j], with column 0 the timestamp in ms.
static ticks_status_e add_records(ticks_file_t* handle, const uint64_t* values,
                                  uint64_t num_records, const ticks_schema_t* schema) {
    const uint8_t ncols = schema->num_columns;

    // Column 0 (timestamp) is delta-encoded as unsigned, so it must be
    // non-decreasing. Validate the batch (and its continuity with previously
    // written data) before writing anything.
    uint64_t prev = handle->has_data ? handle->last_timestamp : 0;
    for (uint64_t i = 0; i < num_records; i++) {
        const uint64_t ts = values[i * ncols];
        if ((handle->has_data || i > 0) && ts < prev)
            return TICKS_ERROR_UNSORTED_DATA;
        prev = ts;
    }

    ticks_status_e r = create_chunks(handle, values, num_records, schema);
    if (r != TICKS_OK)
        return r;

    // The index is NOT rewritten here. Each chunk append already advances and
    // persists index_offset, and the entries accumulate in memory; the index is
    // serialized to disk once, in a single pass, at ticks_close. Writing the
    // whole index after every add (its previous behavior) made N incremental
    // appends cost O(N^2) in index bytes written — a real cost for the common
    // ingest pattern of appending one Dukascopy hour/day file at a time.

    // Update the file-level summary in memory. min_timestamp is fixed by the
    // very first tick ever written; max_timestamp and record_count grow with
    // each batch. These header fields are NOT written to disk here: like the
    // index, they are persisted once at ticks_close (finalize_header). Rewriting
    // the summary after every add() seeked back to the header region between
    // appends and slowed streaming inserts; the on-disk index does not exist
    // until close anyway, so a stale in-progress header changes nothing about
    // the (already non-atomic) crash behavior.
    const uint64_t last_ts = values[(num_records - 1) * ncols];
    if (!handle->has_data)
        handle->header.min_timestamp = values[0];
    handle->header.max_timestamp = last_ts;
    handle->header.record_count += num_records;

    // Record the high-water timestamp so subsequent calls stay ordered.
    handle->last_timestamp = last_ts;
    handle->has_data = 1;

    return TICKS_OK;
}

ticks_status_e ticks_add_data(ticks_file_t* handle, trade_data_t* data, uint64_t num_entries) {
    if (handle == NULL || data == NULL || num_entries == 0 || handle->file_stream == NULL)
        return TICKS_ERROR_INVALID_ARGUMENTS;
    if (handle->header.schema_id != SCHEMA_TRADE)
        return TICKS_ERROR_SCHEMA_MISMATCH;
    // trade_data_t is a flat run of uint64 (asserted at top of file).
    return add_records(handle, (const uint64_t*)data, num_entries, ticks_schema_lookup(SCHEMA_TRADE));
}

ticks_status_e ticks_add_quotes(ticks_file_t* handle, quote_data_t* data, uint64_t num_entries) {
    if (handle == NULL || data == NULL || num_entries == 0 || handle->file_stream == NULL)
        return TICKS_ERROR_INVALID_ARGUMENTS;
    if (handle->header.schema_id != SCHEMA_QUOTE)
        return TICKS_ERROR_SCHEMA_MISMATCH;
    return add_records(handle, (const uint64_t*)data, num_entries, ticks_schema_lookup(SCHEMA_QUOTE));
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

    // The CRC is taken over the on-disk bytes, so corruption is detected without
    // decoding. The record-count cross-check, however, needs each chunk's
    // num_records (offset 0 of the columnar header), which for a compressed file
    // lives inside the frame's payload — so compressed chunks are decoded here.
    const compression_type_e ctype = handle->header.compression_type;
    uint8_t* decoded = NULL;       // lazy scratch for decompressing compressed chunks
    uint32_t decoded_cap = 0;

    // Recompute the file-level summary as we go so it can be cross-checked
    // against the header's denormalized copy.
    uint64_t record_count = 0;

    for (uint32_t i = 0; i < handle->index.num_entries; i++) {
        const ticks_index_entry_t* e = &handle->index.entries[i];

        if (ticks_fseek64(handle->file_stream, (int64_t)e->chunk_offset, SEEK_SET) != 0) {
            free(buf); free(decoded);
            return TICKS_ERROR_FILE_IO;
        }
        if (fread(buf, 1, e->chunk_size, handle->file_stream) != e->chunk_size) {
            free(buf); free(decoded);
            return TICKS_ERROR_FILE_IO;
        }
        if (ticks_crc32(buf, e->chunk_size) != e->chunk_crc32) {
            free(buf); free(decoded);
            return TICKS_ERROR_CORRUPT_DATA;
        }

        if (ctype == COMPRESSION_NONE) {
            record_count += le_get_u32(buf + 0);
        } else {
            if (e->chunk_size < TICKS_COMPRESS_FRAME_HEADER_SIZE) {
                free(buf); free(decoded);
                return TICKS_ERROR_INVALID_FORMAT;
            }
            const uint32_t usize = le_get_u32(buf + 0);
            if (usize < TICKS_CHUNK_HEADER_BASE_SIZE) {
                free(buf); free(decoded);
                return TICKS_ERROR_INVALID_FORMAT;
            }
            if (decoded == NULL || decoded_cap < usize) {
                uint8_t* nd = realloc(decoded, usize);
                if (nd == NULL) { free(buf); free(decoded); return TICKS_ERROR_MEMORY_ALLOCATION; }
                decoded = nd;
                decoded_cap = usize;
            }
            size_t got = 0;
            ticks_status_e ds = ticks_decompress_chunk(ctype, buf, e->chunk_size,
                                                       decoded, usize, &got);
            if (ds != TICKS_OK) { free(buf); free(decoded); return ds; }
            record_count += le_get_u32(decoded + 0);
        }
    }

    free(buf);
    free(decoded);

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
        case TICKS_ERROR_SCHEMA_MISMATCH:
            return "Schema Mismatch (operation does not match the file's record schema)";
        case TICKS_ERROR_COMPRESSION:
            return "Compression Error (codec failed to compress or decompress a chunk)";
        default:
            return "Unrecognized Status Code";   
    }
}


ticks_status_e ticks_iterator_create(ticks_file_t *handle, int64_t from_ms, int64_t to_ms, ticks_iterator_t** out_iterator)
{
    if (handle == NULL || out_iterator == NULL)
        return TICKS_ERROR_INVALID_ARGUMENTS;

    // The range must be a well-formed, non-empty [from_ms, to_ms) window with
    // non-negative bounds. Bounds are in epoch milliseconds — the same unit as
    // the stored timestamps — so sub-second queries are expressible. The window
    // is NOT validated against wall-clock time: a .ticks file is a historical
    // store and a caller may legitimately ask for a window extending up to (or
    // past) "now". Out-of-range windows simply yield no records.
    if (from_ms >= to_ms || from_ms < 0 || to_ms <= 0)
        return TICKS_ERROR_INVALID_ARGUMENTS;

    ticks_iterator_t* iterator = malloc(sizeof(ticks_iterator_t));
    if (iterator == NULL)
        return TICKS_ERROR_MEMORY_ALLOCATION;

    memset(iterator, 0, sizeof(ticks_iterator_t));
    iterator->file_handle = handle;
    iterator->from_ms = (uint64_t)from_ms;
    iterator->to_ms = (uint64_t)to_ms;
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

// Grow *buf to at least `need` bytes (amortized via realloc), tracking capacity
// in *cap. Returns 0 on success, -1 on allocation failure (buffer left intact).
static int iter_ensure_cap(uint8_t** buf, uint32_t* cap, uint32_t need) {
    if (*buf != NULL && *cap >= need)
        return 0;
    uint8_t* nb = realloc(*buf, need);
    if (nb == NULL)
        return -1;
    *buf = nb;
    *cap = need;
    return 0;
}

// Load chunk `i` into the iterator's buffer and reset the decode cursor to its
// first record (cur[] = the chunk's absolute base values). Reads the column
// count, widths and encodings straight from the self-describing chunk header,
// so the iterator does not need the file's schema. If the file is compressed,
// the on-disk frame is read into comp_buf and decoded into chunk_buf first.
static ticks_status_e iter_load_chunk(ticks_iterator_t* it, uint32_t i) {
    const ticks_index_entry_t* e = &it->file_handle->index.entries[i];
    const compression_type_e ctype = it->file_handle->header.compression_type;

    if (ticks_fseek64(it->file_handle->file_stream, (int64_t)e->chunk_offset, SEEK_SET) != 0)
        return TICKS_ERROR_FILE_IO;

    // Bytes of the decoded columnar payload now in chunk_buf (== chunk_size when
    // the file is uncompressed; the frame's decoded size when a codec is used).
    uint32_t payload_size;

    if (ctype == COMPRESSION_NONE) {
        // chunk_buf holds the on-disk bytes directly.
        if (e->chunk_size < TICKS_CHUNK_HEADER_BASE_SIZE)
            return TICKS_ERROR_INVALID_FORMAT;
        if (iter_ensure_cap(&it->chunk_buf, &it->chunk_buf_cap, e->chunk_size) != 0)
            return TICKS_ERROR_MEMORY_ALLOCATION;
        if (fread(it->chunk_buf, 1, e->chunk_size, it->file_handle->file_stream) != e->chunk_size)
            return TICKS_ERROR_FILE_IO;
        payload_size = e->chunk_size;
    } else {
        // Read the compressed frame, then decode it into chunk_buf. The frame's
        // prefix gives the exact decoded size, so chunk_buf is sized precisely.
        if (iter_ensure_cap(&it->comp_buf, &it->comp_buf_cap, e->chunk_size) != 0)
            return TICKS_ERROR_MEMORY_ALLOCATION;
        if (fread(it->comp_buf, 1, e->chunk_size, it->file_handle->file_stream) != e->chunk_size)
            return TICKS_ERROR_FILE_IO;
        if (e->chunk_size < TICKS_COMPRESS_FRAME_HEADER_SIZE)
            return TICKS_ERROR_INVALID_FORMAT;
        const uint32_t decoded_size = le_get_u32(it->comp_buf);
        if (decoded_size < TICKS_CHUNK_HEADER_BASE_SIZE)
            return TICKS_ERROR_INVALID_FORMAT;
        if (iter_ensure_cap(&it->chunk_buf, &it->chunk_buf_cap, decoded_size) != 0)
            return TICKS_ERROR_MEMORY_ALLOCATION;
        size_t got = 0;
        ticks_status_e ds = ticks_decompress_chunk(ctype, it->comp_buf, e->chunk_size,
                                                   it->chunk_buf, decoded_size, &got);
        if (ds != TICKS_OK)
            return ds;
        payload_size = (uint32_t)got;
    }

    const uint8_t* b = it->chunk_buf;
    it->chunk_nrec = le_get_u32(b + 0);
    it->chunk_ncols = b[4];
    if (it->chunk_nrec == 0 || it->chunk_ncols == 0 || it->chunk_ncols > TICKS_MAX_COLUMNS)
        return TICKS_ERROR_INVALID_FORMAT;

    const uint32_t header_size = TICKS_CHUNK_HEADER_SIZE(it->chunk_ncols);
    if (payload_size < header_size)
        return TICKS_ERROR_INVALID_FORMAT;

    const uint8_t* desc = b + TICKS_CHUNK_HEADER_BASE_SIZE;
    const uint8_t* col = b + header_size;
    const uint64_t num_deltas = it->chunk_nrec - 1;
    for (uint8_t j = 0; j < it->chunk_ncols; j++) {
        it->cur[j] = le_get_u64(desc + 0);
        it->widths[j] = (size_e)desc[8];
        it->encs[j] = (col_encoding_e)desc[9];
        it->cols[j] = col;
        col += num_deltas * (uint64_t)it->widths[j];
        desc += TICKS_CHUNK_COL_DESC_SIZE;
    }

    it->current_chunk = i;
    it->current_record_in_chunk = 0;
    it->chunk_loaded = 1;
    return TICKS_OK;
}

// Advance the decode cursor from record k to k+1, folding in delta k for every
// column (raw for unsigned columns, un-zig-zagged for signed ones).
static void iter_advance_record(ticks_iterator_t* it) {
    const uint32_t k = it->current_record_in_chunk;
    if (k + 1 < it->chunk_nrec) {
        for (uint8_t j = 0; j < it->chunk_ncols; j++) {
            const uint64_t raw = iter_read_uint(it->cols[j] + (uint64_t)k * it->widths[j], it->widths[j]);
            if (it->encs[j] == COL_ENC_DELTA_UNSIGNED)
                it->cur[j] += raw;
            else
                it->cur[j] = (uint64_t)((int64_t)it->cur[j] + zigzag_decode(raw));
        }
    }
    it->current_record_in_chunk++;
}

// First index entry whose chunk could overlap the window — i.e. the first whose
// chunk_last_timestamp >= from_ms. Chunks are stored in non-decreasing time
// order, so chunk_last_timestamp is itself non-decreasing across entries, which
// makes this a binary search (lower_bound). This turns the initial seek of a
// range query from O(chunks-before-the-window) into O(log n), so a query deep in
// a large file no longer pays for every chunk that precedes it.
static uint32_t iter_first_chunk(const ticks_index_t* idx, uint64_t from_ms) {
    uint32_t lo = 0, hi = idx->num_entries;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (idx->entries[mid].chunk_last_timestamp < from_ms)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

// Produce the next record within [from, to) as raw column values (out_values
// must hold at least the chunk's column count; *out_ncols is set to it). Shared
// by the typed ticks_iterator_next* wrappers.
static ticks_status_e iter_next_columns(ticks_iterator_t* it, uint64_t* out_values, uint8_t* out_ncols) {
    const ticks_index_t* idx = &it->file_handle->index;

    for (;;) {
        // Ensure a chunk with un-emitted records is loaded. The very first seek
        // jumps straight to the first possibly-overlapping chunk via binary
        // search; afterwards we just walk forward to the next chunk in order.
        if (!it->chunk_loaded || it->current_record_in_chunk >= it->chunk_nrec) {
            uint32_t i = it->chunk_loaded ? it->current_chunk + 1
                                          : iter_first_chunk(idx, it->from_ms);
            for (; i < idx->num_entries; i++) {
                const ticks_index_entry_t* e = &idx->entries[i];
                // Skip chunks lying entirely before the range. Uses last_timestamp,
                // so even the final chunk is pruned here without being decoded.
                if (e->chunk_last_timestamp < it->from_ms)
                    continue;
                // Chunks are time-ordered; once one starts at/after `to`, no later
                // chunk can qualify either.
                if (e->chunk_time_base >= it->to_ms)
                    return TICKS_EOF;
                break;
            }
            if (i >= idx->num_entries)
                return TICKS_EOF;

            ticks_status_e st = iter_load_chunk(it, i);
            if (st != TICKS_OK)
                return st;
        }

        // Emit the next qualifying record from the loaded chunk.
        while (it->current_record_in_chunk < it->chunk_nrec) {
            const uint64_t t = it->cur[0];

            // `to` is exclusive and the stream is sorted, so this ends iteration.
            if (t >= it->to_ms)
                return TICKS_EOF;

            const int emit = (t >= it->from_ms);
            if (emit)
                for (uint8_t j = 0; j < it->chunk_ncols; j++)
                    out_values[j] = it->cur[j]; // snapshot before advancing
            iter_advance_record(it);

            if (emit) {
                if (out_ncols)
                    *out_ncols = it->chunk_ncols;
                return TICKS_OK;
            }
        }
        // Chunk exhausted without hitting `to`; loop to find the next chunk.
    }
}

ticks_status_e ticks_iterator_next(ticks_iterator_t* iterator, trade_data_t* out_record)
{
    if (iterator == NULL || out_record == NULL)
        return TICKS_ERROR_INVALID_ARGUMENTS;
    if (iterator->file_handle->header.schema_id != SCHEMA_TRADE)
        return TICKS_ERROR_SCHEMA_MISMATCH;

    uint64_t vals[TICKS_MAX_COLUMNS];
    ticks_status_e st = iter_next_columns(iterator, vals, NULL);
    if (st != TICKS_OK)
        return st;

    out_record->ms_since_epoch = vals[0];
    out_record->price = vals[1];
    out_record->volume = vals[2];
    return TICKS_OK;
}

ticks_status_e ticks_iterator_next_quote(ticks_iterator_t* iterator, quote_data_t* out_record)
{
    if (iterator == NULL || out_record == NULL)
        return TICKS_ERROR_INVALID_ARGUMENTS;
    if (iterator->file_handle->header.schema_id != SCHEMA_QUOTE)
        return TICKS_ERROR_SCHEMA_MISMATCH;

    uint64_t vals[TICKS_MAX_COLUMNS];
    ticks_status_e st = iter_next_columns(iterator, vals, NULL);
    if (st != TICKS_OK)
        return st;

    out_record->ms_since_epoch = vals[0];
    out_record->bid = vals[1];
    out_record->ask = vals[2];
    out_record->bid_size = vals[3];
    out_record->ask_size = vals[4];
    return TICKS_OK;
}

ticks_status_e ticks_iterator_destroy(ticks_iterator_t *iterator)
{
    if (iterator == NULL)
        return TICKS_ERROR_INVALID_ARGUMENTS;

    free(iterator->chunk_buf);
    free(iterator->comp_buf);
    free(iterator);

    return TICKS_OK;
}