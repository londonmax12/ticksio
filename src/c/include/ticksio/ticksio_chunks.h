#ifndef TICKSIO_CHUNKS_H
#define TICKSIO_CHUNKS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ticksio/ticksio_types.h"
#include "ticksio/ticksio_internal.h"
#include "ticksio/ticksio_constants.h"
#include "ticksio/ticksio_helpers.h"

/*
* @brief Encode records into chunks and append them to the file.
* @param handle The ticks file handle.
* @param values Row-major record values: record i, column j at values[i*ncols+j]
*               (ncols == schema->num_columns; column 0 is the timestamp in ms).
* @param num_entries Total number of records in `values`.
* @param schema The record schema (column count + per-column encodings).
* @return Error code (OK = 0)
*/
ticks_status_e create_chunks(ticks_file_t* handle, const uint64_t* values, uint64_t num_entries,
                             const ticks_schema_t* schema);

#endif // TICKSIO_CHUNKS_H