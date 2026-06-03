# ticksio

A C library and on-disk file format (`.ticks`) for storing tick-level financial
data - trades and quotes — optimized for compact storage and fast sequential
range scans.

A `.ticks` file is **columnar** and **delta-encoded**: within each chunk every
column is stored contiguously and as the signed change from the previous record,
so each value's on-disk width is governed by how much consecutive ticks *move*,
not by their absolute magnitude. An optional per-chunk **ZSTD** layer sits below
that. A denormalized header summary plus a per-chunk index make range queries
prune to just the chunks that overlap the requested time window — without
decoding (or even decompressing) anything else.

The full byte-level layout is specified in [docs/ticks-format.md](docs/ticks-format.md).

---

## Features

- **Columnar + delta encoding** - struct-of-arrays chunk layout; timestamps use
  unsigned deltas, prices/sizes use zig-zag signed deltas.
- **Per-chunk ZSTD compression** (optional) - layered below the columnar
  encoding; `COMPRESSION_NONE` files are byte-compatible with the uncompressed
  format.
- **Range pruning without decoding** - each index entry bounds its chunk's time
  span, so out-of-range chunks (including the last one) are skipped using the
  index alone. CRC checks and pruning never need to decompress.
- **Integrity checking** - per-chunk CRC32 plus a file-level summary
  cross-check, validated by `ticks_verify`.
- **Schema-aware** - files declare a `schema_id` (`trade` or `quote`); the chunk
  encoding is generic over the column set, so adding a schema is a registry
  change, not a format change.
- **Portable on-disk layout** - fixed little-endian byte offsets, serialized
  field-by-field (no compiler padding leaks into the file).

## Schemas

A file stores a single record schema. Column 0 is always the timestamp
(epoch milliseconds).

| `schema_id` | Name    | Columns                                      |
|-------------|---------|----------------------------------------------|
| `0`         | `trade` | timestamp, price, volume                     |
| `1`         | `quote` | timestamp, bid, ask, bid_size, ask_size      |

Integer columns store no decimal point of their own — the header's `price_scale`
and `volume_scale` (base-10 exponents) convert stored integers back to
real-world values (e.g. `price_scale = -2` ⇒ prices are in cents).

---

## Building

ticksio builds with CMake (≥ 3.25) and a C99 compiler. ZSTD is fetched and built
automatically at configure time via CMake `FetchContent`, so a network
connection is required for the first configure.

```sh
cmake -S src/c -B build/c
cmake --build build/c
```

This produces the static library `ticksio` plus two test executables,
`roundtrip` and `sandbox`.

### Running the tests

The round-trip test writes synthetic trade and quote streams, reopens them,
verifies CRC + the header summary, independently decodes the raw chunk bytes,
range-iterates, and exercises the ZSTD path:

```sh
./build/c/roundtrip
```

---

## Usage

### Writing a trade file

```c
#include "ticksio/ticksio.h"
#include <string.h>

ticks_header_t hdr;
memset(&hdr, 0, sizeof(hdr));
strcpy(hdr.ticker, "AAPL");
strcpy(hdr.currency, "USD");
strcpy(hdr.country, "US");
hdr.asset_class      = ASSET_CLASS_STOCK;
hdr.schema_id        = SCHEMA_TRADE;
hdr.price_scale      = -2;              // prices stored in cents
hdr.compression_type = COMPRESSION_ZSTD; // or COMPRESSION_NONE

ticks_file_t* w = NULL;
ticks_new_file("aapl.ticks", &hdr, &w);

trade_data_t ticks[] = {
    { .ms_since_epoch = 1700000000000ULL, .price = 18950, .volume = 100 },
    { .ms_since_epoch = 1700000000004ULL, .price = 18951, .volume =  50 },
    /* ... timestamps must be non-decreasing ... */
};
ticks_add_data(w, ticks, sizeof(ticks) / sizeof(ticks[0]));

ticks_close(w);
```

`ticks_add_quotes` is the equivalent for `SCHEMA_QUOTE` files (`quote_data_t`).

### Reading a time range

```c
ticks_file_t* r = NULL;
ticks_open_read("aapl.ticks", &r);

ticks_verify(r); // optional: recompute CRCs + cross-check the summary

ticks_iterator_t* it = NULL;
// [from, to) in epoch milliseconds (same unit as stored ticks, so sub-second
// windows work); records are produced in ascending time order
ticks_iterator_create(r, from_ms, to_ms, &it);

trade_data_t rec;
ticks_status_e s;
while ((s = ticks_iterator_next(it, &rec)) == TICKS_OK) {
    // rec.ms_since_epoch, rec.price, rec.volume
}
// s == TICKS_EOF when the range is exhausted

ticks_iterator_destroy(it);
ticks_close(r);
```

Use `ticks_iterator_next_quote` to read `quote_data_t` records from a quote file.

### Status codes

Every API call returns a `ticks_status_e` (`TICKS_OK == 0`). Convert any code to
a human-readable string with `ticks_status_to_string`. Notable codes:
`TICKS_EOF`, `TICKS_ERROR_SCHEMA_MISMATCH` (wrong typed accessor for the file's
schema), `TICKS_ERROR_UNSORTED_DATA` (timestamps decreased), and
`TICKS_ERROR_CORRUPT_DATA` (CRC or summary mismatch).

---

## Project layout

```
docs/ticks-format.md        # authoritative byte-level format specification
src/c/
  CMakeLists.txt            # build (fetches + builds ZSTD)
  include/ticksio/          # public + internal headers
    ticksio.h               #   public API
    ticksio_types.h         #   records, header, status codes, enums
    ticksio_constants.h     #   on-disk offsets, sizes, version
  src/                      # implementation (chunks, compression, index, schema, csv)
  tests/                    # roundtrip + sandbox
```

## Format versioning

The current on-disk format is **version 5**. The `version` field in the header
is checked on open; readers reject files (and `ticks_new_file` rejects headers)
whose version or `compression_type` they cannot handle. See the version history
in [docs/ticks-format.md](docs/ticks-format.md) for what each version changed.

## License

Released under the [MIT License](LICENSE).
