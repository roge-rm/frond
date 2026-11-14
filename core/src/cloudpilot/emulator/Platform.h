#ifndef _PLATFORM_H_
#define _PLATFORM_H_

#include <cstdlib>

#include "EmCommon.h"

namespace Platform {
    template <class T>
    void DisposeMemory(T*& p);

    void* AllocateMemory(size_t count);

    void* AllocateMemoryClear(size_t count);

    uint64 GetMilliseconds();

    void GetTime(uint32& hour, uint32& min, uint32& sec);

    void GetDate(uint32& year, uint32& month, uint32& day);

    // The clock GetTime and GetDate read: Unix seconds, taken as UTC. nullptr (the default) is
    // the host's local time.
    void SetClock(int64 (*secondsNow)());

    uint32 Random();
}  // namespace Platform

///////////////////////////////////////////////////////////////////////////////
// IMPLEMENTATION
///////////////////////////////////////////////////////////////////////////////

template <class T>
void Platform::DisposeMemory(T*& p) {
    if (p) {
        free(p);

        p = nullptr;
    }
}

#endif  // _PLATFORM_J_
