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

/**
 * @brief Single function that handles both full and chunked loading automatically
 * @return Error code (0 = OK)
*/
ticks_status_e read_csv(const char* filename, csv_read_result_t* result);

/**
 * @brief Cleans up CSV reader resources
*/
void csv_reader_cleanup(csv_read_result_t* result);

#endif // TICKSIO_CSV_H