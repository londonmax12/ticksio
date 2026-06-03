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
    // The iterator range is in epoch milliseconds (same unit as the stored ticks).
    if (ticks_iterator_create(r, (int64_t)(from_s * 1000), (int64_t)(to_s * 1000), &it) != TICKS_OK) {
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

    // ticks_read_columns (columnar materialize) must reproduce exactly what the
    // row decode does — full file and a sub-range — and reject misuse.
    {
        int64_t* mts = malloc(N * sizeof(int64_t));
        int64_t* mpx = malloc(N * sizeof(int64_t));
        int64_t* mvol = malloc(N * sizeof(int64_t));
        if (!mts || !mpx || !mvol) { fprintf(stderr, "alloc mat\n"); return 1; }
        int64_t* cols[3] = { mts, mpx, mvol };

        uint64_t mc = 0;
        ticks_status_e ms = ticks_read_columns(r, (int64_t)got.min_timestamp,
                                               (int64_t)got.max_timestamp + 1, cols, 3, N, &mc);
        if (ms != TICKS_OK || mc != N) {
            fprintf(stderr, "FAIL: read_columns full count %llu != %llu (status %d)\n",
                    (unsigned long long)mc, (unsigned long long)N, ms); return 1;
        }
        for (uint64_t i = 0; i < N; i++) {
            if ((uint64_t)mts[i] != in[i].ms_since_epoch || (uint64_t)mpx[i] != in[i].price ||
                (uint64_t)mvol[i] != in[i].volume) {
                fprintf(stderr, "FAIL: read_columns rec %llu mismatch\n", (unsigned long long)i); return 1;
            }
        }

        // Sub-range: same window the iterator checked above; cross-check values
        // against a reference scan over `in` (exercises the boundary-chunk path).
        uint64_t mc2 = 0;
        ms = ticks_read_columns(r, (int64_t)(from_s * 1000), (int64_t)(to_s * 1000), cols, 3, N, &mc2);
        if (ms != TICKS_OK || mc2 != want) {
            fprintf(stderr, "FAIL: read_columns range count %llu != %llu (status %d)\n",
                    (unsigned long long)mc2, (unsigned long long)want, ms); return 1;
        }
        uint64_t ri = 0;
        for (uint64_t i = 0; i < N; i++) {
            uint64_t t = in[i].ms_since_epoch;
            if (t >= from_s * 1000 && t < to_s * 1000) {
                if ((uint64_t)mts[ri] != in[i].ms_since_epoch || (uint64_t)mpx[ri] != in[i].price ||
                    (uint64_t)mvol[ri] != in[i].volume) {
                    fprintf(stderr, "FAIL: read_columns range rec %llu mismatch\n", (unsigned long long)ri); return 1;
                }
                ri++;
            }
        }

        // Wrong column count and under-capacity must be rejected, not misread.
        if (ticks_read_columns(r, (int64_t)got.min_timestamp, (int64_t)got.max_timestamp + 1,
                               cols, 5, N, &mc) != TICKS_ERROR_SCHEMA_MISMATCH) {
            fprintf(stderr, "FAIL: expected SCHEMA_MISMATCH for ncols=5 on trade file\n"); return 1;
        }
        if (ticks_read_columns(r, (int64_t)got.min_timestamp, (int64_t)got.max_timestamp + 1,
                               cols, 3, N - 1, &mc) != TICKS_ERROR_INVALID_ARGUMENTS) {
            fprintf(stderr, "FAIL: expected INVALID_ARGUMENTS for under-capacity\n"); return 1;
        }

        free(mts); free(mpx); free(mvol);
    }

    ticks_close(r); free(in); remove(fn);
    printf("PASS trade: %llu recs / %d chunk(s) decoded exactly; CRC+summary ok; "
           "iterator + columnar materialize %llu recs; %.2fx vs naive 24B.\n",
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
    if (ticks_iterator_create(r, (int64_t)(from_s * 1000), (int64_t)(to_s * 1000), &it) != TICKS_OK) {
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

// Sum the on-disk byte size of every chunk (from the index).
static uint64_t total_chunk_bytes(ticks_file_t* r) {
    uint64_t disk = 0;
    for (uint32_t c = 0; c < r->index.num_entries; c++)
        disk += r->index.entries[c].chunk_size;
    return disk;
}

// --- Compression round-trip ----------------------------------------------
// Writes the same trade stream twice — once uncompressed, once with ZSTD — then
// reopens the compressed file, verifies its CRC/summary, decodes every record
// through the public iterator (exercising the decompress path) and checks it
// matches the input exactly, and confirms the codec actually shrank the data.
static int test_compression(void) {
    const uint64_t N = 2000000ULL;

    trade_data_t* in = malloc(N * sizeof(trade_data_t));
    if (!in) { fprintf(stderr, "alloc\n"); return 1; }

    uint64_t ts = 1700000000000ULL;
    int64_t price = 4000000, volume = 1000000;
    uint32_t rng = 777u;
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

    // Reference: same data, no compression.
    const char* fn_none = "roundtrip_none.ticks";
    hdr.compression_type = COMPRESSION_NONE;
    ticks_file_t* w = NULL;
    if (ticks_new_file(fn_none, &hdr, &w) != TICKS_OK) { fprintf(stderr, "new_file none\n"); return 1; }
    if (ticks_add_data(w, in, N) != TICKS_OK) { fprintf(stderr, "add none\n"); return 1; }
    ticks_close(w);

    ticks_file_t* rn = NULL;
    if (ticks_open_read(fn_none, &rn) != TICKS_OK) { fprintf(stderr, "open none\n"); return 1; }
    uint64_t disk_none = total_chunk_bytes(rn);
    ticks_close(rn);

    // ZSTD-compressed copy.
    const char* fn_z = "roundtrip_zstd.ticks";
    hdr.compression_type = COMPRESSION_ZSTD;
    if (ticks_new_file(fn_z, &hdr, &w) != TICKS_OK) { fprintf(stderr, "new_file zstd\n"); return 1; }
    ticks_status_e s = ticks_add_data(w, in, N);
    if (s != TICKS_OK) { fprintf(stderr, "add zstd: %s\n", ticks_status_to_string(s)); return 1; }
    ticks_close(w);

    ticks_file_t* r = NULL;
    if (ticks_open_read(fn_z, &r) != TICKS_OK) { fprintf(stderr, "open zstd\n"); return 1; }
    ticks_header_t got; ticks_get_header(r, &got);
    if (got.compression_type != COMPRESSION_ZSTD || got.record_count != N) {
        fprintf(stderr, "FAIL: zstd header (ctype=%u rc=%llu)\n",
                got.compression_type, (unsigned long long)got.record_count); return 1;
    }
    // CRC is taken over the compressed on-disk bytes, so verify must pass without
    // decompressing.
    if (ticks_verify(r) != TICKS_OK) { fprintf(stderr, "FAIL: zstd CRC/summary\n"); return 1; }

    uint64_t disk_zstd = total_chunk_bytes(r);

    // Decode every record through the public iterator and compare to the input.
    uint64_t first_s = in[0].ms_since_epoch / 1000;
    uint64_t last_s = in[N - 1].ms_since_epoch / 1000;
    ticks_iterator_t* it = NULL;
    if (ticks_iterator_create(r, (int64_t)(first_s * 1000), (int64_t)((last_s + 1) * 1000), &it) != TICKS_OK) {
        fprintf(stderr, "FAIL: zstd iter create\n"); return 1;
    }
    uint64_t got_n = 0; trade_data_t rec; ticks_status_e ns;
    while ((ns = ticks_iterator_next(it, &rec)) == TICKS_OK) {
        const trade_data_t* want = &in[got_n];
        if (rec.ms_since_epoch != want->ms_since_epoch || rec.price != want->price ||
            rec.volume != want->volume) {
            fprintf(stderr, "FAIL: zstd record %llu mismatch after decompress\n",
                    (unsigned long long)got_n); return 1;
        }
        got_n++;
    }
    ticks_iterator_destroy(it);
    if (ns != TICKS_EOF || got_n != N) {
        fprintf(stderr, "FAIL: zstd iter count %llu != %llu (status %d)\n",
                (unsigned long long)got_n, (unsigned long long)N, ns); return 1;
    }

    if (disk_zstd >= disk_none) {
        fprintf(stderr, "FAIL: zstd %llu not smaller than uncompressed %llu\n",
                (unsigned long long)disk_zstd, (unsigned long long)disk_none); return 1;
    }

    ticks_close(r); free(in);
    remove(fn_none); remove(fn_z);
    printf("PASS compress: %llu recs decoded exactly through ZSTD; CRC+summary ok; "
           "%llu -> %llu bytes (%.2fx smaller than uncompressed columnar).\n",
           (unsigned long long)N, (unsigned long long)disk_none,
           (unsigned long long)disk_zstd, (double)disk_none / (double)disk_zstd);
    return 0;
}

int main(void) {
    if (test_trades() != 0) return 1;
    if (test_quotes() != 0) return 1;
    if (test_compression() != 0) return 1;
    printf("ALL PASS\n");
    return 0;
}
