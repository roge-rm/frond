#pragma once
#include <winsock2.h>
#include <windows.h>
#include <errno.h>
#include <io.h>
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
// flock on the whole file through LockFileEx; the lock goes when the handle closes.
static inline int flock(int fd, int operation) {
    const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (handle == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    OVERLAPPED range{};
    if (operation & LOCK_UN) return UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &range) ? 0 : -1;
    DWORD flags = (operation & LOCK_EX) ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    if (operation & LOCK_NB) flags |= LOCKFILE_FAIL_IMMEDIATELY;
    if (LockFileEx(handle, flags, 0, MAXDWORD, MAXDWORD, &range)) return 0;
    errno = (operation & LOCK_NB) ? EWOULDBLOCK : EACCES;
    return -1;
}
