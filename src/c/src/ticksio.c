#include "ticksio/ticksio.h"

#include "ticksio/ticksio_internal.h"
#include "ticksio/chunks.h"

// Helper function to write the magic and header
static int write_initial_data(FILE *file, struct ticks_file_t_internal* handle) {
    size_t magic_len = strlen(TICKS_MAGIC);
    
    if (fwrite(TICKS_MAGIC, 1, magic_len, file) != magic_len)
        return EXIT_FAILURE;

    if (fwrite(&handle->header, 1, sizeof(ticks_header_t), file) != sizeof(ticks_header_t))
        return EXIT_FAILURE;
    
    long current_offset_long = ftell(file);
    if (current_offset_long == -1L)
        return EXIT_FAILURE;

    uint64_t index_start_offset = (uint64_t)current_offset_long;
    if (fwrite(&index_start_offset, 1, sizeof(uint64_t), file) != sizeof(uint64_t))
        return EXIT_FAILURE;

    handle->index_offset = index_start_offset;
    handle->index_size = 0;
    
    // Index size is initially zero, a temporary placeholder is used to safely pass 0 value
    uint64_t zero_placeholder = 0;
    if (fwrite(&zero_placeholder, 1, sizeof(uint64_t), file) != sizeof(uint64_t))
        return EXIT_FAILURE;

    handle->index.num_entries = 0;
    handle->index.entries = NULL;
    
    return EXIT_SUCCESS;
}

// Helper function to read the index table
static int read_index_table(FILE *file, struct ticks_file_t_internal* handle) {
    if (!file || !handle || handle->index_offset == 0) {
        errno = EINVAL;
        return EXIT_FAILURE;
    }

    // Initialize index entries to NULL
    handle->index.entries = NULL;

    if (handle->index_size == 0)
        return EXIT_SUCCESS; // No entries to reads

    // Allocate memory for the index entries
    uint32_t num_entries = handle->index_size / sizeof(ticks_index_entry_t);
    handle->index.num_entries = num_entries;
    handle->index.entries = malloc(handle->index_size);

    if (handle->index.entries == NULL)
        return EXIT_FAILURE; // errno is set by malloc

    // Move file pointer to the index offset
    if (fseek(file, handle->index_offset, SEEK_SET) != 0) {
        free(handle->index.entries);
        handle->index.entries = NULL;
        return EXIT_FAILURE; // errno is set by fseek
    }

    // Read index entries to memory
    if (fread(handle->index.entries, 1, handle->index_size, file) != handle->index_size) {
        free(handle->index.entries);
        handle->index.entries = NULL;
        return EXIT_FAILURE; // errno is set by fread
    }

    return EXIT_SUCCESS;
}

// --- API Implementation ---

ticks_file_t* ticks_create(const char *filename, const ticks_header_t *header) {
    if (filename == NULL || header == NULL) {
        errno = EINVAL; 
        return NULL;
    }

    // Allocate memory for the internal handle structure
    struct ticks_file_t_internal* handle = malloc(sizeof(struct ticks_file_t_internal));
    if (handle == NULL) {
        printf("Failed to allocate memory: %s\n", strerror(errno));
        return NULL; // errno is set by malloc
    }

    // Open the file for writing (binary mode)
    handle->file_stream = fopen(filename, "wb");
    if (handle->file_stream == NULL) {
        printf("Failed to open file: %s\n", strerror(errno));
        free(handle);
        return NULL; // errno is set by fopen
    }

    // Write data to the file
    if (write_initial_data(handle->file_stream, (struct ticks_file_t_internal*)handle) != 0) {
        printf("Failed to write initial data: %s\n", strerror(errno));
        fclose(handle->file_stream);
        free(handle);
        errno = EIO; 
        return NULL;
    }
    
    // Store a copy of the header internally
    handle->header = *header;
    
    // Keep the file open for subsequent writes/reads, or close and reopen in 'rb+'/'ab' mode.
    // For simplicity, we'll close it here and rely on ticks_open for reading.
    fclose(handle->file_stream);
    handle->file_stream = NULL; 

    // The handle still holds the metadata, but the file stream is closed.
    return (ticks_file_t*)handle;
}

ticks_file_t* ticks_open(const char *filename) {
    if (filename == NULL) {
        errno = EINVAL;
        return NULL;
    }

    // Allocate memory for the internal handle structure
    struct ticks_file_t_internal *handle = malloc(sizeof(struct ticks_file_t_internal));
    if (handle == NULL) {
        return NULL;
    }

    // Open the file for reading (binary mode)
    handle->file_stream = fopen(filename, "rb");
    if (handle->file_stream == NULL) {
        free(handle);
        return NULL;
    }
    
    // Read and Validate the Magic Number
    char magic_buffer[sizeof(TICKS_MAGIC)];
    size_t magic_len = strlen(TICKS_MAGIC); 

    if (fread(magic_buffer, 1, magic_len, handle->file_stream) != magic_len) {
        // File too short or read error
        fclose(handle->file_stream);
        free(handle);
        errno = feof(handle->file_stream) ? EILSEQ : EIO;
        return NULL;
    }

    if (strncmp(magic_buffer, TICKS_MAGIC, magic_len) != 0) {
        fclose(handle->file_stream);
        free(handle);
        errno = EILSEQ; // Invalid magic
        return NULL;
    }
    
    // Read the Header Structure into the internal state
    if (fread(&handle->header, 1, sizeof(ticks_header_t), handle->file_stream) != sizeof(ticks_header_t)) {
        // Read error or file truncated
        fclose(handle->file_stream);
        free(handle);
        errno = EILSEQ;
        return NULL;
    }

    // Read the Index Offset and Size
    if (fread(&handle->index_offset, 1, sizeof(uint64_t), handle->file_stream) != sizeof(uint64_t) || fread(&handle->index_size, 1, sizeof(uint64_t), handle->file_stream) != sizeof(uint64_t)) {
        fclose(handle->file_stream);
        free(handle);
        errno = EILSEQ;
        return NULL;
    }

    read_index_table(handle->file_stream, handle);

    return (ticks_file_t*)handle;
}

int ticks_close(ticks_file_t *handle) {
    if (handle == NULL) {
        errno = EINVAL;
        return -1;
    }
    
    // Try to close the internal file stream if it's open
    int status = (handle->file_stream != NULL) ? fclose(handle->file_stream) : 0;
    
    if (status != 0) {
        // fclose failed, errno is set by fclose
        free(handle);
        return EXIT_FAILURE;
    }

    // Free index entries if allocated
    if (handle->index.entries != NULL) {
        free(handle->index.entries);
    }
    
    // Free the dynamically allocated handle structure
    free(handle);

    return EXIT_SUCCESS;
}

int ticks_get_header(ticks_file_t *handle, ticks_header_t *out_asset_class) {
    if (handle == NULL || out_asset_class == NULL) {
        errno = EINVAL;
        return EXIT_FAILURE;
    }

    *out_asset_class = handle->header;
    
    return EXIT_SUCCESS;
}

int ticks_get_index_offset(ticks_file_t *handle, uint64_t *out_offset) {
    if (handle == NULL || out_offset == NULL) {
        errno = EINVAL;
        return EXIT_FAILURE;
    }

    *out_offset = handle->index_offset;
    
    return EXIT_SUCCESS;
}

int ticks_get_index_size(ticks_file_t *handle, uint64_t *out_size) {
    if (handle == NULL || out_size == NULL) {
        errno = EINVAL;
        return EXIT_FAILURE;
    }

    *out_size = handle->index_size;
    
    return EXIT_SUCCESS;
}

int ticks_add_data(ticks_file_t *handle, const trade_data_t *data, uint64_t num_entries) {
    if (handle == NULL || data == NULL || num_entries == 0 || handle->file_stream == NULL) {
        errno = EINVAL;
        return EXIT_FAILURE;
    }

    // Create chunks from the provided data
    if (create_chunks(handle, (trade_data_t*)data, num_entries) != EXIT_SUCCESS) {
        return EXIT_FAILURE; // errno is set by create_chunks
    }

    return EXIT_SUCCESS;
}