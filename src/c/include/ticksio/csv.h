#ifndef TICKSIO_CSV_H
#define TICKSIO_CSV_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ticksio/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    trade_data_t* buffer;
    uint64_t total_records;
    uint64_t records_in_buffer;
    uint64_t current_chunk;
    uint8_t is_completed;
    uint8_t is_full_load;
    FILE* file_handle;
} csv_read_result_t;

typedef enum {
    CSV_READ_SUCCESS = 0,
    CSV_READ_ERROR = -1,
    CSV_READ_EOF = -2,
    CSV_READ_INVALID_ARGS = -3
} csv_read_status_t;

/**
 * @brief Counts total records in CSV file (excluding header)
*/
uint64_t count_csv_records(FILE *fp);

/**
 * @brief Reads a chunk of trade data from CSV
*/
int read_csv_chunk(FILE *fp, trade_data_t *buffer, int max_records);

/**
 * @brief Attempts to load entire CSV file into memory at once
 * @return CSV_READ_SUCCESS if full load successful, CSV_READ_ERROR if failed
*/
csv_read_status_t attempt_full_load(csv_read_result_t* result, const char* filename);

/**
 * @brief Initializes CSV reader - attempts full load first, falls back to chunked
*/
csv_read_status_t csv_reader_init(csv_read_result_t* result, const char* filename);

/**
 * @brief Reads next chunk of CSV data (only used in chunked mode)
*/
csv_read_status_t csv_read_next_chunk(csv_read_result_t* result, size_t chunk_size);

/**
 * @brief Single function that handles both full and chunked loading automatically
 * @return CSV_READ_SUCCESS if more data available, CSV_READ_EOF if done, CSV_READ_ERROR on failure
*/
csv_read_status_t read_csv(const char* filename, csv_read_result_t* result);

/**
 * @brief Cleans up CSV reader resources
*/
void csv_reader_cleanup(csv_read_result_t* result);

#endif // TICKSIO_CSV_H