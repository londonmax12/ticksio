#ifndef TICKS_CONSTANTS_H
#define TICKS_CONSTANTS_H

#ifdef __cplusplus
extern "C" {
#endif

// Internal constants for ticksio library

// --- Header constants ---
#define TICKS_MAGIC "TICK"
#define TICKS_TICKER_SIZE 8
#define TICKS_CURRENCY_SIZE 3
#define TICKS_COUNTRY_SIZE 2

// --- Chunking constants ---
#define MAX_CHUNK_SIZE 16777216 // 16 MB

// --- CSV constants ---
#define CSV_MAX_TIMESTAMP_LEN 30
#define CSV_MAX_LINE_LEN 1024
// TODO: Make this adjustable based on available memory and remove this
#define CSV_DEFAULT_CHUNK_SIZE 10000

#endif // TICKS_CONSTANTS_H