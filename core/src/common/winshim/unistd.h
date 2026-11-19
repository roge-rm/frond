#pragma once
#include <sys/types.h>
#include_next <unistd.h>
#include <io.h>
#include <errno.h>
#include <wchar.h>
#include <stdint.h>
#ifdef __cplusplus
#include <mutex>
#include <string.h>
#include <direct.h>
extern "C++" {
// pread and pwrite as a seek and a read under one lock; every positional access goes through these.
inline std::mutex& frondWinIoLock() { static std::mutex lock; return lock; }
inline ssize_t pread(int fd, void* buffer, size_t count, off_t offset) {
    std::lock_guard<std::mutex> guard(frondWinIoLock());
    if (_lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    return _read(fd, buffer, unsigned(count));
}
inline ssize_t pwrite(int fd, const void* buffer, size_t count, off_t offset) {
    std::lock_guard<std::mutex> guard(frondWinIoLock());
    if (_lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    return _write(fd, buffer, unsigned(count));
}
inline int fsync(int fd) { return _commit(fd); }
inline int fchmod(int, unsigned) { return 0; }
inline char* mkdtemp(char* pattern) {
    if (_mktemp_s(pattern, strlen(pattern) + 1) != 0) return nullptr;
    return _mkdir(pattern) == 0 ? pattern : nullptr;
}
inline unsigned geteuid() { return 0; }
extern "C++" {
inline int unlink(const wchar_t* path) { return _wunlink(path); }
inline int rmdir(const wchar_t* path) { return _wrmdir(path); }
}
}
#endif
