#include "cputil.h"

#include <stdlib.h>
#include <time.h>

uint64_t timestampUsec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void uarmAbort() {
    abort();
}
