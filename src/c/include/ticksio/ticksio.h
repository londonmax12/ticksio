#ifndef TICKSIO_H
#define TICKSIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ticksio/constants.h"
#include "ticksio/types.h"

// --- API (Uses opaque pointer for file handle) ---
/**
 * @brief Opens an existing ticks file in read mode, validates the magic, and returns the handle.
 * @param filename The name of the file to open.
 * @return A handle pointer on success, NULL on failure (and sets errno).
 */
ticks_file_t* ticks_open_read(const char* filename);
/**
 * @brief Opens an existing ticks file in write mode, validates the magic, and returns the handle.
 * @param filename The name of the file to open.
 * @return A handle pointer on success, NULL on failure (and sets errno).
 */
ticks_file_t* ticks_open_write(const char* filename);
/**
 * @brief Creates a new ticks file, writes the header, and returns the handle.
 * @param filename The name of the file to create.
 * @param header The header structure containing initial settings.
 * @return A handle pointer on success, NULL on failure (and sets errno).
 */
ticks_file_t* ticks_new_file(const char* filename, ticks_header_t* header);

/**
 * @brief Closes the file stream and frees the opaque handle's memory.
 * @param handle The file stream handle to close.
 * @return 0 on success, 1 on failure (and sets errno).
 */
int ticks_close(ticks_file_t* handle);

/**
 * @brief Retrieves the header information from the file handle.
 * @param handle The file stream handle.
 * @param out_asset_class Pointer to store the result.
 * @return 0 on success, 1 on failure.
 */
int ticks_get_header(ticks_file_t* handle, ticks_header_t* out_asset_class);

/**
 * @brief Retrieves the index offset from the file handle.
 * @param handle The file stream handle.
 * @param out_offset Pointer to store the result.
 * @return 0 on success, 1 on failure.
 */
int ticks_get_index_offset(ticks_file_t* handle, uint64_t* out_offset);
/**
 * @brief Retrieves the index size from the file handle.
 * @param handle The file stream handle.
 * @param out_size Pointer to store the result.
 * @return 0 on success, 1 on failure.
*/
int ticks_get_index_size(ticks_file_t* handle, uint64_t* out_size);

/**
 * @brief Adds trade data entries to the ticks file, creating chunks as needed.
 * @param handle The file stream handle.
 * @param data Pointer to the array of trade_data_t entries to add.
 * @param num_entries Number of entries in the data array.
 * @return 0 on success, 1 on failure (and sets errno).
 */
int ticks_add_data(ticks_file_t* handle, const trade_data_t* data, uint64_t num_entries);

#endif // TICKSIO_H