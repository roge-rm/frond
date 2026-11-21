#pragma once
#include <sys/types.h>
#include_next <unistd.h>
#include <io.h>
#include <errno.h>
#include <wchar.h>
#include <stdint.h>
#ifdef __cplusplus
#include <mutex>
#include <limits.h>
#include <string.h>
#include <direct.h>
extern "C++" {
// The storage handles use positional I/O. Keep their CRT cursor unchanged.
inline std::mutex& frondWinIoLock() { static std::mutex lock; return lock; }
template <typename Buffer, typename Operation>
inline ssize_t frondWinAt(int fd, Buffer buffer, size_t count, int64_t offset, Operation operation) {
    if (offset < 0) { errno = EINVAL; return -1; }
    std::lock_guard<std::mutex> guard(frondWinIoLock());
    const auto previous = _lseeki64(fd, 0, SEEK_CUR);
    if (previous < 0 || _lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    const auto result = operation(fd, buffer, unsigned(count > INT_MAX ? INT_MAX : count));
    const int error = errno;
    if (_lseeki64(fd, previous, SEEK_SET) < 0) return -1;
    errno = error;
    return result;
}
inline ssize_t pread(int fd, void* buffer, size_t count, int64_t offset) {
    return frondWinAt(fd, buffer, count, offset, _read);
}
inline ssize_t pwrite(int fd, const void* buffer, size_t count, int64_t offset) {
    return frondWinAt(fd, buffer, count, offset, _write);
}
inline int fsync(int fd) { return _commit(fd); }
inline int fchmod(int, unsigned) { return 0; }
inline unsigned geteuid() { return 0; }
extern "C++" {
inline int unlink(const wchar_t* path) { return _wunlink(path); }
inline int rmdir(const wchar_t* path) { return _wrmdir(path); }
}
}
#endif
