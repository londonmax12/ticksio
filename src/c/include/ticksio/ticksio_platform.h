#ifndef TICKSIO_PLATFORM_H
#define TICKSIO_PLATFORM_H

#include <time.h>

// Portable implementation of timegm for Windows and other platforms
static time_t timegm_portable(struct tm *t) {
    #if defined(_WIN32)
        return _mkgmtime(t);
    #else
        return timegm(t);
    #endif
}

#endif // TICKSIO_PLATFORM_H