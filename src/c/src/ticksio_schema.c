#include "ticksio/ticksio_schema.h"

#include <stddef.h>

// Per-column delta encodings for each schema. Column 0 is always the timestamp
// (non-decreasing → unsigned delta); the rest are signed values → zig-zag.
static const col_encoding_e TRADE_ENCODINGS[] = {
    COL_ENC_DELTA_UNSIGNED, // timestamp
    COL_ENC_DELTA_ZIGZAG,   // price
    COL_ENC_DELTA_ZIGZAG    // volume
};
static const col_encoding_e QUOTE_ENCODINGS[] = {
    COL_ENC_DELTA_UNSIGNED, // timestamp
    COL_ENC_DELTA_ZIGZAG,   // bid
    COL_ENC_DELTA_ZIGZAG,   // ask
    COL_ENC_DELTA_ZIGZAG,   // bid_size
    COL_ENC_DELTA_ZIGZAG    // ask_size
};

static const ticks_schema_t SCHEMAS[] = {
    { SCHEMA_TRADE, "trade", 3, TRADE_ENCODINGS },
    { SCHEMA_QUOTE, "quote", 5, QUOTE_ENCODINGS }
};

const ticks_schema_t* ticks_schema_lookup(schema_id_e id) {
    for (size_t i = 0; i < sizeof(SCHEMAS) / sizeof(SCHEMAS[0]); i++)
        if (SCHEMAS[i].id == id)
            return &SCHEMAS[i];
    return NULL;
}
