"""Parquet / Feather / CSV side of the format benchmark.

Reads the identical rows ticksio just wrote to bench/out.csv, then times
write / insert / read and records size for each format, appending rows to
bench/results.csv (which bench.exe created with the ticks rows).

  write  = bulk: serialize the whole table to disk in one call
  insert = streaming: write in row-group batches via the format's incremental
           writer (Parquet/Feather are write-once; this is the closest analog
           to live append and is labelled as such)
  read   = decode the whole file into native columnar memory (numpy arrays).
           ticksio's read iterates rows into C structs; both materialize all the
           data into the language's native in-memory form, which is the fairest
           practical cross-format read. (Materializing Parquet into millions of
           *Python* objects via to_pydict would instead measure Python object
           creation, not decode, so we avoid it.)

Timing here is pyarrow (C++), native like ticksio's C; the harness still
differs, so read these as order-of-magnitude, not photo-finish.
"""
import os, csv, time, io, lzma
import numpy as np
import pyarrow as pa
import pyarrow.csv as pacsv
import pyarrow.parquet as pq
import pyarrow.feather as feather

SRC = "bench/out.csv"
RESULTS = "bench/results.csv"
BATCH = 100_000  # streaming row-group size


def materialize(tbl):
    # decode every column into a native numpy array (no Python-object blowup)
    for i in range(tbl.num_columns):
        tbl.column(i).combine_chunks().to_numpy(zero_copy_only=False)

t = pacsv.read_csv(SRC).cast(
    pa.schema([("timestamp", pa.int64()), ("price", pa.int64()), ("volume", pa.int64())])
)
n = t.num_rows
batches = t.to_batches(max_chunksize=BATCH)

# Native int64 columns, used by the bi5 (Dukascopy) encoder below.
ts_col = t.column("timestamp").combine_chunks().to_numpy(zero_copy_only=False)
px_col = t.column("price").combine_chunks().to_numpy(zero_copy_only=False)
vol_col = t.column("volume").combine_chunks().to_numpy(zero_copy_only=False)


def timed(fn, reps=3):
    best = float("inf")
    for _ in range(reps):
        t0 = time.perf_counter()
        fn()
        best = min(best, time.perf_counter() - t0)
    return best


def bench_parquet(label, **kw):
    path = "bench/p.parquet"

    def write():
        pq.write_table(t, path, **kw)

    def insert():
        w = pq.ParquetWriter(path, t.schema, **kw)
        for b in batches:
            w.write_table(pa.Table.from_batches([b]))
        w.close()

    def read():
        materialize(pq.read_table(path))

    write()
    size = os.path.getsize(path)
    return (label, n, size, timed(write), timed(insert), timed(read))


def bench_feather(label, compression):
    path = "bench/p.feather"

    def write():
        feather.write_feather(t, path, compression=compression)

    def insert():
        import pyarrow.ipc as ipc
        with pa.OSFile(path, "wb") as sink:
            opts = ipc.IpcWriteOptions(
                compression=None if compression == "uncompressed" else compression
            )
            with ipc.new_file(sink, t.schema, options=opts) as w:
                for b in batches:
                    w.write_batch(b)

    def read():
        materialize(feather.read_table(path))

    write()
    size = os.path.getsize(path)
    return (label, n, size, timed(write), timed(insert), timed(read))


def bench_csv(label):
    path = "bench/p.csv"

    def write():
        pacsv.write_csv(t, path)

    def insert():
        with open(path, "wb") as f:
            f.write(b"timestamp,price,volume\n")
            for b in batches:
                buf = io.BytesIO()
                pacsv.write_csv(
                    pa.Table.from_batches([b]),
                    buf,
                    write_options=pacsv.WriteOptions(include_header=False),
                )
                f.write(buf.getvalue())

    def read():
        materialize(pacsv.read_csv(path))

    write()
    size = os.path.getsize(path)
    return (label, n, size, timed(write), timed(insert), timed(read))


def bench_bi5(label, preset=lzma.PRESET_DEFAULT):
    """Dukascopy .bi5 — the raw vendor tick format ticksio ingests from: fixed-
    width big-endian records, LZMA-compressed, with no columnar or delta layer.

    Real bi5 is a 5-field quote record (ms-offset, ask, bid, ask_vol, bid_vol),
    big-endian, stored one LZMA-compressed file per hour. Our synthetic stream
    is trades, so we encode the analogous trade triple with bi5's own strategy:
    time as a uint32 millisecond offset from the first tick (the per-file offset
    is exactly why bi5 gets away with 32-bit time), price and volume as int32,
    packed big-endian one record after another, then LZMA-compressed as a whole.
    Same row-wise fixed-width + LZMA approach as the vendor format, on the
    identical rows the other formats encode — and lossless for this dataset
    (offsets < 2^32, price/volume < 2^31).
    """
    path = "bench/p.bi5"
    base = int(ts_col[0])
    rec_dt = np.dtype([("t", ">u4"), ("p", ">i4"), ("v", ">i4")])

    # The fixed-width record array is bi5's on-disk payload (pre-compression);
    # building it is the analog of the columnar serialization the other formats
    # do inside their writers, so it is rebuilt inside write()/insert() to time.
    def pack(lo, hi):
        rec = np.empty(hi - lo, dtype=rec_dt)
        rec["t"] = (ts_col[lo:hi] - base).astype(">u4")
        rec["p"] = px_col[lo:hi].astype(">i4")
        rec["v"] = vol_col[lo:hi].astype(">i4")
        return rec.tobytes()

    def write():
        with open(path, "wb") as f:
            f.write(lzma.compress(pack(0, n), format=lzma.FORMAT_ALONE, preset=preset))

    def insert():
        # bi5 has no native row-group appender; the closest live-append analog is
        # feeding the LZMA stream batch by batch (labelled as such, like Parquet).
        c = lzma.LZMACompressor(format=lzma.FORMAT_ALONE, preset=preset)
        with open(path, "wb") as f:
            off = 0
            for b in batches:
                m = b.num_rows
                f.write(c.compress(pack(off, off + m)))
                off += m
            f.write(c.flush())

    def read():
        with open(path, "rb") as f:
            raw = lzma.decompress(f.read())
        rec = np.frombuffer(raw, dtype=rec_dt)
        # materialize each column into native int64 (timestamps re-absolutized),
        # mirroring the other formats' decode-into-numpy read.
        rec["t"].astype(np.int64) + base
        rec["p"].astype(np.int64)
        rec["v"].astype(np.int64)

    write()
    size = os.path.getsize(path)
    return (label, n, size, timed(write), timed(insert), timed(read))


def run_one(name, fn):
    # Per-format progress logging (flushed) so a long-running codec — bi5's LZMA
    # in particular — is visibly in-progress rather than looking hung.
    print(f"  [bench_py] {name:<14} ...", end="", flush=True)
    t0 = time.perf_counter()
    res = fn()
    print(f" done {time.perf_counter() - t0:6.1f}s  "
          f"size={res[2]:>12,}  w={res[3]:.3f} i={res[4]:.3f} r={res[5]:.3f}", flush=True)
    return res


print(f"[bench_py] N={n:,} — timing parquet/feather/bi5/csv", flush=True)
rows = [
    run_one("parquet snappy", lambda: bench_parquet("parquet snappy", compression="snappy")),
    run_one("parquet zstd", lambda: bench_parquet("parquet zstd", compression="zstd")),
    run_one("feather zstd", lambda: bench_feather("feather zstd", "zstd")),
    run_one("bi5", lambda: bench_bi5("bi5")),
    run_one("csv", lambda: bench_csv("csv")),
]

with open(RESULTS, "a", newline="") as f:
    wr = csv.writer(f)
    for r in rows:
        wr.writerow([r[0], r[1], r[2], f"{r[3]:.6f}", f"{r[4]:.6f}", f"{r[5]:.6f}"])

for r in rows:
    print(f"{r[0]:<16} size={r[2]:>10,}  write={r[3]:.3f}s insert={r[4]:.3f}s read={r[5]:.3f}s")
print(f"\nappended {len(rows)} rows to {RESULTS}")
