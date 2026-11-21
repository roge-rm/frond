#pragma once
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
// The Win32 calls are declared here so windows.h stays out of the core's sources.
namespace frondWin {
struct LockRange { uintptr_t internal, internalHigh; uint32_t offset, offsetHigh; void* event; };
extern "C++" {
int __stdcall lockFile(void*, unsigned long, unsigned long, unsigned long, unsigned long, LockRange*) __asm__("LockFileEx");
int __stdcall unlockFile(void*, unsigned long, unsigned long, unsigned long, LockRange*) __asm__("UnlockFileEx");
}
}
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
// Reserve a byte beyond the data so other handles can still read and write the file.
static inline int flock(int fd, int operation) {
    void* const handle = reinterpret_cast<void*>(_get_osfhandle(fd));
    if (reinterpret_cast<intptr_t>(handle) == -1) { errno = EBADF; return -1; }
    frondWin::LockRange range{};
    constexpr uint64_t lockOffset = INT64_MAX - 1;
    range.offset = uint32_t(lockOffset);
    range.offsetHigh = uint32_t(lockOffset >> 32);
    if (operation & LOCK_UN) return frondWin::unlockFile(handle, 0, 1, 0, &range) ? 0 : -1;
    unsigned long flags = (operation & LOCK_EX) ? 2 /* LOCKFILE_EXCLUSIVE_LOCK */ : 0;
    if (operation & LOCK_NB) flags |= 1 /* LOCKFILE_FAIL_IMMEDIATELY */;
    if (frondWin::lockFile(handle, flags, 0, 1, 0, &range)) return 0;
    errno = (operation & LOCK_NB) ? EWOULDBLOCK : EACCES;
    return -1;
}
