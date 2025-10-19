#include "ticksio/ticksio.h"
#include "ticksio/csv.h"
#include "ticksio/ticksio_internal.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

static void print_error(const char* func_name, ticks_status_e status) {
    const char* status_str = ticks_status_to_string(status);
    fprintf(stderr, "ERROR in %s: %s\n", func_name, status_str);
}

int main() {
    const char* test_filename = "test_data.ticks";

    printf("--- Creating File ---\n");

    ticks_header_t* new_header = malloc(sizeof(ticks_header_t));
    memset(new_header, 0, sizeof(ticks_header_t));

    strcpy(new_header->ticker, "AAPL");
    strcpy(new_header->currency, "USD");
    strcpy(new_header->country, "US");
    new_header->asset_class = 10;
    new_header->compression_type = 0;

    ticks_file_t* create_handle = NULL;
    ticks_status_e create_open_status = ticks_new_file(test_filename, new_header, &create_handle);

    free(new_header);

    if (create_handle == NULL || create_handle->file_stream == NULL) {
        print_error("ticks_create", create_open_status);
        return EXIT_FAILURE;
    }

    printf("File created successfully: %s\n", test_filename);
        
    printf("\n--- Reading CSV ---\n");
    
    csv_read_result_t reader;
    memset(&reader, 0, sizeof(reader));
    
    ticks_status_e read_status;
    while ((read_status = read_csv("random_tick_datass.csv", &reader)) == TICKS_OK) {
        printf("Adding %llu records to ticks file...\n", reader.records_in_buffer);
        ticks_status_e add_data_result = ticks_add_data(create_handle, reader.buffer, reader.records_in_buffer);
        if (add_data_result != TICKS_OK) {
            print_error("ticks_add_data", add_data_result);
            csv_reader_cleanup(&reader);
            ticks_close(create_handle);
            return EXIT_FAILURE;
        }
        
        if (reader.is_full_load) break;
    }
    
    csv_reader_cleanup(&reader);

    if (read_status != TICKS_EOF) {
        print_error("read_csv", read_status);
        ticks_close(create_handle);
        return EXIT_FAILURE;
    }

    ticks_status_e create_close_status = ticks_close(create_handle);
    if (create_close_status != TICKS_OK) {
        print_error("ticks_close (create)", create_close_status);
        return TICKS_OK;
    }
    printf("Data added and file closed successfully.\n");

    printf("\n--- Opening File ---\n");
    ticks_file_t* read_handle = NULL;
    ticks_status_e open_status = ticks_open_read(test_filename, &read_handle);

    if (read_handle == NULL || open_status != TICKS_OK) {
        print_error("ticks_open", open_status);
        return EXIT_FAILURE;
    }

    printf("File opened successfully.\n");

    printf("\n--- Retrieving Read File Metadata ---\n");
    ticks_header_t ticks_header;

    ticks_status_e get_header_status = ticks_get_header(read_handle, &ticks_header);
    if (get_header_status == TICKS_OK) {
        printf("Header Information\n");
        printf("├── Asset Class: %hu\n", ticks_header.asset_class);
        printf("├── Ticker: %s\n", ticks_header.ticker);
        printf("├── Currency: %s\n", ticks_header.currency);
        printf("├── Country: %s\n", ticks_header.country);
        printf("└── Compression Type: %hu\n", ticks_header.compression_type);
    } else {
        print_error("ticks_get_asset_class", get_header_status);
    }

    printf("\nIndex Information\n");
    uint64_t index_offset, index_size;
    ticks_status_e get_index_offset_status = ticks_get_index_offset(read_handle, &index_offset);
    if (index_offset == 0 || get_index_offset_status != TICKS_OK)
        printf("├── Index Offset: %llu\n", (unsigned long long)index_offset);
    else
        print_error("ticks_get_index_offset", get_index_offset_status);

    ticks_status_e get_index_size_status = ticks_get_index_size(read_handle, &index_size);
    if  (get_index_size_status == TICKS_OK)
        if (index_size == 0)
            printf("└── Index Size: %llu\n", (unsigned long long)index_size);
        else
            printf("├── Index Size: %llu (%llu Entries)\n", (unsigned long long)index_size , (unsigned long long)(index_size / sizeof(ticks_index_entry_t)));
    else
        print_error("ticks_get_index_size", get_index_size_status);

    if (index_size > 0 && read_handle->index.entries != NULL) {
        printf("└── First Index Entry:\n");
        printf("    ├── Time Base: %llu\n", (unsigned long long)read_handle->index.entries[0].chunk_time_base);
        printf("    ├── Offset: %llu\n", (unsigned long long)read_handle->index.entries[0].chunk_offset);
        printf("    ├── Size: %u\n", read_handle->index.entries[0].chunk_size);
        printf("    ├── Timestamp Size: %u\n", read_handle->index.entries[0].timestamp_size);
        printf("    ├── Price Size: %u\n", read_handle->index.entries[0].price_size);
        printf("    └── Volume Size: %u\n", read_handle->index.entries[0].volume_size);
    }

    ticks_status_e read_close_status = ticks_close(read_handle);
    if (read_close_status != TICKS_OK) {
        print_error("ticks_close (read)", TICKS_OK);
        return EXIT_FAILURE;
    }

    printf("Files closed and handles freed successfully.\n");

    return EXIT_SUCCESS;
}