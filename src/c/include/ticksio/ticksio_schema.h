#ifndef TICKSIO_SCHEMA_H
#define TICKSIO_SCHEMA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ticksio/ticksio_types.h"

// Returns the descriptor for a record schema (column count + per-column
// encodings), or NULL if the id is not a known schema. Column 0 of every schema
// is the timestamp (COL_ENC_DELTA_UNSIGNED); the remaining columns are signed
// values (COL_ENC_DELTA_ZIGZAG).
const ticks_schema_t* ticks_schema_lookup(schema_id_e id);

#ifdef __cplusplus
}
#endif

#endif // TICKSIO_SCHEMA_H
