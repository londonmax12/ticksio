#ifndef TICKSIO_COMPRESS_H
#define TICKSIO_COMPRESS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ticksio/ticksio_types.h"

// Per-chunk block compression (format v5+). A file's compression_type selects
// the codec uniformly for every chunk; COMPRESSION_NONE means chunks are written
// verbatim and these helpers are not used. See TICKS_COMPRESS_FRAME_HEADER_SIZE
// in ticksio_constants.h for the on-disk frame layout.

// Whether this build can read/write a file declaring `type` (COMPRESSION_NONE is
// always supported; codecs depend on what is compiled in). Used to reject files
// the library cannot decode at open/create time rather than failing mid-decode.
int ticks_compression_supported(compression_type_e type);

// Worst-case framed output size for compressing `src_size` payload bytes with
// `type` (includes the frame header). 0 if `type` is unsupported / NONE.
size_t ticks_compress_bound(compression_type_e type, size_t src_size);

// Compress src[0..src_size) into the framed on-disk layout, writing to dst (cap
// dst_cap) and setting *out_size to the framed byte count. `type` must name a
// supported codec (callers handle COMPRESSION_NONE without calling this).
ticks_status_e ticks_compress_chunk(compression_type_e type,
                                    const uint8_t* src, size_t src_size,
                                    uint8_t* dst, size_t dst_cap, size_t* out_size);

// Reverse of ticks_compress_chunk: read the frame in src[0..src_size), decode the
// payload into dst (cap dst_cap), and set *out_size to the decompressed size.
// Fails (TICKS_ERROR_CORRUPT_DATA) if the decoded size disagrees with the frame.
ticks_status_e ticks_decompress_chunk(compression_type_e type,
                                      const uint8_t* src, size_t src_size,
                                      uint8_t* dst, size_t dst_cap, size_t* out_size);

#ifdef __cplusplus
}
#endif

#endif // TICKSIO_COMPRESS_H
