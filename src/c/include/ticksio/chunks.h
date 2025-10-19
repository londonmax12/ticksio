#ifndef TICKSIO_CHUNKS_H
#define TICKSIO_CHUNKS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ticksio/types.h"
#include "ticksio/ticksio_internal.h"
#include "ticksio/constants.h"
#include "ticksio/helpers.h"

/*
* @brief Add trade data as chunks in file
* @param row_index Pointer to the current index in entries array
* @param entries Array of trade_data_t entries
* @param num_entries Total number of entries in the array
* @return Error code (OK = 0)
*/
ticks_status_e create_chunks(ticks_file_t* handle, const trade_data_t* entries, uint64_t num_entries);

#endif // TICKSIO_CHUNKS_H