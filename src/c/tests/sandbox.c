#include "ticksio/ticksio.h"
#include "ticksio/csv.h"
#include "ticksio/ticksio_internal.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

static void print_error(const char *func_name) {
    fprintf(stderr, "ERROR in %s: %s\n", func_name, strerror(errno));
}

int main() {
    const char *test_filename = "test_data.ticks";

    printf("--- Creating File ---\n");
    ticks_header_t my_header = {0};
    strcpy(my_header.ticker, "AAPL");
    strcpy(my_header.currency, "USD");
    strcpy(my_header.country, "US");
    my_header.asset_class = 10;
    my_header.compression_type = 0;

    ticks_file_t* create_handle = ticks_create(test_filename, &my_header);

    if (create_handle == NULL || create_handle->file_stream == NULL) {
        print_error("ticks_create");
        return EXIT_FAILURE;
    }

    printf("File created successfully: %s\n", test_filename);
    
    printf("\n--- Reading CSV ---\n");
    
    csv_read_result_t reader;
    memset(&reader, 0, sizeof(reader));
    
    while (read_csv("random_tick_data.csv", &reader, 10000) == CSV_READ_SUCCESS) {
        printf("Adding %llu records to ticks file...\n", reader.records_in_buffer);
        if (ticks_add_data(create_handle, reader.buffer, reader.records_in_buffer) != EXIT_SUCCESS) {
            print_error("ticks_add_data");
            csv_reader_cleanup(&reader);
            ticks_close(create_handle);
            return EXIT_FAILURE;
        }
        
        if (reader.is_full_load) break;
    }
    
    csv_reader_cleanup(&reader);

    printf("\n--- Opening File ---\n");
    ticks_file_t* read_handle = ticks_open(test_filename);

    if (read_handle == NULL) {
        print_error("ticks_open");
        return EXIT_FAILURE;
    }

    printf("File opened successfully.\n");

    printf("\n--- Retrieving Read File Metadata ---\n");
    ticks_header_t ticks_header;

    if (ticks_get_header(read_handle, &ticks_header) == 0) {
        printf("Header Information:\n");
        printf(" - Asset Class: %hu\n", ticks_header.asset_class);
        printf(" - Ticker: %s\n", ticks_header.ticker);
        printf(" - Currency: %s\n", ticks_header.currency);
        printf(" - Country: %s\n", ticks_header.country);
        printf(" - Compression Type: %hu\n", ticks_header.compression_type);
    } else {
        print_error("ticks_get_asset_class");
    }

    printf("Index Offset and Size:\n");
    uint64_t index_offset, index_size;
    if (ticks_get_index_offset(read_handle, &index_offset) == 0) {
        printf(" - Index Offset: %llu\n", (unsigned long long)index_offset);
    } else {
        print_error("ticks_get_index_offset");
    }
    if (ticks_get_index_size(read_handle, &index_size) == 0) {
        printf(" - Index Size: %llu\n", (unsigned long long)index_size);
    } else {
        print_error("ticks_get_index_size");
    }

    
    if (ticks_close(read_handle) != 0) {
        print_error("ticks_close (read)");
        return EXIT_FAILURE;
    }

    printf("Files closed and handles freed successfully.\n");

    return EXIT_SUCCESS;
}