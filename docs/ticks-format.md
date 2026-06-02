# File Type Specification — `.ticks`
- **Version:** `1`
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

A file is laid out as: a fixed 40-byte header region, followed by the chunk
data, followed by the index. The header stores the absolute offset and byte
size of the index so a reader can locate it without scanning.

### 2.1 Header region (40 bytes)
| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0  | `magic_number` | char[4] | `"TICK"` (`0x54 0x49 0x43 0x4B`) |
| 4  | `version` | uint16 | Format version (currently `1`) |
| 6  | `endianness` | uint8 | On-disk byte order: 0 = undefined, 1 = little, 2 = big (always written as `1`) |
| 7  | `ticker` | char[8] | Instrument code (e.g., `GBPJPY` or `AAPL`) |
| 15 | `currency` | char[3] | ISO currency code (e.g., `USD`) |
| 18 | `country` | char[2] | ISO country code (e.g., `AU`) |
| 20 | `asset_class` | uint16 | Enum for asset class |
| 22 | `compression_type` | uint16 | Enum for compression algorithm |
| 24 | `index_offset` | uint64 | Byte offset to the index section |
| 32 | `index_size` | uint64 | Byte size of the index section |

Fixed-width text fields (`ticker`, `currency`, `country`) are NUL-padded if the
value is shorter than the field.

---

### 2.2 Chunks (up to 16 MB per chunk)
Chunk data begins at offset 40 (immediately after the header region). Each
chunk is a packed array of tick rows. A single column width is chosen per chunk
as the minimum that fits every value in that chunk, so all rows within a chunk
share the same `timestamp_size` / `price_size` / `volume_size` (recorded in the
chunk's index entry).

Each row, in order:

| Field | Type | Description |
|--------|------|-------------|
| `time_delta` | uintN | Milliseconds since the chunk's `chunk_time_base` (unsigned; ticks must be non-decreasing in time) |
| `price` | uintN | Price (integer; scale is application-defined) |
| `volume` | uintN | Trade volume |

`N` is the per-column width from the index entry (1, 2, 4, or 8 bytes).

---

### 2.3 Index (27 bytes per entry)
The index is a packed array of fixed-size entries, one per chunk, located at
`index_offset` and spanning `index_size` bytes
(`index_size = entry_count × 27`).

| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0  | `chunk_time_base` | uint64 | Epoch timestamp (ms) of the first tick in the chunk |
| 8  | `chunk_offset` | uint64 | File offset where the chunk starts |
| 16 | `chunk_size` | uint32 | Byte size of the chunk on disk |
| 20 | `chunk_crc32` | uint32 | CRC32 (IEEE 802.3) of the chunk's on-disk bytes |
| 24 | `timestamp_size` | uint8 | Column width: 1, 2, 4, or 8 bytes |
| 25 | `price_size` | uint8 | Column width: 1, 2, 4, or 8 bytes |
| 26 | `volume_size` | uint8 | Column width: 1, 2, 4, or 8 bytes |

---

## 3. Integrity
Each index entry stores a `chunk_crc32` computed over the chunk's on-disk bytes.
Readers can recompute and compare it to detect corruption (`ticks_verify`).

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
| 1 | 2026-06-02 | Explicit little-endian on-disk layout; added `version` and per-chunk `chunk_crc32`; documented fixed offsets. |
| (draft) | 2025-10-05 | Initial specification |
</content>
</invoke>
