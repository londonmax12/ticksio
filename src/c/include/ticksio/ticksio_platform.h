#ifndef TICKSIO_PLATFORM_H
#define TICKSIO_PLATFORM_H

#include <time.h>
#include <stdio.h>
#include <stdint.h>

// Portable implementation of timegm for Windows and other platforms
static inline time_t timegm_portable(struct tm *t) {
    #if defined(_WIN32)
        return _mkgmtime(t);
    #else
        return timegm(t);
    #endif
}

// Portable 64-bit file seek. On Windows `long` (and thus fseek) is 32-bit, so
// files larger than 2 GB require the _fseeki64/fseeko variants.
static inline int ticks_fseek64(FILE* stream, int64_t offset, int origin) {
    #if defined(_WIN32)
        return _fseeki64(stream, offset, origin);
    #else
        return fseeko(stream, (off_t)offset, origin);
    #endif
}

// Portable 64-bit file position query. Returns -1 on error.
static inline int64_t ticks_ftell64(FILE* stream) {
    #if defined(_WIN32)
        return _ftelli64(stream);
    #else
        return (int64_t)ftello(stream);
    #endif
}

#endif // TICKSIO_PLATFORM_H