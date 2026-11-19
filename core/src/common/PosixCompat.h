#pragma once
// Small helpers for the Linux-only calls the core uses, so the same code builds on macOS.
// On Linux each one is the plain call.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // the mains ignore SIGPIPE
#endif

#ifndef POLLRDHUP
#define POLLRDHUP 0  // no peer-close event here; POLLHUP and a zero-length read show it
#endif

namespace frondPosix {

#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
inline constexpr int kCloexec = SOCK_CLOEXEC;
inline constexpr int kNonblock = SOCK_NONBLOCK;
#else
inline constexpr int kCloexec = 0x40000000;
inline constexpr int kNonblock = 0x20000000;
#endif

// socket() taking kCloexec and kNonblock in type; the flags are set with fcntl where the system has no
// such socket types.
inline int Socket(int domain, int type, int protocol) {
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
    return ::socket(domain, type, protocol);
#elif defined(_WIN32)
    const SOCKET sock = ::socket(domain, type & ~(kCloexec | kNonblock), protocol);
    if (sock == INVALID_SOCKET) return -1;
    if (type & kNonblock) {
        u_long on = 1;
        ioctlsocket(sock, FIONBIO, &on);
    }
    return int(sock);
#else
    const int fd = ::socket(domain, type & ~(kCloexec | kNonblock), protocol);
    if (fd < 0) return fd;
    if (type & kCloexec) fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
    if (type & kNonblock) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    return fd;
#endif
}

// Modified time of a stat result in nanoseconds.
inline uint64_t MtimeNs(const struct stat& st) {
#if defined(_WIN32)
    return uint64_t(st.st_mtime) * 1000000000ull;
#elif defined(__APPLE__)
    return uint64_t(st.st_mtimespec.tv_sec) * 1000000000ull + uint64_t(st.st_mtimespec.tv_nsec);
#else
    return uint64_t(st.st_mtim.tv_sec) * 1000000000ull + uint64_t(st.st_mtim.tv_nsec);
#endif
}

// Reserves length bytes of the file like posix_fallocate: 0, ENOSPC when full, or another error
// number where the file system can't (the caller then writes the bytes).
inline int Preallocate(int fd, uint64_t length) {
#if defined(_WIN32)
    return _chsize_s(fd, __int64(length));
#elif defined(__APPLE__)
    fstore_t store{F_ALLOCATEALL | F_ALLOCATECONTIG, F_PEOFPOSMODE, 0, off_t(length), 0};
    if (fcntl(fd, F_PREALLOCATE, &store) == -1) {
        store.fst_flags = F_ALLOCATEALL;
        if (fcntl(fd, F_PREALLOCATE, &store) == -1) return errno == ENOSPC ? ENOSPC : EOPNOTSUPP;
    }
    return ftruncate(fd, off_t(length)) == 0 ? 0 : errno;
#else
    return posix_fallocate(fd, 0, off_t(length));
#endif
}

// A pipe pair with both ends non-blocking and close-on-exec, for waking a poll() loop.
inline bool WakePipe(int& readEnd, int& writeEnd) {
#ifdef _WIN32
    (void)readEnd;
    (void)writeEnd;
    return false;  // only the host serial worker uses it, and that is off here
#else
    int ends[2];
    if (pipe(ends) != 0) return false;
    for (const int end : ends) {
        fcntl(end, F_SETFD, fcntl(end, F_GETFD) | FD_CLOEXEC);
        fcntl(end, F_SETFL, fcntl(end, F_GETFL) | O_NONBLOCK);
    }
    readEnd = ends[0];
    writeEnd = ends[1];
    return true;
#endif
}

}  // namespace frondPosix
