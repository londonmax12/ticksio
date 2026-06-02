#ifndef TICKSIO_H
#define TICKSIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ticksio/ticksio_types.h"

// --- API (Uses opaque pointer for file handle) ---
/**
 * @brief Opens an existing ticks file in read mode, validates the magic, and returns the handle.
 * @param filename The name of the file to open.
 * @param out_handle Pointer to store the resulting handle.
 * @return Status code indicating success or failure (0 = OK).
 */
ticks_status_e ticks_open_read(const char* filename, ticks_file_t** out_handle);
/**
 * @brief Opens an existing ticks file in write mode, validates the magic, and returns the handle.
 * @param filename The name of the file to open.
 * @param out_handle Pointer to store the resulting handle.
 * @return Status code indicating success or failure (0 = OK).
 */
ticks_status_e ticks_open_write(const char* filename, ticks_file_t** out_handle);
/**
 * @brief Creates a new ticks file, writes the header, and returns the handle.
 * @param filename The name of the file to create.
 * @param header The header structure containing initial settings.
 * @param out_handle Pointer to store the resulting handle.
 * @return Status code indicating success or failure (0 = OK).
 */
ticks_status_e ticks_new_file(const char* filename, ticks_header_t* header, ticks_file_t** out_handle);

/**
 * @brief Closes the file stream and frees the opaque handle's memory.
 * @param handle The file stream handle to close.
 * @return Status code indicating success or failure (0 = OK).
 */
ticks_status_e ticks_close(ticks_file_t* handle);

/**
 * @brief Retrieves the header information from the file handle.
 * @param handle The file stream handle.
 * @param out_asset_class Pointer to store the result.
 * @return Status code indicating success or failure (0 = OK).
 */
ticks_status_e ticks_get_header(ticks_file_t* handle, ticks_header_t* out_asset_class);

/**
 * @brief Retrieves the index offset from the file handle.
 * @param handle The file stream handle.
 * @param out_offset Pointer to store the result.
 * @return Status code indicating success or failure (0 = OK).
 */
ticks_status_e ticks_get_index_offset(ticks_file_t* handle, uint64_t* out_offset);
/**
 * @brief Retrieves the index size from the file handle.
 * @param handle The file stream handle.
 * @param out_size Pointer to store the result.
 * @return Status code indicating success or failure (0 = OK).
*/
ticks_status_e ticks_get_index_size(ticks_file_t* handle, uint64_t* out_size);

/**
 * @brief Adds trade ticks to the file, creating chunks as needed.
 *        The file must have been created with schema_id == SCHEMA_TRADE.
 * @param handle The file stream handle.
 * @param data Pointer to the array of trade_data_t entries to add.
 * @param num_entries Number of entries in the data array.
 * @return Status code (0 = OK; TICKS_ERROR_SCHEMA_MISMATCH if the file is not a
 *         trade file; TICKS_ERROR_UNSORTED_DATA if timestamps decrease).
 */
ticks_status_e ticks_add_data(ticks_file_t* handle, trade_data_t* data, uint64_t num_entries);

/**
 * @brief Adds quote (BBO) ticks to the file, creating chunks as needed.
 *        The file must have been created with schema_id == SCHEMA_QUOTE.
 * @param handle The file stream handle.
 * @param data Pointer to the array of quote_data_t entries to add.
 * @param num_entries Number of entries in the data array.
 * @return Status code (0 = OK; TICKS_ERROR_SCHEMA_MISMATCH if the file is not a
 *         quote file; TICKS_ERROR_UNSORTED_DATA if timestamps decrease).
 */
ticks_status_e ticks_add_quotes(ticks_file_t* handle, quote_data_t* data, uint64_t num_entries);

/**
 * @brief Verifies the integrity of every chunk by recomputing its CRC32 and
 *        comparing it against the value stored in the index.
 * @param handle The file stream handle (must be open for reading).
 * @return TICKS_OK if all chunks are intact, TICKS_ERROR_CORRUPT_DATA on a
 *         checksum mismatch, or an I/O error code.
 */
ticks_status_e ticks_verify(ticks_file_t* handle);

/**
 * @brief Converts a ticks_status_e code to a human-readable string.
 * @param status The status code to convert.
 * @return A string representation of the status code.
 */
const char* ticks_status_to_string(ticks_status_e status);

/*
* @brief Creates an iterator for traversing records within a specified time range
* @param handle Pointer to the ticks file handle
* @param from Start time (inclusive)
* @param to End time (exclusive)
* @param out_iterator Pointer to store the resulting iterator
* @return Status code indicating success or failure (0 = OK)
*/
ticks_status_e ticks_iterator_create(ticks_file_t* handle, time_t from, time_t to, ticks_iterator_t** out_iterator);

/*
* @brief Returns the next tick within the iterator's [from, to) range.
*
* Records are produced in ascending time order. Chunks whose time span falls
* entirely outside the range are skipped using the index alone (no decode),
* including the final chunk, which is bounded by its chunk_last_timestamp.
*
* @param iterator Pointer to the iterator.
* @param out_record Pointer to store the next tick (timestamps in ms).
* @return TICKS_OK if a record was produced, TICKS_EOF when the range is
*         exhausted, TICKS_ERROR_SCHEMA_MISMATCH if the file is not a trade
*         file, or an error code on I/O / format failure.
*/
ticks_status_e ticks_iterator_next(ticks_iterator_t* iterator, trade_data_t* out_record);

/*
* @brief Returns the next quote tick within the iterator's [from, to) range.
*        The file must have schema_id == SCHEMA_QUOTE. Same semantics and chunk
*        pruning as ticks_iterator_next.
* @param iterator Pointer to the iterator.
* @param out_record Pointer to store the next quote (timestamps in ms).
* @return TICKS_OK, TICKS_EOF, TICKS_ERROR_SCHEMA_MISMATCH, or an error code.
*/
ticks_status_e ticks_iterator_next_quote(ticks_iterator_t* iterator, quote_data_t* out_record);

/*
* @brief Destroys the iterator and frees associated resources
* @param iterator Pointer to the iterator to destroy
* @return Status code indicating success or failure (0 = OK)
*/
ticks_status_e ticks_iterator_destroy(ticks_iterator_t* iterator);

// Compression is configured per file via ticks_header_t.compression_type
// (COMPRESSION_NONE / COMPRESSION_ZSTD) at ticks_new_file time; chunks are then
// transparently (de)compressed by the add/iterate paths. See docs/ticks-format.md §4.
#endif // TICKSIO_H