#pragma once
#include <sys/types.h>
#include_next <sys/stat.h>
#ifdef __cplusplus
#include <wchar.h>
extern "C++" {
// stat on a std::filesystem path (wchar_t here), and lstat, which has no links to tell apart.
inline int stat(const wchar_t* path, struct stat* info) {
    return _wstat64i32(path, reinterpret_cast<struct _stat64i32*>(info));
}
inline int lstat(const char* path, struct stat* info) { return stat(path, info); }
inline int lstat(const wchar_t* path, struct stat* info) { return stat(path, info); }
}
#endif
