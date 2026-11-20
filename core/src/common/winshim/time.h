#pragma once
#include_next <time.h>
// localtime_r and gmtime_r on the CRT's thread-safe calls.
static inline struct tm* frondLocaltimeR(const time_t* t, struct tm* out) { return localtime_s(out, t) == 0 ? out : 0; }
static inline struct tm* frondGmtimeR(const time_t* t, struct tm* out) { return gmtime_s(out, t) == 0 ? out : 0; }
#define localtime_r frondLocaltimeR
#define gmtime_r frondGmtimeR
