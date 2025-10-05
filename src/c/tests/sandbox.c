#include "ticksio.h"
#include <stdio.h>

int main() {
    ticks_header_t header = {
        .ticker = "GBPJPY",
        .currency = "USD",
        .asset_class = 1,
        .country = "AU",
        .compression_type = 0
    };

    if (ticks_create("test.ticks", &header) == 0)
        printf("File created successfully!\n");

    return 0;
}
