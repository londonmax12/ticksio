#ifndef TICKSIO_H
#define TICKSIO_H

#include <stdint.h>
#include <stdio.h>

#define TICKS_MAGIC "TICK"
#define TICKS_TICKER_SIZE 8
#define TICKS_CURRENCY_SIZE 3
#define TICKS_COUNTRY_SIZE 2

typedef struct {
    char ticker[TICKS_TICKER_SIZE];
    char currency[TICKS_CURRENCY_SIZE];
    uint16_t asset_class;
    char country[TICKS_COUNTRY_SIZE];
    uint16_t compression_type;
} ticks_header_t;

// API
int ticks_create(const char *filename, const ticks_header_t *header);
int ticks_open(const char *filename, FILE **out_file);
int ticks_close(FILE *file);

#endif
