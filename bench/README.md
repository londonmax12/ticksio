# Benchmarks

A repeatable comparison of `.ticks` against Parquet, Feather (Arrow IPC), and
CSV on the same synthetic trade stream. Measures **size**, **write**,
**insert**, and **read**, records the raw numbers to [results.csv](results.csv),
and renders [comparison.png](comparison.png).

![format comparison](comparison.png)

See [results.md](results.md) for the table form.

## What is measured

The dataset is one deterministic synthetic liquid-stock time-and-sales stream
(monotonic millisecond timestamps with small gaps, a cent-level price random
walk, round-lot volumes). Every format encodes the **identical rows** as three
`int64` columns — `timestamp, price, volume` — so the comparison is
representation-for-representation (ticksio stores price as a scaled integer; the
Parquet/Feather columns match it exactly). The ticks files are verified to
round-trip all N records losslessly before timing.

| metric | meaning |
|--------|---------|
| **size** | on-disk bytes (reported as bytes/tick) |
| **write** | bulk: encode + write the whole dataset to disk in one call |
| **insert** | streaming: write in batches via the format's incremental writer (Parquet/Feather are write-once — this is the closest live-append analog and is labelled as such) |
| **read** | decode the whole file into the language's native in-memory form (ticksio iterates rows into C structs; the columnar formats decode into numpy arrays) |

## Caveats — read these before quoting the numbers

- **One synthetic dataset.** The shape is realistic but favourable to delta
  encoding. Jumpier data (wide-swing crypto, illiquid names) narrows the size
  gap. Re-run with your own data to get numbers that matter to you.
- **Different harnesses.** ticksio is timed in C; Parquet/Feather/CSV in pyarrow
  (C++ under Python). Both are native code, but the harnesses differ — treat the
  throughput bars as order-of-magnitude, not a photo finish.
- **Not apples-to-apples on features.** `.ticks` is a fixed-schema, purpose-built
  tick format. Parquet/Feather are general-purpose columnar formats carrying
  overhead for things ticksio doesn't do (arbitrary/nested schemas, per-column
  predicate pushdown, broad ecosystem). "Smaller for tick data" ≠ "better
  format."

## Running it

Requires the built library (`build/c/libticksio.a` + the fetched
`libzstd.a`), a C compiler, and Python with `pyarrow`, `numpy`, `matplotlib`.

```sh
# 1. build the ticks side (generates data, times ticks, writes results.csv + out.csv)
gcc -O2 -Isrc/c/include bench/bench.c build/c/libticksio.a \
    build/c/_deps/zstd-build/lib/libzstd.a -o bench/bench.exe
./bench/bench.exe 5000000          # row count (default 5,000,000)

# 2. time Parquet / Feather / CSV on the identical rows (appends to results.csv)
python bench/bench_py.py

# 3. render the chart + markdown table
python bench/chart.py
```

`bench.exe` must run before `bench_py.py` (it writes both the shared `out.csv`
and the header row of `results.csv`; the Python side appends to it).
