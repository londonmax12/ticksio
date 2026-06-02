# File Type Specification — `.ticks`
- **Version:** `3`
- **Author:** London Ball (@londonmax12 on Github)
- **Last Updated:** 2026-06-02

---

## 1. Overview
The `.ticks` file format provides an efficient storage solution for tick-level
financial data, optimized for both compression and sequential access.

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
| 4  | `version` | uint16 | Format version (currently `3`) |
| 6  | `endianness` | uint8 | On-disk byte order: 0 = undefined, 1 = little, 2 = big (always written as `1`) |
| 7  | `ticker` | char[8] | Instrument code (e.g., `GBPJPY` or `AAPL`) |
| 15 | `currency` | char[3] | ISO currency code (e.g., `USD`) |
| 18 | `country` | char[2] | ISO country code (e.g., `AU`) |
| 20 | `asset_class` | uint16 | Enum for asset class |
| 22 | `compression_type` | uint16 | Enum for compression algorithm |
| 24 | `price_scale` | int8 | Base-10 exponent: real price = stored `price` × 10^`price_scale` (e.g. `-2` ⇒ prices are in cents) |
| 25 | `volume_scale` | int8 | Base-10 exponent applied to `volume` the same way |
| 26 | `record_count` | uint64 | Total number of ticks across all chunks (`0` if the file is empty) |
| 34 | `min_timestamp` | uint64 | Epoch ms of the first tick in the file (`0` if empty) |
| 42 | `max_timestamp` | uint64 | Epoch ms of the last tick in the file (`0` if empty) |
| 50 | `reserved` | byte[6] | Reserved for forward-compatible fields; written as zero |
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
Chunk data begins at offset 48 (immediately after the header region). Each chunk
is **self-describing** (it starts with its own header) and **columnar**: rather
than interleaving fields row-by-row, it stores all timestamps, then all prices,
then all volumes. Within a chunk the columns are **delta-encoded** against the
previous record, so each column's width is governed by how much consecutive
ticks *move*, not by their absolute magnitude.

#### 2.2.1 Chunk header (31 bytes)
| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0  | `num_records` | uint32 | Number of records (ticks) in the chunk |
| 4  | `ts_base` | uint64 | Absolute timestamp (ms) of the first tick (equals the index entry's `chunk_time_base`) |
| 12 | `price_base` | uint64 | Absolute price of the first tick |
| 20 | `volume_base` | uint64 | Absolute volume of the first tick |
| 28 | `ts_delta_size` | uint8 | Width of each timestamp delta: 1, 2, 4, or 8 bytes |
| 29 | `price_delta_size` | uint8 | Width of each price delta |
| 30 | `volume_delta_size` | uint8 | Width of each volume delta |

#### 2.2.2 Columns
Immediately after the chunk header come three contiguous columns, each holding
`num_records − 1` deltas (the first record is the `*_base` value in the header):

1. **Timestamps** — `num_records − 1` values of `ts_delta_size` bytes. Each is
   the unsigned millisecond gap from the previous tick (ticks are non-decreasing
   in time, so these are never negative).
2. **Prices** — `num_records − 1` values of `price_delta_size` bytes. Each is the
   signed change from the previous price, **zig-zag-encoded** to an unsigned
   integer (`0 → 0, −1 → 1, 1 → 2, −2 → 3, …`).
3. **Volumes** — `num_records − 1` values of `volume_delta_size` bytes, signed
   change from the previous volume, zig-zag-encoded.

To reconstruct record `i` (`i ≥ 1`): `value[i] = value[i−1] + delta[i−1]`
(un-zig-zag the price/volume deltas first); `value[0]` is the `*_base` field.

A single-record chunk (`num_records == 1`) carries no deltas and is just the
31-byte header.

The widths are recorded in both the chunk header (authoritative) and the index
entry (a redundant accelerator). Because each chunk is self-describing, the
index can be rebuilt by scanning chunk headers, and a chunk decoded without the
index at all.

---

### 2.3 Index (35 bytes per entry)
The index is a packed array of fixed-size entries, one per chunk, located at
`index_offset` and spanning `index_size` bytes
(`index_size = entry_count × 35`).

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0  | `chunk_time_base` | uint64 | Epoch timestamp (ms) of the first tick in the chunk |
| 8  | `chunk_last_timestamp` | uint64 | Epoch timestamp (ms) of the last tick in the chunk |
| 16 | `chunk_offset` | uint64 | File offset where the chunk starts |
| 24 | `chunk_size` | uint32 | Byte size of the chunk on disk (header + columns) |
| 28 | `chunk_crc32` | uint32 | CRC32 (IEEE 802.3) of the chunk's on-disk bytes |
| 32 | `timestamp_size` | uint8 | Timestamp delta width: 1, 2, 4, or 8 bytes |
| 33 | `price_size` | uint8 | Price delta width: 1, 2, 4, or 8 bytes |
| 34 | `volume_size` | uint8 | Volume delta width: 1, 2, 4, or 8 bytes |

The three width bytes mirror the chunk header's `*_delta_size` fields.

`chunk_time_base` and `chunk_last_timestamp` together bound each chunk's time
span. Because chunks are stored in non-decreasing time order, a range query can
prune any chunk whose `[chunk_time_base, chunk_last_timestamp]` falls outside
the requested window using the index alone — no chunk needs to be read or
decoded. Storing the last timestamp explicitly (rather than inferring a chunk's
end from the *next* chunk's `chunk_time_base`) is what lets the **final** chunk
be pruned too, since it has no successor.

---

## 3. Integrity
Each index entry stores a `chunk_crc32` computed over the chunk's on-disk bytes
(its self-describing header plus the three columns). Readers can recompute and
compare it to detect corruption (`ticks_verify`). The same pass also recomputes
the file-level summary — `record_count` from the summed chunk `num_records`, and
`min_timestamp` / `max_timestamp` from the index extremes — and fails with a
corruption error if it disagrees with the values stored in the header.

---

## 4. Compression & Encoding
The `compression_type` field reserves an enum for a per-chunk block compression
algorithm (`0 = none`, `1 = ZSTD`, `2 = LZ4`).

> **Status:** Not yet implemented. Chunks are currently stored uncompressed
> (`compression_type = 0`), and `chunk_size` is the uncompressed size. Treat the
> ZSTD/LZ4 values as reserved until a future version.

---

## 5. Version History
| Version | Date | Changes |
|----------|------|----------|
| 3 | 2026-06-02 | Added a denormalized file-level summary to the header — `record_count`, `min_timestamp`, `max_timestamp` — so a catalog can answer "how many ticks / what time span" from the header alone without reading the index (header region 48 → 72 bytes). All three are `0` for an empty file. `ticks_verify` now cross-checks the summary against the index and chunk headers. |
| 2 | 2026-06-02 | Columnar (struct-of-arrays) chunk layout; delta encoding of every column (timestamps unsigned, price/volume zig-zag signed); self-describing per-chunk header (`num_records`, bases, delta widths); added `price_scale`/`volume_scale` and 6 reserved header bytes (header region 40 → 48 bytes); added `chunk_last_timestamp` to each index entry for full range pruning (index entry 27 → 35 bytes). |
| 1 | 2026-06-02 | Explicit little-endian on-disk layout; added `version` and per-chunk `chunk_crc32`; documented fixed offsets. |
| (draft) | 2025-10-05 | Initial specification |
</content>
</invoke>
