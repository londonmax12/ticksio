// Standalone round-trip test for the v2 columnar/delta chunk format.
// Generates synthetic ticks, writes them via the public API, then reopens the
// file and independently decodes the raw chunk bytes using the documented
// on-disk layout, checking that every value reconstructs exactly.
#include "ticksio/ticksio.h"
#include "ticksio/ticksio_internal.h"
#include "ticksio/ticksio_constants.h"
#include "ticksio/ticksio_helpers.h"
#include "ticksio/ticksio_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 6000000ULL

static uint64_t read_uint(const uint8_t* p, size_e size) {
    switch (size) {
        case SIZE_8BIT:  return p[0];
        case SIZE_16BIT: return le_get_u16(p);
        case SIZE_32BIT: return le_get_u32(p);
        default:         return le_get_u64(p);
    }
}

int main(void) {
    const char* fn = "roundtrip.ticks";

    // --- Synthetic data: large absolute values, small movements. ---
    // Absolute prices need 4 bytes; deltas should need far fewer. This is the
    // whole point of delta encoding, so the test asserts the size win too.
    trade_data_t* in = malloc(N * sizeof(trade_data_t));
    if (!in) { fprintf(stderr, "alloc failed\n"); return 1; }

    uint64_t ts = 1700000000000ULL; // ms since epoch
    int64_t price = 4000000;        // $40000.00 in cents
    int64_t volume = 1000000;
    uint32_t rng = 12345u;
    for (uint64_t i = 0; i < N; i++) {
        rng = rng * 1103515245u + 12345u;       // LCG
        int step = (int)((rng >> 16) % 7) - 3;  // -3..+3
        ts += (rng >> 5) % 4;                   // 0..3 ms gap (non-decreasing)
        price += step;                          // random walk, both directions
        if (price < 1) price = 1;
        volume += ((int)((rng >> 9) % 201)) - 100; // -100..+100
        if (volume < 0) volume = 0;
        in[i].ms_since_epoch = ts;
        in[i].price = (uint64_t)price;
        in[i].volume = (uint64_t)volume;
    }

    ticks_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    strcpy(hdr.ticker, "TEST");
    strcpy(hdr.currency, "USD");
    strcpy(hdr.country, "US");
    hdr.asset_class = ASSET_CLASS_STOCK;
    hdr.compression_type = COMPRESSION_NONE;
    hdr.price_scale = -2;
    hdr.volume_scale = 0;

    ticks_file_t* w = NULL;
    if (ticks_new_file(fn, &hdr, &w) != TICKS_OK) { fprintf(stderr, "new_file failed\n"); return 1; }
    ticks_status_e s = ticks_add_data(w, in, N);
    if (s != TICKS_OK) { fprintf(stderr, "add_data: %s\n", ticks_status_to_string(s)); return 1; }
    ticks_close(w);

    // --- Reopen and verify integrity + scales survived the round-trip. ---
    ticks_file_t* r = NULL;
    if (ticks_open_read(fn, &r) != TICKS_OK) { fprintf(stderr, "open_read failed\n"); return 1; }
    ticks_header_t got;
    ticks_get_header(r, &got);
    if (got.version != TICKS_VERSION || got.price_scale != -2 || got.volume_scale != 0) {
        fprintf(stderr, "FAIL: header mismatch (v=%u ps=%d vs=%d)\n", got.version, got.price_scale, got.volume_scale);
        return 1;
    }
    if (ticks_verify(r) != TICKS_OK) { fprintf(stderr, "FAIL: CRC verify\n"); return 1; }

    // --- Independently decode every chunk from raw bytes and compare. ---
    uint64_t total_records = 0, on_disk_bytes = 0;
    int num_chunks = (int)r->index.num_entries;
    for (uint32_t c = 0; c < r->index.num_entries; c++) {
        const ticks_index_entry_t* e = &r->index.entries[c];
        uint8_t* buf = malloc(e->chunk_size);
        ticks_fseek64(r->file_stream, (int64_t)e->chunk_offset, SEEK_SET);
        if (fread(buf, 1, e->chunk_size, r->file_stream) != e->chunk_size) {
            fprintf(stderr, "FAIL: short read on chunk %u\n", c); return 1;
        }

        uint32_t nrec   = le_get_u32(buf + 0);
        uint64_t tbase  = le_get_u64(buf + 4);
        uint64_t pbase  = le_get_u64(buf + 12);
        uint64_t vbase  = le_get_u64(buf + 20);
        size_e ts_w = buf[28], p_w = buf[29], v_w = buf[30];

        // Index width bytes must mirror the chunk header (redundant accelerator).
        if (ts_w != e->timestamp_size || p_w != e->price_size || v_w != e->volume_size || tbase != e->chunk_time_base) {
            fprintf(stderr, "FAIL: index/chunk header disagree on chunk %u\n", c); return 1;
        }

        const uint8_t* ts_col = buf + TICKS_CHUNK_HEADER_DISK_SIZE;
        const uint8_t* p_col  = ts_col + (uint64_t)(nrec - 1) * ts_w;
        const uint8_t* v_col  = p_col  + (uint64_t)(nrec - 1) * p_w;

        uint64_t t = tbase, p = pbase, v = vbase;
        for (uint32_t i = 0; i < nrec; i++) {
            if (i > 0) {
                t += read_uint(ts_col + (uint64_t)(i - 1) * ts_w, ts_w);
                p = (uint64_t)((int64_t)p + zigzag_decode(read_uint(p_col + (uint64_t)(i - 1) * p_w, p_w)));
                v = (uint64_t)((int64_t)v + zigzag_decode(read_uint(v_col + (uint64_t)(i - 1) * v_w, v_w)));
            }
            const trade_data_t* exp = &in[total_records + i];
            if (t != exp->ms_since_epoch || p != exp->price || v != exp->volume) {
                fprintf(stderr, "FAIL: record %llu mismatch: t %llu/%llu p %llu/%llu v %llu/%llu\n",
                        (unsigned long long)(total_records + i),
                        (unsigned long long)t, (unsigned long long)exp->ms_since_epoch,
                        (unsigned long long)p, (unsigned long long)exp->price,
                        (unsigned long long)v, (unsigned long long)exp->volume);
                return 1;
            }
        }
        total_records += nrec;
        on_disk_bytes += e->chunk_size;
        free(buf);
    }

    if (total_records != N) {
        fprintf(stderr, "FAIL: record count %llu != %llu\n", (unsigned long long)total_records, N);
        return 1;
    }

    double naive = (double)N * 24.0; // 3 x uint64 per record, uncompressed
    printf("PASS: %llu records over %d chunk(s) decoded exactly; CRC ok.\n",
           (unsigned long long)total_records, num_chunks);
    printf("On-disk chunk bytes: %llu  (naive 24B/rec: %.0f)  ratio: %.2fx smaller\n",
           (unsigned long long)on_disk_bytes, naive, naive / (double)on_disk_bytes);

    ticks_close(r);
    free(in);
    remove(fn);
    return 0;
}
