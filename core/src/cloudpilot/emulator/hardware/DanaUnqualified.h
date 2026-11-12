#ifndef DANA_UNQUALIFIED_H
#define DANA_UNQUALIFIED_H

#include <cstdarg>
#include <cstdio>
#include <map>
#include <string>

#include "EmCommon.h"
#include "Logging.h"

// Guest access the Dana model hasn't qualified. The machine keeps running: reads see the floating
// bus and writes are dropped. Each kind logs its first access, then again at every power-of-two
// count. Emulation thread only; the counts aren't saved.
inline std::map<std::string, uint64>& DanaUnqualifiedCounts() {
    static std::map<std::string, uint64> counts;
    return counts;
}

inline uint64 DanaUnqualifiedCount(const std::string& kind) {
    auto& counts = DanaUnqualifiedCounts();
    auto found = counts.find(kind);
    return found == counts.end() ? 0 : found->second;
}

inline uint64 DanaUnqualifiedTotal() {
    uint64 total = 0;
    for (auto& entry : DanaUnqualifiedCounts()) total += entry.second;
    return total;
}

inline void DanaUnqualified(const char* kind, const char* format, ...)
    __attribute__((format(printf, 2, 3)));

inline void DanaUnqualified(const char* kind, const char* format, ...) {
    uint64 count = ++DanaUnqualifiedCounts()[kind];
    if (count & (count - 1)) return;
    char detail[160];
    va_list args;
    va_start(args, format);
    vsnprintf(detail, sizeof(detail), format, args);
    va_end(args);
    logPrintf("Dana: ignored %s: %s (%llu so far)", kind, detail, (unsigned long long)count);
}

#endif
