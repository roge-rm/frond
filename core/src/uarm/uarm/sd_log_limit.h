#ifndef _SD_LOG_LIMIT_H_
#define _SD_LOG_LIMIT_H_

#include <cstdint>
#include <cstdio>

// Limits repeated SD/MMC error lines on stderr. The first SD_LOG_FULL are logged in full,
// then one summary line per SD_LOG_EVERY suppressed repeats. It counts events only, so
// output is the same for a given run. Logging state only: never saved.
struct SdLogLimit {
    uint32_t seen;
};

#define SD_LOG_FULL 8
#define SD_LOG_EVERY 4096

// True when this occurrence should be logged in full.
static inline bool sdLogAllow(SdLogLimit *l, const char *prefix, const char *what) {
    if (l->seen < 0xffffffffu) l->seen++;
    if (l->seen <= SD_LOG_FULL) return true;
    uint32_t suppressed = l->seen - SD_LOG_FULL;
    if (!(suppressed % SD_LOG_EVERY))
        fprintf(stderr, "%s: %u more %s suppressed\n", prefix, (unsigned)suppressed, what);
    return false;
}

static inline void sdLogReset(SdLogLimit *l) { l->seen = 0; }

#endif
