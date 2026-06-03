# Benchmarks

A repeatable comparison of `.ticks` against Parquet, Feather (Arrow IPC),
Dukascopy **bi5** (the raw vendor tick format ticksio ingests from), and CSV on
the same synthetic trade stream. Measures **size**, **write**, **insert**, and
**read**, records the raw numbers to [results.csv](results.csv), and renders
[comparison.png](comparison.png).

![format comparison](comparison.png)

See [results.md](results.md) for the table form.

## What is measured

The dataset is one deterministic synthetic liquid-stock time-and-sales stream
(monotonic millisecond timestamps with small gaps, a cent-level price random
walk, round-lot volumes). Every format encodes the **identical rows** as three
`int64` columns — `timestamp, price, volume` — so the comparison is
representation-for-representation (ticksio stores price as a scaled integer; the
Parquet/Feather columns match it exactly). The ticks files are verified to
round-trip all N records losslessly before timing. The one exception is bi5,
which by design uses 32-bit fields and a per-file time offset (lossless on this
dataset); see the caveat below.

Every timed metric is reported **best-of-N** (minimum wall-clock over `REPS`
runs, default 3) on **both** sides — the C ticks side and the pyarrow side use
the same rule. A single cold run is dominated by cache/OS jitter; the
steady-state minimum is the honest number. zstd level is **3** on every zstd
path (ticksio's writer, Parquet, Feather), so the size comparison isn't skewed
by mismatched compression effort.

| metric | meaning |
|--------|---------|
| **size** | on-disk bytes (reported as bytes/tick) |
| **write** | bulk: encode + write the whole dataset to disk in one call |
| **insert** | streaming: write in batches via the format's incremental writer, **swept across batch sizes (1k / 10k / 100k)** — insert throughput is a function of batch granularity, so the curve is the answer, not a single point. (Parquet/Feather are write-once; this is the closest live-append analog and is labelled as such.) |
| **read — scan** | iterate the file's batches, decode each into native arrays and **discard** (bounded memory, no retained N-sized output). ticksio iterates rows into one reused C struct; the columnar formats iterate batches and drop them. |
| **read — materialize** | decode the **whole** file into contiguous N-sized native arrays and keep them. ticksio fills three N-sized `int64` arrays; the columnar formats `combine_chunks` + `to_numpy` per column. This is the head-to-head "load it all into memory" read. |

The single-N run records the full insert curve and both read modes. The scaling
sweep collapses each to one representative point per N (insert at the 100k batch,
read in materialize mode) to keep the multi-N chart legible.

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
- **bi5 is modelled, not byte-exact.** Real Dukascopy `.bi5` is a 5-field
  big-endian quote record (ms-offset, ask, bid, ask/bid volume), LZMA-compressed
  one file per hour. This bench is a *trade* stream, so the bi5 point applies
  that same strategy — fixed-width big-endian records + whole-file LZMA, with a
  32-bit per-file time offset — to the identical trade rows the other formats
  encode (lossless here). It captures bi5's row-wise + LZMA cost on this data,
  not the exact vendor bytes. LZMA's slow write/insert is a real property of the
  format. (Uses Python's stdlib `lzma`, so it adds no extra dependency.)

## Running it

Requires the built library (`build/c/libticksio.a` + the fetched
`libzstd.a`), a C compiler, and Python with `pyarrow`, `numpy`, `matplotlib`.

```sh
# 1. build the ticks side (generates data, times ticks, writes results.csv + out.csv)
gcc -O2 -Isrc/c/include bench/bench.c build/c/libticksio.a \
    build/c/_deps/zstd-build/lib/libzstd.a -o bench/bench.exe
./bench/bench.exe 5000000          # row count (default 5,000,000); optional 2nd arg = REPS (default 3)

# 2. time Parquet / Feather / CSV on the identical rows (appends to results.csv)
python bench/bench_py.py           # honours BENCH_REPS env var (default 3) to match the C side

# 3. render the chart + markdown table
python bench/chart.py
```

`bench.exe` must run before `bench_py.py` (it writes both the shared `out.csv`
and the header row of `results.csv`; the Python side appends to it).

## Scaling sweep

The single-N run above is a snapshot. To see how each format behaves as the
stream grows — does bytes/tick stay flat, does throughput fall off at large N —
run the sweep, which repeats the whole comparison across a range of row counts
and renders [scaling.png](scaling.png): a 2x2 of size + write/insert/read, each a
multi-line chart (one line per format) against N on a log axis.

```sh
python bench/scaling.py              # full geometric sweep (100k .. 10M), then render
python bench/scaling.py 1e5 1e6 1e7  # custom row counts
python bench/scaling.py plot         # re-render scaling.png from existing scaling.csv
```

The sweep shells out to `bench.exe` once per N (build it first, step 1 above) and
writes the tidy per-N numbers to [scaling.csv](scaling.csv). The `plot` mode
re-renders from that CSV without re-timing.
