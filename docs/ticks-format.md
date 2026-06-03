# File Type Specification — `.ticks`
- **Version:** `5`
- **Author:** London Ball (@londonmax12 on Github)
- **Last Updated:** 2026-06-02

---

## 1. Overview
The `.ticks` file format provides an efficient storage solution for tick-level
financial data, optimized for both compression and sequential access.

A file stores a single **record schema**, identified by `schema_id` in the
header. Each schema fixes the number and meaning of a record's columns; column 0
is always the timestamp (epoch milliseconds). Two schemas are defined today:

| `schema_id` | Name | Columns |
|-------------|------|---------|
| 0 | `trade` | timestamp, price, volume |
| 1 | `quote` | timestamp, bid, ask, bid_size, ask_size |

The on-disk chunk encoding is generic over the column set, so adding a schema is
a registry change rather than a new format. See §2.4.

All multi-byte integers are stored **little-endian**, at **fixed byte offsets**.
Records are serialized field-by-field — C structs are never written to disk
directly — so the layout is independent of compiler padding and host
architecture. The `endianness` byte documents this and is always `1` (little).

---

## 2. File Structure

A file is laid out as: a fixed 72-byte header region, followed by the chunk
data, followed by the index. The header stores the absolute offset and byte
size of the index so a reader can locate it without scanning.

### 2.1 Header region (72 bytes)
| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0  | `magic_number` | char[4] | `"TICK"` (`0x54 0x49 0x43 0x4B`) |
| 4  | `version` | uint16 | Format version (currently `5`) |
| 6  | `endianness` | uint8 | On-disk byte order: 0 = undefined, 1 = little, 2 = big (always written as `1`) |
| 7  | `ticker` | char[8] | Instrument code (e.g., `GBPJPY` or `AAPL`) |
| 15 | `currency` | char[3] | ISO currency code (e.g., `USD`) |
| 18 | `country` | char[2] | ISO country code (e.g., `AU`) |
| 20 | `asset_class` | uint16 | Enum for asset class |
| 22 | `compression_type` | uint16 | Enum for compression algorithm |
| 24 | `price_scale` | int8 | Base-10 exponent for price-like columns (price, bid, ask): real = stored × 10^`price_scale` (e.g. `-2` ⇒ cents) |
| 25 | `volume_scale` | int8 | Base-10 exponent for size-like columns (volume, bid_size, ask_size) |
| 26 | `record_count` | uint64 | Total number of ticks across all chunks (`0` if the file is empty) |
| 34 | `min_timestamp` | uint64 | Epoch ms of the first tick in the file (`0` if empty) |
| 42 | `max_timestamp` | uint64 | Epoch ms of the last tick in the file (`0` if empty) |
| 50 | `schema_id` | uint16 | Record schema for the file (`0` = trade, `1` = quote); see §1 and §2.4 |
| 52 | `reserved` | byte[4] | Reserved for forward-compatible fields; written as zero |
| 56 | `index_offset` | uint64 | Byte offset to the index section |
| 64 | `index_size` | uint64 | Byte size of the index section |

Fixed-width text fields (`ticker`, `currency`, `country`) are NUL-padded if the
value is shorter than the field. The integer columns store no decimal point of
their own — `price_scale` / `volume_scale` are the single source of truth for
converting the stored integers back to real-world values.

`record_count`, `min_timestamp`, and `max_timestamp` are a **denormalized
file-level summary**: a catalog can learn how many ticks a file holds and the
time window they span by reading only this 72-byte header, without seeking to
`index_offset` and scanning the index. `min_timestamp` / `max_timestamp` equal
the first index entry's `chunk_time_base` and the last entry's
`chunk_last_timestamp` respectively, and `record_count` equals the sum of every
chunk's `num_records`; they are maintained by the writer and re-validated on
verify. For an **empty file** (no chunks) all three are `0`, which is the
sentinel distinguishing "empty" from "starts at epoch 0".

---

### 2.2 Chunks (up to 16 MB per chunk)
Chunk data begins at offset 72 (immediately after the header region). Each chunk
is **self-describing** (it starts with its own header) and **columnar**: rather
than interleaving fields row-by-row, it stores all of column 0's values, then
all of column 1's, and so on. Within a chunk the columns are **delta-encoded**
against the previous record, so each column's width is governed by how much
consecutive ticks *move*, not by their absolute magnitude.

The chunk header is **generic over the column set**: it records how many columns
the chunk has and, per column, the absolute first value, the delta width, and
the delta encoding. A decoder therefore needs neither the index nor the schema
registry — the chunk bytes fully describe themselves.

When the file's `compression_type` names a codec, the entire self-describing
chunk (header + columns) described in this section is the **payload** that gets
compressed and wrapped in a per-chunk frame on disk; see §4. With
`compression_type = 0` (none) the chunk is written exactly as described here.

#### 2.2.1 Chunk header (variable length)
A fixed 5-byte prefix, then one 10-byte descriptor per column
(`header_size = 5 + 10 × num_columns`):

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | `num_records` | uint32 | Number of records (ticks) in the chunk |
| 4 | `num_columns` | uint8 | Number of columns (matches the file's schema) |

then, for each column `j` (starting at offset 5, stride 10):

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| +0 | `base` | uint64 | Absolute value of the column's first record |
| +8 | `width` | uint8 | Width of each delta in this column: 1, 2, 4, or 8 bytes |
| +9 | `enc` | uint8 | Delta encoding: `0` = unsigned, `1` = zig-zag signed |

Column 0 is always the timestamp (`enc = 0`, unsigned); all other columns use
zig-zag signed deltas (`enc = 1`).

#### 2.2.2 Columns
Immediately after the chunk header come `num_columns` contiguous column regions,
in column order. Column `j` holds `num_records − 1` deltas of `width[j]` bytes
each (the first record's value is the descriptor's `base`):

- **Unsigned columns** (`enc = 0`, the timestamp): each delta is the unsigned
  gap from the previous record. Timestamps are non-decreasing, so never negative.
- **Zig-zag columns** (`enc = 1`, every other column): each delta is the signed
  change from the previous record, **zig-zag-encoded** to an unsigned integer
  (`0 → 0, −1 → 1, 1 → 2, −2 → 3, …`).

To reconstruct record `i` (`i ≥ 1`) for column `j`:
`value[i] = value[i−1] + delta[i−1]` (un-zig-zag first if `enc = 1`);
`value[0]` is the descriptor's `base`.

A single-record chunk (`num_records == 1`) carries no deltas and is just the
header.

---

### 2.3 Index (32 bytes per entry)
The index is a packed array of fixed-size entries, one per chunk, located at
`index_offset` and spanning `index_size` bytes
(`index_size = entry_count × 32`).

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0  | `chunk_time_base` | uint64 | Epoch timestamp (ms) of the first tick in the chunk |
| 8  | `chunk_last_timestamp` | uint64 | Epoch timestamp (ms) of the last tick in the chunk |
| 16 | `chunk_offset` | uint64 | File offset where the chunk starts |
| 24 | `chunk_size` | uint32 | Byte size of the chunk **as stored on disk** — the uncompressed header + columns when `compression_type = 0`, or the compression frame (§4) when a codec is set |
| 28 | `chunk_crc32` | uint32 | CRC32 (IEEE 802.3) of the chunk's on-disk bytes (the compression frame, when compressed) |

The index is a pure accelerator: per-column widths are **not** duplicated here
(they live in the self-describing chunk header, which is authoritative), so an
index entry is fixed-size regardless of the schema's column count. Because each
chunk is self-describing, the index can be rebuilt by scanning chunk headers.

`chunk_time_base` and `chunk_last_timestamp` together bound each chunk's time
span. Because chunks are stored in non-decreasing time order, a range query can
prune any chunk whose `[chunk_time_base, chunk_last_timestamp]` falls outside
the requested window using the index alone — no chunk needs to be read or
decoded. Storing the last timestamp explicitly (rather than inferring a chunk's
end from the *next* chunk's `chunk_time_base`) is what lets the **final** chunk
be pruned too, since it has no successor.

---

### 2.4 Schemas
A file's `schema_id` (header offset 50) selects a fixed column layout. Column 0
is always the timestamp (epoch ms, unsigned-delta); the remaining columns are
signed values (zig-zag delta).

| `schema_id` | Name | Columns (in order) |
|-------------|------|--------------------|
| 0 | `trade` | timestamp, price, volume |
| 1 | `quote` | timestamp, bid, ask, bid_size, ask_size |

The chunk encoding (§2.2) is generic over the column count and per-column
encoding, so a new schema is added by registering a new id and its column list —
the on-disk chunk format does not change. A reader that does not recognize a
file's `schema_id` can still decode the raw columns (the chunk headers are
self-describing) but cannot attach column meaning. Writers and the typed readers
reject operations whose schema does not match the file's (`SCHEMA_MISMATCH`).

---

## 3. Integrity
Each index entry stores a `chunk_crc32` computed over the chunk's on-disk bytes
(its self-describing header plus all of its columns). Readers can recompute and
compare it to detect corruption (`ticks_verify`). The same pass also recomputes
the file-level summary — `record_count` from the summed chunk `num_records`, and
`min_timestamp` / `max_timestamp` from the index extremes — and fails with a
corruption error if it disagrees with the values stored in the header.

### 3.1 Writer crash-consistency
The writer is **not crash-atomic**. As records are added, the writer appends
chunk bytes only; the **index, the header's `index_offset` / `index_size`
pointers, and the file-level summary are all written once, in a single pass, at
`ticks_close`** (they accumulate in memory during the session, so repeated
appends stay pure sequential writes — they don't seek back to the header or
rewrite the index each time). A process or power failure partway through can
therefore leave the file in any of several intermediate states: chunk bytes with
no index on disk at all, and a header whose `index_offset`/summary still hold
their initial (empty-file) values rather than describing the appended chunks.
The format is **recoverable** rather than self-healing: because every
chunk is self-describing (§2.2) and chunks are laid out contiguously from offset
72 in non-decreasing time order, a repair tool can rebuild the index and the
file-level summary by scanning chunk headers from the start of the chunk region
up to `index_offset`. No such repair routine ships today — a torn write must be
recovered out-of-band (e.g. re-running the writer) — but the on-disk layout is
designed so that it is always possible. Callers that need durability across
crashes should write to a temporary file and atomically rename it into place.

---

## 4. Compression & Encoding
The `compression_type` field (header offset 22) selects a per-chunk block
compression algorithm applied uniformly to every chunk in the file:
`0 = none`, `1 = ZSTD`, `2 = LZ4`.

The compression layer sits **below** the columnar/delta encoding of §2.2: a
chunk is first built as its self-describing header + delta-encoded columns (the
*payload*), and then, if a codec is set, that payload is compressed and wrapped
in a small per-chunk frame:

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | `uncompressed_size` | uint32 | Byte size of the payload after decoding (lets a reader size its decode buffer exactly) |
| 4 | `payload` | bytes | The codec's compressed payload (e.g. a single ZSTD frame) |

The index entry's `chunk_offset` points at this frame; `chunk_size` is the
frame's byte size (`4 + payload`); and `chunk_crc32` is computed over the frame.
Because the CRC and the time bounds (`chunk_time_base` / `chunk_last_timestamp`)
all live in the index over the *on-disk* bytes, **corruption detection (CRC) and
range pruning never need to decompress** — only chunks that actually fall in a
query's window are decoded. The decode buffer size comes from
`uncompressed_size`, and a decoded length disagreeing with it is treated as
corruption. (The one part of `ticks_verify` that does decode a compressed chunk
is the record-count cross-check, which reads each chunk's `num_records` from the
decoded payload; the CRC pass itself runs on the on-disk bytes.)

With `compression_type = 0` no frame is written: chunks are stored exactly as in
§2.2 and the file is byte-compatible with version 4.

> **Status:** ZSTD (`1`) is implemented. LZ4 (`2`) remains reserved — a reader
> rejects a file (and `ticks_new_file` rejects a header) whose `compression_type`
> it cannot decode. The ZSTD library is built via CMake `FetchContent`.

---

## 5. Version History
| Version | Date | Changes |
|----------|------|----------|
| 5 | 2026-06-02 | Per-chunk block compression. When `compression_type` names a codec (ZSTD implemented), each chunk's columnar payload is compressed and wrapped in a 4-byte frame (`uncompressed_size` + payload); `chunk_size`/`chunk_crc32` describe the on-disk frame so verify and range pruning still need no decode. `compression_type = 0` files stay byte-compatible with v4. Added a `SCHEMA_*`-style guard rejecting unsupported codecs at open/create. |
| 4 | 2026-06-02 | Schema-aware, generic columnar chunks. Added `schema_id` to the header (carved from reserved; region stays 72 bytes) selecting the record layout (`trade` = 3 columns, `quote` = 5 columns). The chunk header is now variable-length and self-describing per column (count + per-column base/width/encoding), so the format is generic over the column set. Dropped the per-column width bytes from each index entry (now authoritative in the chunk header), shrinking the entry 35 → 32 bytes. Added typed `ticks_add_quotes` / `ticks_iterator_next_quote` and a `SCHEMA_MISMATCH` status. |
| 3 | 2026-06-02 | Added a denormalized file-level summary to the header — `record_count`, `min_timestamp`, `max_timestamp` — so a catalog can answer "how many ticks / what time span" from the header alone without reading the index (header region 48 → 72 bytes). All three are `0` for an empty file. `ticks_verify` now cross-checks the summary against the index and chunk headers. |
| 2 | 2026-06-02 | Columnar (struct-of-arrays) chunk layout; delta encoding of every column (timestamps unsigned, price/volume zig-zag signed); self-describing per-chunk header (`num_records`, bases, delta widths); added `price_scale`/`volume_scale` and 6 reserved header bytes (header region 40 → 48 bytes); added `chunk_last_timestamp` to each index entry for full range pruning (index entry 27 → 35 bytes). |
| 1 | 2026-06-02 | Explicit little-endian on-disk layout; added `version` and per-chunk `chunk_crc32`; documented fixed offsets. |
| (draft) | 2025-10-05 | Initial specification |
</content>
</invoke>
