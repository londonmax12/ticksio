#include "ticksio/ticksio_compress.h"

#include <zstd.h>

#include "ticksio/ticksio_constants.h"
#include "ticksio/ticksio_helpers.h"

int ticks_compression_supported(compression_type_e type) {
    switch (type) {
        case COMPRESSION_NONE:
        case COMPRESSION_ZSTD:
            return 1;
        default:
            return 0; // LZ4 (and any future codec) is reserved but not compiled in
    }
}

size_t ticks_compress_bound(compression_type_e type, size_t src_size) {
    switch (type) {
        case COMPRESSION_ZSTD:
            return TICKS_COMPRESS_FRAME_HEADER_SIZE + ZSTD_compressBound(src_size);
        default:
            return 0;
    }
}

ticks_status_e ticks_compress_chunk(compression_type_e type,
                                    const uint8_t* src, size_t src_size,
                                    uint8_t* dst, size_t dst_cap, size_t* out_size) {
    if (src == NULL || dst == NULL || out_size == NULL)
        return TICKS_ERROR_INVALID_ARGUMENTS;
    if (type != COMPRESSION_ZSTD)
        return TICKS_ERROR_INVALID_ARGUMENTS;
    // The uncompressed size is stored as a uint32 in the frame; chunks are capped
    // at MAX_CHUNK_SIZE (16 MB) so this always holds, but guard it explicitly.
    if (src_size > UINT32_MAX)
        return TICKS_ERROR_INVALID_ARGUMENTS;
    if (dst_cap < TICKS_COMPRESS_FRAME_HEADER_SIZE)
        return TICKS_ERROR_COMPRESSION;

    uint8_t* payload = dst + TICKS_COMPRESS_FRAME_HEADER_SIZE;
    size_t payload_cap = dst_cap - TICKS_COMPRESS_FRAME_HEADER_SIZE;
    size_t csize = ZSTD_compress(payload, payload_cap, src, src_size, TICKS_ZSTD_LEVEL);
    if (ZSTD_isError(csize))
        return TICKS_ERROR_COMPRESSION;

    le_put_u32(dst, (uint32_t)src_size);
    *out_size = TICKS_COMPRESS_FRAME_HEADER_SIZE + csize;
    return TICKS_OK;
}

ticks_status_e ticks_decompress_chunk(compression_type_e type,
                                      const uint8_t* src, size_t src_size,
                                      uint8_t* dst, size_t dst_cap, size_t* out_size) {
    if (src == NULL || dst == NULL || out_size == NULL)
        return TICKS_ERROR_INVALID_ARGUMENTS;
    if (type != COMPRESSION_ZSTD)
        return TICKS_ERROR_INVALID_ARGUMENTS;
    if (src_size < TICKS_COMPRESS_FRAME_HEADER_SIZE)
        return TICKS_ERROR_INVALID_FORMAT;

    const uint32_t usize = le_get_u32(src);
    if (usize > dst_cap)
        return TICKS_ERROR_INVALID_FORMAT; // frame claims more than the caller allotted

    size_t got = ZSTD_decompress(dst, dst_cap,
                                 src + TICKS_COMPRESS_FRAME_HEADER_SIZE,
                                 src_size - TICKS_COMPRESS_FRAME_HEADER_SIZE);
    // A decode error or a length that disagrees with the frame means the stored
    // bytes are not what the writer produced — treat as corruption.
    if (ZSTD_isError(got) || got != usize)
        return TICKS_ERROR_CORRUPT_DATA;

    *out_size = got;
    return TICKS_OK;
}
