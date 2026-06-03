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
import os, csv, time, io
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


rows = [
    bench_parquet("parquet snappy", compression="snappy"),
    bench_parquet("parquet zstd", compression="zstd"),
    bench_feather("feather zstd", "zstd"),
    bench_csv("csv"),
]

with open(RESULTS, "a", newline="") as f:
    wr = csv.writer(f)
    for r in rows:
        wr.writerow([r[0], r[1], r[2], f"{r[3]:.6f}", f"{r[4]:.6f}", f"{r[5]:.6f}"])

for r in rows:
    print(f"{r[0]:<16} size={r[2]:>10,}  write={r[3]:.3f}s insert={r[4]:.3f}s read={r[5]:.3f}s")
print(f"\nappended {len(rows)} rows to {RESULTS}")
