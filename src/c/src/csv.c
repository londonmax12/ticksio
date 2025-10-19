#include "ticksio/csv.h"

#include "ticksio/constants.h"

// Helper function to convert timestamp string to milliseconds since epoch
static uint64_t timestamp_to_ms(const char *timestamp_str) {
    struct tm tm_time;
    memset(&tm_time, 0, sizeof(struct tm));
    int year, month, day, hour, min, sec, ms = 0;

    int matched = sscanf(
        timestamp_str,
        "%d-%d-%d %d:%d:%d.%d",
        &year, &month, &day, &hour, &min, &sec, &ms
    );

    if (matched < 6) { // At least need YYYY-MM-DD HH:MM:SS
        fprintf(stderr, "Timestamp parsing failed for: %s\n", timestamp_str);
        return 0;
    }

    tm_time.tm_year = year - 1900;
    tm_time.tm_mon = month - 1;
    tm_time.tm_mday = day;
    tm_time.tm_hour = hour;
    tm_time.tm_min = min;
    tm_time.tm_sec = sec;
    tm_time.tm_isdst = -1;

    time_t seconds_since_epoch = mktime(&tm_time);

    if (seconds_since_epoch == (time_t)-1) {
        fprintf(stderr, "Error converting time_t for: %s\n", timestamp_str);
        return 0;
    }

    return (uint64_t)seconds_since_epoch * 1000 + (matched == 7 ? ms : 0);
}

// Helper function to skip the CSV header line
static ticks_status_e skip_header(FILE *fp) {
    if (!fp) return TICKS_ERROR_INVALID_ARGUMENTS;
    
    char line[CSV_MAX_LINE_LEN];
    if (fgets(line, sizeof(line), fp) == NULL) {
        return TICKS_ERROR_FILE_IO;
    }
    return TICKS_OK;
}

static uint64_t count_csv_records(FILE *fp)
{
    if (!fp) return 0;

    long current_pos = ftell(fp); // Save current position
    
    fseek(fp, 0, SEEK_SET);
    skip_header(fp); // Skip header

    uint64_t count = 0;
    char buffer[CSV_MAX_LINE_LEN];
    
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        // Basic validation
        // TODO: More robust validation
        if (strlen(buffer) > 5) { // Arbitrary minimum length
            count++;
        }
    }

    fseek(fp, current_pos, SEEK_SET); // Restore position
    return count;
}

static int read_csv_chunk(FILE *fp, trade_data_t *buffer, int max_records)
{
    if (!fp || !buffer || max_records <= 0) {
        return -1;
    }

    char line[CSV_MAX_LINE_LEN];
    char temp_timestamp[CSV_MAX_TIMESTAMP_LEN];
    int records_read = 0;

    while (records_read < max_records && fgets(line, sizeof(line), fp)) {
        // Skip empty lines
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0') {
            continue;
        }

        int items_matched = sscanf(
            line,
            "%[^,],%lf,%d",
            temp_timestamp,
            &buffer[records_read].price,
            &buffer[records_read].volume
        );

        if (items_matched == 3) {
            buffer[records_read].ms_since_epoch = timestamp_to_ms(temp_timestamp);
            if (buffer[records_read].ms_since_epoch != 0) {
                records_read++;
            } else {
                fprintf(stderr, "Timestamp conversion failed for: %s", line);
            }
        } else if (items_matched > 0) {
            fprintf(stderr, "Incomplete data in line: %s", line);
        }
    }

    return records_read;
}

static ticks_status_e attempt_full_load(csv_read_result_t *result, const char *filename)
{
     printf("Attempting full file load...\n");
    
    FILE* fp = fopen(filename, "r");
    if (!fp) {
        perror("Error opening file");
        return TICKS_ERROR_FILE_IO;
    }

    // Count total records
    result->total_records = count_csv_records(fp);
    if (result->total_records == 0) {
        fclose(fp);
        fprintf(stderr, "File contains no data records\n");
        return TICKS_ERROR_INVALID_FORMAT;
    }

    // Calculate memory requirement
    size_t required_memory = result->total_records * sizeof(trade_data_t);
    double memory_mb = (double)required_memory / (1024.0 * 1024.0);
    
    printf("Loading %llu records (%.2f MB) into memory...\n",  result->total_records, memory_mb);

    // Attempt to allocate memory for all records
    result->buffer = (trade_data_t*)malloc(required_memory);
    if (!result->buffer) {
        fclose(fp);
        fprintf(stderr, "Full load failed: Cannot allocate %.2f MB. Falling back to chunked loading.\n", memory_mb);
        return TICKS_ERROR_MEMORY_ALLOCATION;
    }

    // Read all data at once
    fseek(fp, 0, SEEK_SET);
    skip_header(fp);
    
    int records_read = read_csv_chunk(fp, result->buffer, (int)result->total_records);
    fclose(fp);

    if (records_read != (int)result->total_records) {
        fprintf(stderr, "Warning: Expected %llu records but read %d\n", result->total_records, records_read);
        result->total_records = records_read;
    }

    result->records_in_buffer = result->total_records;
    result->is_full_load = 1;
    result->is_completed = 1;
    result->current_chunk = 1;
    
    printf("Full load successful! Loaded %llu records.\n", result->records_in_buffer);
    return TICKS_OK;
}

static ticks_status_e csv_reader_init(csv_read_result_t *result, const char *filename)
{
    if (!result || !filename) {
        return TICKS_ERROR_INVALID_ARGUMENTS;
    }

    memset(result, 0, sizeof(csv_read_result_t));
    
    // First attempt full load for maximum performance
    if (attempt_full_load(result, filename) == TICKS_OK) {
        return TICKS_OK;
    }
    
    // Fall back to chunked loading
    printf("Falling back to chunked loading...\n");
    result->file_handle = fopen(filename, "r");
    if (!result->file_handle) {
        perror("Error: Failed to open file");
        return TICKS_ERROR_FILE_IO;
    }

    // Count total records for progress reporting
    result->total_records = count_csv_records(result->file_handle);
    if (result->total_records == 0) {
        fclose(result->file_handle);
        result->file_handle = NULL;
        return TICKS_ERROR_INVALID_FORMAT;
    }

    printf("File contains %llu records, loading in chunks...\n", result->total_records);
    
    // Reset to beginning after counting
    fseek(result->file_handle, 0, SEEK_SET);
    skip_header(result->file_handle);

    return TICKS_OK;
}

static ticks_status_e csv_read_next_chunk(csv_read_result_t *result, size_t chunk_size)
{
    if (!result) {
        return TICKS_ERROR_INVALID_ARGUMENTS;
    }

    // If we did a full load, we're already done
    if (result->is_full_load) {
        return result->is_completed ? TICKS_EOF : TICKS_OK;
    }

    // Chunked loading logic
    if (!result->file_handle || result->is_completed) {
        return TICKS_EOF;
    }

    // Free previous buffer if it exists
    if (result->buffer) {
        free(result->buffer);
        result->buffer = NULL;
    }

    // Allocate buffer for chunk
    result->buffer = (trade_data_t*)malloc(chunk_size * sizeof(trade_data_t));
    if (!result->buffer) {
        fprintf(stderr, "Failed to allocate memory for chunk\n");
        return TICKS_ERROR_MEMORY_ALLOCATION;
    }

    // Read chunk
    int records_read = read_csv_chunk(result->file_handle, result->buffer, (int)chunk_size);
    if (records_read < 0) {
        free(result->buffer);
        result->buffer = NULL;
        return TICKS_ERROR_FILE_IO;
    }

    result->records_in_buffer = records_read;
    result->current_chunk++;

    // Show progress for chunked loading
    uint64_t total_loaded = (result->current_chunk - 1) * chunk_size + records_read;
    printf("Chunk %llu: loaded %llu records (%.1f%%)\n", 
           result->current_chunk, total_loaded,
           (double)total_loaded / result->total_records * 100.0);

    if (records_read == 0 || feof(result->file_handle)) {
        result->is_completed = 1;
        fclose(result->file_handle);
        result->file_handle = NULL;
        return TICKS_EOF;
    }

    return TICKS_OK;
}

ticks_status_e read_csv(const char *filename, csv_read_result_t *result)
{
    if (!result) {
        return TICKS_ERROR_INVALID_ARGUMENTS;
    }

    // First call - initialize
    if (result->file_handle == NULL && !result->is_full_load) {
        return csv_reader_init(result, filename);
    }

    // Subsequent calls - read next chunk if in chunked mode
    // TODO: Dynamically adjust chunk size based on available memory for better performance
    if (!result->is_full_load && !result->is_completed) {
        return csv_read_next_chunk(result, MAX_CHUNK_SIZE);
    }

    // If we did a full load, first call returns all data, subsequent calls return EOF
    return result->is_full_load && result->current_chunk == 0 ? TICKS_OK: TICKS_EOF;
}

void csv_reader_cleanup(csv_read_result_t* result)
{
    if (!result) return;

    if (result->file_handle) {
        fclose(result->file_handle);
        result->file_handle = NULL;
    }

    if (result->buffer) {
        free(result->buffer);
        result->buffer = NULL;
    }

    memset(result, 0, sizeof(csv_read_result_t));
}