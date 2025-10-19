#include "ticksio/ticksio.h"

#include "ticksio/ticksio_internal.h"
#include "ticksio/chunks.h"
#include "ticksio/index.h"
#include "ticksio.h"

// Helper function to write the magic and header
static ticks_status_e write_initial_data(FILE *file, struct ticks_file_t_internal* handle) {
    size_t magic_len = strlen(TICKS_MAGIC);
    
    if (fwrite(TICKS_MAGIC, 1, magic_len, file) != magic_len)
        return TICKS_ERROR_FILE_IO;

    if (fwrite(&handle->header, 1, sizeof(ticks_header_t), file) != sizeof(ticks_header_t))
        return TICKS_ERROR_FILE_IO;
    
    long current_offset_long = ftell(file);
    if (current_offset_long == -1L)
        return TICKS_ERROR_FILE_IO;

    // Offset after header + index_offset size (uint64_t) + index_size size (uint64_t)
    uint64_t index_start_offset = (uint64_t)current_offset_long + sizeof(uint64_t) + sizeof (uint64_t);
    if (fwrite(&index_start_offset, 1, sizeof(uint64_t), file) != sizeof(uint64_t))
        return TICKS_ERROR_FILE_IO;

    handle->index_offset = index_start_offset;
    handle->index_size = 0;
    
    // Index size is initially zero, a temporary placeholder is used to safely pass 0 value
    uint64_t zero_placeholder = 0;
    if (fwrite(&zero_placeholder, 1, sizeof(uint64_t), file) != sizeof(uint64_t))
        return TICKS_ERROR_FILE_IO;

    handle->index.num_entries = 0;
    handle->index.entries = NULL;
    
    return TICKS_OK;
}

// Helper function to read the index table
static ticks_status_e read_index_table(FILE *file, struct ticks_file_t_internal* handle) {
    if (!file || !handle || handle->index_offset == 0) {
        return TICKS_ERROR_INVALID_ARGUMENTS;
    }

    // Initialize index entries to NULL
    handle->index.entries = NULL;

    if (handle->index_size == 0)
        return TICKS_ERROR_INVALID_FORMAT; // No entries to reads

    // Allocate memory for the index entries
    uint32_t num_entries = handle->index_size / sizeof(ticks_index_entry_t);
    handle->index.num_entries = num_entries;
    handle->index.entries = malloc(handle->index_size);

    if (handle->index.entries == NULL)
        return TICKS_ERROR_MEMORY_ALLOCATION;

    // Move file pointer to the index offset
    if (fseek(file, handle->index_offset, SEEK_SET) != 0) {
        free(handle->index.entries);
        handle->index.entries = NULL;
        return TICKS_ERROR_FILE_IO;
    }

    // Read index entries to memory
    if (fread(handle->index.entries, 1, handle->index_size, file) != handle->index_size) {
        free(handle->index.entries);
        handle->index.entries = NULL;
        return TICKS_ERROR_FILE_IO;
    }

    return TICKS_OK;
}

// --- API Implementation ---
ticks_status_e ticks_new_file(const char* filename, ticks_header_t* header, ticks_file_t** out_handle) {
    if (filename == NULL || header == NULL) 
        return TICKS_ERROR_INVALID_ARGUMENTS;

    if (header->endianness == ENDIAN_UNDEFINED) {
        if (is_little_endian())
            header->endianness = ENDIAN_LITTLE;
        else
            header->endianness = ENDIAN_BIG;
    }
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

    // Store a copy of the header internally
    handle->header.asset_class = header->asset_class;
    strncpy(handle->header.ticker, header->ticker, TICKS_TICKER_SIZE);
    strncpy(handle->header.currency, header->currency, TICKS_CURRENCY_SIZE);
    strncpy(handle->header.country, header->country, TICKS_COUNTRY_SIZE);
    handle->header.compression_type = header->compression_type;

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

    // Open the file in specified mode
    handle->file_stream = fopen(filename, mode);
    if (handle->file_stream == NULL) {
        free(handle);
        return TICKS_ERROR_FILE_IO;
    }
    
    // Read and Validate the Magic Number
    char magic_buffer[sizeof(TICKS_MAGIC)];
    size_t magic_len = strlen(TICKS_MAGIC); 

    if (fread(magic_buffer, 1, magic_len, handle->file_stream) != magic_len) {
        // File too short or read error
        fclose(handle->file_stream);
        free(handle);
        return feof(handle->file_stream) ? TICKS_ERROR_INVALID_FORMAT : TICKS_ERROR_FILE_IO;
    }

    if (strncmp(magic_buffer, TICKS_MAGIC, magic_len) != 0) {
        fclose(handle->file_stream);
        free(handle);
        return TICKS_ERROR_INVALID_FORMAT;
    }
    
    // Read the Header Structure into the internal state
    if (fread(&handle->header, 1, sizeof(ticks_header_t), handle->file_stream) != sizeof(ticks_header_t)) {
        // Read error or file truncated
        fclose(handle->file_stream);
        free(handle);
        return TICKS_ERROR_INVALID_FORMAT;
    }

    // TODO: Add more robust error checking to this function

    // Read the Index Offset and Size
    if (fread(&handle->index_offset, 1, sizeof(uint64_t), handle->file_stream) != sizeof(uint64_t) || fread(&handle->index_size, 1, sizeof(uint64_t), handle->file_stream) != sizeof(uint64_t)) {
        fclose(handle->file_stream);
        free(handle);
        return TICKS_ERROR_FILE_IO;
    }

    // Read the Index Table into memory
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

    handle->mode = FILE_MODE_READ;
    
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

    // Create chunks from the provided data
    ticks_status_e create_chunks_result = create_chunks(handle, data, num_entries);
    if (create_chunks_result != TICKS_OK)
        return create_chunks_result;
  
    ticks_status_e create_index_result = create_index(handle);
    if (create_index_result != TICKS_OK)
        return create_index_result;

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
        default:
            return "Unrecognized Status Code";   
    }
}