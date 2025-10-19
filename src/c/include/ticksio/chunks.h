#ifndef TICKSIO_CHUNKS_H
#define TICKSIO_CHUNKS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ticksio/types.h"
#include "ticksio/ticksio_internal.h"
#include "ticksio/constants.h"
#include "ticksio/helpers.h"

int create_chunks(ticks_file_t* handle, const trade_data_t* entries, uint64_t num_entries);

#endif // TICKSIO_CHUNKS_H