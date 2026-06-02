// Standalone round-trip test for the v4 schema-aware columnar/delta format.
// For each schema (trade and quote) it: generates synthetic ticks, writes them
// via the typed API, reopens the file, verifies CRC + header summary, then
// independently decodes the raw chunk bytes using the documented generic layout
// and checks every value reconstructs. Finally it range-iterates and checks the
// emitted records match a reference scan.
#include "ticksio/ticksio.h"
#include "ticksio/ticksio_internal.h"
#include "ticksio/ticksio_constants.h"
#include "ticksio/ticksio_helpers.h"
#include "ticksio/ticksio_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t read_uint(const uint8_t* p, size_e size) {
    switch (size) {
        case SIZE_8BIT:  return p[0];
        case SIZE_16BIT: return le_get_u16(p);
        case SIZE_32BIT: return le_get_u32(p);
        default:         return le_get_u64(p);
    }
}

// Decode every chunk of an open file straight from its bytes (no schema lookup)
// and compare against expected[N * ncols] (row-major). Returns total on-disk
// chunk bytes via out_disk, or -1 on mismatch.
static long long verify_chunks(ticks_file_t* r, const uint64_t* expected, uint64_t N, uint8_t ncols) {
    uint64_t total = 0, disk = 0;
    for (uint32_t c = 0; c < r->index.num_entries; c++) {
        const ticks_index_entry_t* e = &r->index.entries[c];
        uint8_t* buf = malloc(e->chunk_size);
        ticks_fseek64(r->file_stream, (int64_t)e->chunk_offset, SEEK_SET);
        if (fread(buf, 1, e->chunk_size, r->file_stream) != e->chunk_size) {
            fprintf(stderr, "FAIL: short read on chunk %u\n", c); free(buf); return -1;
        }

        uint32_t nrec = le_get_u32(buf + 0);
        uint8_t nc = buf[4];
        if (nc != ncols) { fprintf(stderr, "FAIL: chunk %u ncols %u != %u\n", c, nc, ncols); free(buf); return -1; }

        uint64_t cur[TICKS_MAX_COLUMNS];
        size_e w[TICKS_MAX_COLUMNS];
        col_encoding_e enc[TICKS_MAX_COLUMNS];
        const uint8_t* col[TICKS_MAX_COLUMNS];
        const uint8_t* desc = buf + TICKS_CHUNK_HEADER_BASE_SIZE;
        const uint8_t* p = buf + TICKS_CHUNK_HEADER_SIZE(nc);
        const uint64_t nd = nrec - 1;
        for (uint8_t j = 0; j < nc; j++) {
            cur[j] = le_get_u64(desc + 0);
            w[j] = (size_e)desc[8];
            enc[j] = (col_encoding_e)desc[9];
            col[j] = p;
            p += nd * (uint64_t)w[j];
            desc += TICKS_CHUNK_COL_DESC_SIZE;
        }

        for (uint32_t i = 0; i < nrec; i++) {
            if (i > 0) {
                for (uint8_t j = 0; j < nc; j++) {
                    uint64_t raw = read_uint(col[j] + (uint64_t)(i - 1) * w[j], w[j]);
                    if (enc[j] == COL_ENC_DELTA_UNSIGNED) cur[j] += raw;
                    else cur[j] = (uint64_t)((int64_t)cur[j] + zigzag_decode(raw));
                }
            }
            for (uint8_t j = 0; j < nc; j++) {
                uint64_t want = expected[(total + i) * ncols + j];
                if (cur[j] != want) {
                    fprintf(stderr, "FAIL: rec %llu col %u: got %llu want %llu\n",
                            (unsigned long long)(total + i), j,
                            (unsigned long long)cur[j], (unsigned long long)want);
                    free(buf); return -1;
                }
            }
        }
        total += nrec;
        disk += e->chunk_size;
        free(buf);
    }
    if (total != N) { fprintf(stderr, "FAIL: total %llu != %llu\n", (unsigned long long)total, N); return -1; }
    return (long long)disk;
}

// Count records whose timestamp (column 0) falls in [from_ms, to_ms).
static uint64_t count_in_range(const uint64_t* v, uint64_t N, uint8_t ncols, uint64_t from_ms, uint64_t to_ms) {
    uint64_t n = 0;
    for (uint64_t i = 0; i < N; i++) {
        uint64_t t = v[i * ncols];
        if (t >= from_ms && t < to_ms) n++;
    }
    return n;
}

// --- Trade schema round-trip ---------------------------------------------
static int test_trades(void) {
    const char* fn = "roundtrip_trade.ticks";
    const uint64_t N = 6000000ULL;

    trade_data_t* in = malloc(N * sizeof(trade_data_t));
    if (!in) { fprintf(stderr, "alloc\n"); return 1; }

    uint64_t ts = 1700000000000ULL;
    int64_t price = 4000000, volume = 1000000;
    uint32_t rng = 12345u;
    for (uint64_t i = 0; i < N; i++) {
        rng = rng * 1103515245u + 12345u;
        ts += (rng >> 5) % 4;
        price += (int)((rng >> 16) % 7) - 3;
        if (price < 1) price = 1;
        volume += ((int)((rng >> 9) % 201)) - 100;
        if (volume < 0) volume = 0;
        in[i].ms_since_epoch = ts;
        in[i].price = (uint64_t)price;
        in[i].volume = (uint64_t)volume;
    }

    ticks_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    strcpy(hdr.ticker, "TEST"); strcpy(hdr.currency, "USD"); strcpy(hdr.country, "US");
    hdr.asset_class = ASSET_CLASS_STOCK;
    hdr.schema_id = SCHEMA_TRADE;
    hdr.price_scale = -2;

    ticks_file_t* w = NULL;
    if (ticks_new_file(fn, &hdr, &w) != TICKS_OK) { fprintf(stderr, "new_file\n"); return 1; }
    ticks_status_e s = ticks_add_data(w, in, N);
    if (s != TICKS_OK) { fprintf(stderr, "add_data: %s\n", ticks_status_to_string(s)); return 1; }
    ticks_close(w);

    ticks_file_t* r = NULL;
    if (ticks_open_read(fn, &r) != TICKS_OK) { fprintf(stderr, "open_read\n"); return 1; }
    ticks_header_t got; ticks_get_header(r, &got);
    if (got.version != TICKS_VERSION || got.schema_id != SCHEMA_TRADE ||
        got.record_count != N || got.price_scale != -2) {
        fprintf(stderr, "FAIL: trade header (v=%u schema=%u rc=%llu)\n",
                got.version, got.schema_id, (unsigned long long)got.record_count); return 1;
    }
    if (ticks_verify(r) != TICKS_OK) { fprintf(stderr, "FAIL: trade CRC\n"); return 1; }

    long long disk = verify_chunks(r, (const uint64_t*)in, N, 3);
    if (disk < 0) return 1;
    int chunks = (int)r->index.num_entries;

    // Range-iterate a slice and check it matches a reference scan.
    uint64_t first_s = in[0].ms_since_epoch / 1000;
    uint64_t from_s = first_s + 1000, to_s = first_s + 3000;
    uint64_t want = count_in_range((const uint64_t*)in, N, 3, from_s * 1000, to_s * 1000);

    ticks_iterator_t* it = NULL;
    if (ticks_iterator_create(r, (time_t)from_s, (time_t)to_s, &it) != TICKS_OK) {
        fprintf(stderr, "FAIL: trade iter create\n"); return 1;
    }
    uint64_t got_n = 0, prev_t = 0; trade_data_t rec; ticks_status_e ns;
    while ((ns = ticks_iterator_next(it, &rec)) == TICKS_OK) {
        if (rec.ms_since_epoch < from_s * 1000 || rec.ms_since_epoch >= to_s * 1000 || rec.ms_since_epoch < prev_t) {
            fprintf(stderr, "FAIL: trade iter record out of range/order\n"); return 1;
        }
        prev_t = rec.ms_since_epoch; got_n++;
    }
    ticks_iterator_destroy(it);
    if (ns != TICKS_EOF || got_n != want) {
        fprintf(stderr, "FAIL: trade iter count %llu != %llu (status %d)\n",
                (unsigned long long)got_n, (unsigned long long)want, ns); return 1;
    }

    ticks_close(r); free(in); remove(fn);
    printf("PASS trade: %llu recs / %d chunk(s) decoded exactly; CRC+summary ok; "
           "iterator %llu recs; %.2fx vs naive 24B.\n",
           (unsigned long long)N, chunks, (unsigned long long)got_n, (double)N * 24.0 / (double)disk);
    return 0;
}

// --- Quote schema round-trip ---------------------------------------------
static int test_quotes(void) {
    const char* fn = "roundtrip_quote.ticks";
    const uint64_t N = 4000000ULL;

    quote_data_t* in = malloc(N * sizeof(quote_data_t));
    if (!in) { fprintf(stderr, "alloc\n"); return 1; }

    uint64_t ts = 1700000000000ULL;
    int64_t bid = 4000000, ask = 4000100, bsz = 500, asz = 700;
    uint32_t rng = 99991u;
    for (uint64_t i = 0; i < N; i++) {
        rng = rng * 1103515245u + 12345u;
        ts += (rng >> 5) % 4;
        bid += (int)((rng >> 16) % 5) - 2;
        ask = bid + 50 + (int)((rng >> 11) % 100); // ask stays above bid
        bsz += (int)((rng >> 13) % 41) - 20; if (bsz < 0) bsz = 0;
        asz += (int)((rng >> 7) % 41) - 20; if (asz < 0) asz = 0;
        in[i].ms_since_epoch = ts;
        in[i].bid = (uint64_t)bid; in[i].ask = (uint64_t)ask;
        in[i].bid_size = (uint64_t)bsz; in[i].ask_size = (uint64_t)asz;
    }

    ticks_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    strcpy(hdr.ticker, "TEST"); strcpy(hdr.currency, "USD"); strcpy(hdr.country, "US");
    hdr.asset_class = ASSET_CLASS_STOCK;
    hdr.schema_id = SCHEMA_QUOTE;
    hdr.price_scale = -2;

    ticks_file_t* w = NULL;
    if (ticks_new_file(fn, &hdr, &w) != TICKS_OK) { fprintf(stderr, "new_file\n"); return 1; }

    // A trade add into a quote file must be rejected.
    trade_data_t bad = {0};
    if (ticks_add_data(w, &bad, 1) != TICKS_ERROR_SCHEMA_MISMATCH) {
        fprintf(stderr, "FAIL: expected SCHEMA_MISMATCH for trade-into-quote\n"); return 1;
    }

    ticks_status_e s = ticks_add_quotes(w, in, N);
    if (s != TICKS_OK) { fprintf(stderr, "add_quotes: %s\n", ticks_status_to_string(s)); return 1; }
    ticks_close(w);

    ticks_file_t* r = NULL;
    if (ticks_open_read(fn, &r) != TICKS_OK) { fprintf(stderr, "open_read\n"); return 1; }
    ticks_header_t got; ticks_get_header(r, &got);
    if (got.schema_id != SCHEMA_QUOTE || got.record_count != N) {
        fprintf(stderr, "FAIL: quote header\n"); return 1;
    }
    if (ticks_verify(r) != TICKS_OK) { fprintf(stderr, "FAIL: quote CRC\n"); return 1; }

    long long disk = verify_chunks(r, (const uint64_t*)in, N, 5);
    if (disk < 0) return 1;
    int chunks = (int)r->index.num_entries;

    uint64_t first_s = in[0].ms_since_epoch / 1000;
    uint64_t from_s = first_s + 500, to_s = first_s + 2500;
    uint64_t want = count_in_range((const uint64_t*)in, N, 5, from_s * 1000, to_s * 1000);

    ticks_iterator_t* it = NULL;
    if (ticks_iterator_create(r, (time_t)from_s, (time_t)to_s, &it) != TICKS_OK) {
        fprintf(stderr, "FAIL: quote iter create\n"); return 1;
    }
    // Wrong typed accessor must be rejected.
    trade_data_t trec;
    if (ticks_iterator_next(it, &trec) != TICKS_ERROR_SCHEMA_MISMATCH) {
        fprintf(stderr, "FAIL: expected SCHEMA_MISMATCH for trade-next on quote file\n"); return 1;
    }
    uint64_t got_n = 0; quote_data_t q; ticks_status_e ns;
    while ((ns = ticks_iterator_next_quote(it, &q)) == TICKS_OK) {
        if (q.ask < q.bid) { fprintf(stderr, "FAIL: quote ask<bid after decode\n"); return 1; }
        got_n++;
    }
    ticks_iterator_destroy(it);
    if (ns != TICKS_EOF || got_n != want) {
        fprintf(stderr, "FAIL: quote iter count %llu != %llu\n",
                (unsigned long long)got_n, (unsigned long long)want); return 1;
    }

    ticks_close(r); free(in); remove(fn);
    printf("PASS quote: %llu recs / %d chunk(s) decoded exactly; CRC+summary ok; "
           "schema guards ok; iterator %llu recs; %.2fx vs naive 40B.\n",
           (unsigned long long)N, chunks, (unsigned long long)got_n, (double)N * 40.0 / (double)disk);
    return 0;
}

int main(void) {
    if (test_trades() != 0) return 1;
    if (test_quotes() != 0) return 1;
    printf("ALL PASS\n");
    return 0;
}
