#ifndef INDEX_H
#define INDEX_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ticksio/ticksio_internal.h"
#include "ticksio/types.h"

/*
* @brief Creates the index in the ticks file
* @param handle Pointer to the ticks file handle
* @return Error code (0 = OK)
*/
ticks_status_e create_index(ticks_file_t* handle);

#endif // INDEX_H