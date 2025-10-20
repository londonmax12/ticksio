#ifndef TICKSIO_HELPERS_H
#define TICKSIO_HELPERS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ticksio/ticksio_types.h"

size_e determine_min_size_uint64(uint64_t value);
int is_little_endian();

#endif // TICKSIO_HELPERS_H