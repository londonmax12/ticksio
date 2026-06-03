"""Parquet / Feather / bi5 / CSV side of the format benchmark.

Reads the identical rows ticksio just wrote to bench/out.csv, then times
write / insert / read and records size for each format, appending rows to
bench/results.csv (which bench.exe created with the ticks rows) in the same
tidy long form: `format,n,metric,variant,value`.

  write       = bulk: serialize the whole table to disk in one call.
  insert      = streaming: write in row-group batches via the format's
                incremental writer. Swept across batch sizes (1k / 10k / 100k)
                because insert throughput is a function of batch granularity,
                not a single point — the curve is the honest answer.
                (Parquet/Feather are write-once; this is the closest analog to
                live append and is labelled as such.)
  read, two modes — the same split the ticks side reports:
    scan        — iterate the file's batches, decode each into native arrays and
                  discard (bounded memory, no retained N-sized output). Compared
                  against ticksio's iterate-into-a-reused-struct scan.
    materialize — decode the whole file into contiguous N-sized native arrays
                  (combine_chunks + to_numpy), retained. Compared head-to-head
                  against ticksio decoding into three N-sized int64 arrays.
    (Materializing Parquet into millions of *Python* objects via to_pydict would
    instead measure Python object creation, not decode, so we avoid it.)

Every metric is best-of-REPS (min wall-clock), matching the C side: cold-run
jitter dominates a single shot; the steady-state number is the honest one.
Timing here is pyarrow (C++), native like ticksio's C; the harness still
differs, so read the throughput as order-of-magnitude, not photo-finish.
"""
import os, csv, time, io, lzma
import numpy as np
import pyarrow as pa
import pyarrow.csv as pacsv
import pyarrow.parquet as pq
import pyarrow.feather as feather
import pyarrow.ipc as ipc

SRC = "bench/out.csv"
RESULTS = "bench/results.csv"
REPS = int(os.environ.get("BENCH_REPS", "3"))
# Insert (streaming) batch sizes — must match INSERT_BATCHES in bench.c.
BATCHES = [1000, 10000, 100000]
SCAN_BATCH = 100_000  # read-side streaming granularity for the "scan" mode


def materialize(tbl):
    # decode every column into one contiguous native int64 array (retained)
    cols = []
    for i in range(tbl.num_columns):
        cols.append(tbl.column(i).combine_chunks().to_numpy(zero_copy_only=False))
    return cols


def consume_batch(batch):
    # decode each column of one batch into a native array and drop it — the
    # bounded-memory "scan" unit, no contiguous N-sized array ever built.
    for i in range(batch.num_columns):
        batch.column(i).to_numpy(zero_copy_only=False)


t = pacsv.read_csv(SRC).cast(
    pa.schema([("timestamp", pa.int64()), ("price", pa.int64()), ("volume", pa.int64())])
)
n = t.num_rows

# Native int64 columns, used by the bi5 (Dukascopy) encoder below.
ts_col = t.column("timestamp").combine_chunks().to_numpy(zero_copy_only=False)
px_col = t.column("price").combine_chunks().to_numpy(zero_copy_only=False)
vol_col = t.column("volume").combine_chunks().to_numpy(zero_copy_only=False)


def timed(fn, reps=REPS):
    best = float("inf")
    for _ in range(reps):
        t0 = time.perf_counter()
        fn()
        best = min(best, time.perf_counter() - t0)
    return best


def parquet_spec(label, **kw):
    path = "bench/p.parquet"

    def write():
        pq.write_table(t, path, **kw)

    def insert(bs):
        def go():
            w = pq.ParquetWriter(path, t.schema, **kw)
            for b in t.to_batches(max_chunksize=bs):
                w.write_table(pa.Table.from_batches([b]))
            w.close()
        return go

    def scan():
        for b in pq.ParquetFile(path).iter_batches(batch_size=SCAN_BATCH):
            consume_batch(b)

    def mat():
        materialize(pq.read_table(path))

    return dict(path=path, write=write, insert=insert, scan=scan, materialize=mat)


def feather_spec(label, compression):
    path = "bench/p.feather"
    opts = ipc.IpcWriteOptions(
        compression=None if compression == "uncompressed" else compression
    )

    def write():
        feather.write_feather(t, path, compression=compression)

    def insert(bs):
        def go():
            with pa.OSFile(path, "wb") as sink:
                with ipc.new_file(sink, t.schema, options=opts) as w:
                    for b in t.to_batches(max_chunksize=bs):
                        w.write_batch(b)
        return go

    def scan():
        # Bulk write() lays the file out as one batch, so re-write it batched for
        # the scan so the iterator genuinely streams (bounded memory).
        if not getattr(scan, "_prepped", False):
            insert(SCAN_BATCH)()
            scan._prepped = True
        r = ipc.open_file(path)
        for i in range(r.num_record_batches):
            consume_batch(r.get_batch(i))

    def mat():
        materialize(feather.read_table(path))

    return dict(path=path, write=write, insert=insert, scan=scan, materialize=mat)


def csv_spec(label):
    path = "bench/p.csv"

    def write():
        pacsv.write_csv(t, path)

    def insert(bs):
        def go():
            with open(path, "wb") as f:
                f.write(b"timestamp,price,volume\n")
                for b in t.to_batches(max_chunksize=bs):
                    buf = io.BytesIO()
                    pacsv.write_csv(
                        pa.Table.from_batches([b]), buf,
                        write_options=pacsv.WriteOptions(include_header=False),
                    )
                    f.write(buf.getvalue())
        return go

    def scan():
        r = pacsv.open_csv(path)
        for b in r:
            consume_batch(b)

    def mat():
        materialize(pacsv.read_csv(path))

    return dict(path=path, write=write, insert=insert, scan=scan, materialize=mat)


def bi5_spec(label, preset=lzma.PRESET_DEFAULT):
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

    bi5 is a single LZMA blob with no internal batch structure, so it cannot
    stream a read: both read modes pay the whole-file decompress, and scan and
    materialize end up close. That is a true property of the format, reported
    rather than hidden.
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

    def insert(bs):
        # bi5 has no native row-group appender; the closest live-append analog is
        # feeding the LZMA stream batch by batch (labelled as such, like Parquet).
        def go():
            c = lzma.LZMACompressor(format=lzma.FORMAT_ALONE, preset=preset)
            with open(path, "wb") as f:
                for off in range(0, n, bs):
                    f.write(c.compress(pack(off, min(off + bs, n))))
                f.write(c.flush())
        return go

    def _decompress():
        with open(path, "rb") as f:
            return np.frombuffer(lzma.decompress(f.read()), dtype=rec_dt)

    def scan():
        rec = _decompress()
        # iterate the single decompressed blob in slices, decode + discard
        for off in range(0, len(rec), SCAN_BATCH):
            s = rec[off:off + SCAN_BATCH]
            s["t"].astype(np.int64) + base
            s["p"].astype(np.int64)
            s["v"].astype(np.int64)

    def mat():
        rec = _decompress()
        rec["t"].astype(np.int64) + base  # timestamps re-absolutized
        rec["p"].astype(np.int64)
        rec["v"].astype(np.int64)

    return dict(path=path, write=write, insert=insert, scan=scan, materialize=mat)


def run(label, spec):
    """Time every metric for one format and return long-form rows."""
    print(f"  [bench_py] {label:<14} ...", end="", flush=True)
    t0 = time.perf_counter()

    spec["write"]()                       # canonical bulk file for size + reads
    size = os.path.getsize(spec["path"])
    rows = [(label, n, "size", "", str(size))]
    rows.append((label, n, "write", "", f"{timed(spec['write']):.6f}"))
    for bs in BATCHES:
        rows.append((label, n, "insert", str(bs), f"{timed(spec['insert'](bs)):.6f}"))
    spec["write"]()                       # restore bulk layout before the reads
    rows.append((label, n, "read", "scan", f"{timed(spec['scan']):.6f}"))
    rows.append((label, n, "read", "materialize", f"{timed(spec['materialize']):.6f}"))

    print(f" done {time.perf_counter() - t0:6.1f}s  size={size:>12,}", flush=True)
    return rows


print(f"[bench_py] N={n:,}  REPS={REPS} — timing parquet/feather/bi5/csv", flush=True)
specs = [
    ("parquet snappy", parquet_spec("parquet snappy", compression="snappy")),
    ("parquet zstd",   parquet_spec("parquet zstd", compression="zstd")),
    ("feather zstd",   feather_spec("feather zstd", "zstd")),
    ("bi5",            bi5_spec("bi5")),
    ("csv",            csv_spec("csv")),
]

all_rows = []
for label, spec in specs:
    all_rows.extend(run(label, spec))

with open(RESULTS, "a", newline="") as f:
    csv.writer(f).writerows(all_rows)

print(f"\nappended {len(all_rows)} rows to {RESULTS}")
