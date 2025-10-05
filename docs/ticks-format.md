# File Type Specification — `.ticks`
- **Version:** `1.0`
- **Author:** London Ball
- **Last Updated:** 2025-10-05

---

## 1. Overview
The `.ticks` file format provides an efficient storage solution for tick-level financial data, optimized for both compression and sequential access.

---

## 2. File Structure

### 2.1 Header (28 bytes)
| Field | Type | Description |
|--------|------|-------------|
| `magic_number` | 4 bytes | `"TICK"` (`0x54 0x49 0x43 0x4B`) |
| `version` | uint16 | Format version (e.g., 1) |
| `endianness` | uint8 | 0 = little endian, 1 = big endian |
| `ticker` | char[8] | Instrument code (e.g., `GBPJPY` or `AAPL`) |
| `currency` | char[3] | ISO currency code (e.g., `USD`) |
| `asset_class` | uint16 | Enum for asset class |
| `country_code` | char[2] | ISO country code (e.g., `AU`) |
| `compression_type` | uint16 | Enum for compression algorithm |
| `index_offset` | uint64 | Byte offset to index section |

---

### 2.2 Chunks (~32 MB per chunk before compression)
Each chunk contains a set of tick rows before compression.

| Field | Description |
|--------|-------------|
| `time_delta` | Time difference from previous tick (variable length) |
| `price` | Price in cents (variable length) |
| `volume` | Trade volume (variable length) |

---

### 2.3 Index (19 bytes per entry)
Each entry points to a compressed chunk.

| Field | Type | Description |
|--------|------|-------------|
| `chunk_start_time` | uint64 | Epoch timestamp of first tick in chunk |
| `chunk_offset` | uint64 | File offset where chunk starts |
| `row_sizes.time_delta_size` | uint8 | 0=int8, 1=int16, 2=int32, 3=int64 |
| `row_sizes.price_size` | uint8 | 0=int8, 1=int16, 2=int32, 3=int64 |
| `row_sizes.volume_size` | uint8 | 0=int8, 1=int16, 2=int32, 3=int64 |

---

## 3. Compression & Encoding
Each chunk is compressed using a **block-based compression algorithm**.  
The specific algorithm is defined in the header’s `compression_type` field.  
Endianness is specified by the `endianness` field (0 = little-endian, 1 = big-endian).

---

## 4. Version History
| Version | Date | Changes |
|----------|------|----------|
| 1.0 | 2025-10-05 | Initial specification |
