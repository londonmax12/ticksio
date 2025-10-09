#ifndef TICKSIO_H
#define TICKSIO_H

#include <stdint.h>

#include "ticksio/constants.h"
#include "ticksio/types.h"

// --- OPAQUE TYPE DECLARATION (The Handle) ---

// Forward declaration of the internal structure.
// The user only sees a pointer to this incomplete type.
typedef struct ticks_file_t_internal ticks_file_t;

// --- API (Using the Opaque Handle) ---

/**
 * @brief Creates a new ticks file, writes the header, and returns the handle.
 * @param filename The name of the file to create.
 * @param header The header structure containing initial settings.
 * @return A handle pointer on success, NULL on failure (and sets errno).
 */
ticks_file_t* ticks_create(const char *filename, const ticks_header_t *header);

/**
 * @brief Opens an existing ticks file, validates the magic, and returns the handle.
 * @param filename The name of the file to open.
 * @return A handle pointer on success, NULL on failure (and sets errno).
 */
ticks_file_t* ticks_open(const char *filename);

/**
 * @brief Closes the file stream and frees the opaque handle's memory.
 * @param handle The file stream handle to close.
 * @return 0 on success, -1 on failure (and sets errno).
 */
int ticks_close(ticks_file_t *handle);

/**
 * @brief Retrieves the header information from the file handle.
 * @param handle The file stream handle.
 * @param out_asset_class Pointer to store the result.
 * @return 0 on success, -1 on failure.
 */
int ticks_get_header(ticks_file_t *handle, ticks_header_t *out_asset_class);

/**
 * @brief Retrieves the index offset from the file handle.
 * @param handle The file stream handle.
 * @param out_offset Pointer to store the result.
 * @return 0 on success, -1 on failure.
 */
int ticks_get_index_offset(ticks_file_t *handle, uint64_t *out_offset);
/**
 * @brief Retrieves the index size from the file handle.
 * @param handle The file stream handle.
 * @param out_size Pointer to store the result.
 * @return 0 on success, -1 on failure.
 */
int ticks_get_index_size(ticks_file_t *handle, uint64_t *out_size);

#endif // TICKSIO_H