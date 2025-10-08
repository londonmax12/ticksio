#include "ticksio.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h> // Required for malloc and free

// --- INTERNAL (HIDDEN) STRUCTURE DEFINITION ---
// This is defined ONLY here in the .c file.
// Its contents are invisible to external code.
struct ticks_file_t_internal {
    FILE *file_stream;  // The hidden file pointer
    ticks_header_t header; // The hidden file header (contains all data)
    uint64_t index_offset; // Byte offset where the index data starts in the file
    uint64_t index_size;   // Size of the index data in bytes
};


// Helper function to write the magic and header
static int write_initial_data(FILE *file, const ticks_header_t *header) {
    size_t magic_len = strlen(TICKS_MAGIC);
    
    if (fwrite(TICKS_MAGIC, 1, magic_len, file) != magic_len) {
        return -1;
    }

    if (fwrite(header, 1, sizeof(ticks_header_t), file) != sizeof(ticks_header_t)) {
        return -1;
    }
    
    long current_offset_long = ftell(file);
    if (current_offset_long == -1L) {
        return -1;
    }

    uint64_t index_start_offset = (uint64_t)current_offset_long;
    if (fwrite(&index_start_offset, 1, sizeof(uint64_t), file) != sizeof(uint64_t)) {
        return -1;
    }

    
    // Index size is initially zero, a temporary placeholder is used to safely pass 0 value
    uint64_t zero_placeholder = 0;
    if (fwrite(&zero_placeholder, 1, sizeof(uint64_t), file) != sizeof(uint64_t)) {
        return -1;
    }

    return 0;
}

// --- API Implementation ---

ticks_file_t* ticks_create(const char *filename, const ticks_header_t *header) {
    if (filename == NULL || header == NULL) {
        errno = EINVAL; 
        return NULL;
    }

    // Allocate memory for the internal handle structure
    struct ticks_file_t_internal *handle = malloc(sizeof(struct ticks_file_t_internal));
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
    if (write_initial_data(handle->file_stream, header) != 0) {
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
    if (fread(&handle->index_offset, 1, sizeof(uint64_t), handle->file_stream) != sizeof(uint64_t) ||
        fread(&handle->index_size, 1, sizeof(uint64_t), handle->file_stream) != sizeof(uint64_t))
    {
        fclose(handle->file_stream);
        free(handle);
        errno = EILSEQ;
        return NULL;
    }

    return (ticks_file_t*)handle;
}

int ticks_close(ticks_file_t *handle) {
    if (handle == NULL) {
        errno = EINVAL;
        return -1;
    }
    
    // Try to close the internal file stream if it's open
    int status = (handle->file_stream != NULL) ? fclose(handle->file_stream) : 0;
    
    // Free the dynamically allocated handle structure
    free(handle);

    // Return status of fclose (0 for success, -1 for failure)
    return (status == EOF) ? -1 : 0;
}

int ticks_get_header(ticks_file_t *handle, ticks_header_t *out_asset_class) {
    if (handle == NULL || out_asset_class == NULL) {
        errno = EINVAL;
        return -1;
    }

    *out_asset_class = handle->header;
    
    return 0;
}

int ticks_get_index_offset(ticks_file_t *handle, uint64_t *out_offset) {
    if (handle == NULL || out_offset == NULL) {
        errno = EINVAL;
        return -1;
    }

    *out_offset = handle->index_offset;
    
    return 0;
}

int ticks_get_index_size(ticks_file_t *handle, uint64_t *out_size) {
    if (handle == NULL || out_size == NULL) {
        errno = EINVAL;
        return -1;
    }

    *out_size = handle->index_size;
    
    return 0;
}
