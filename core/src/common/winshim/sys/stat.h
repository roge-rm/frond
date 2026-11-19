#pragma once
#include <sys/types.h>
#include_next <sys/stat.h>
// 64-bit sizes and times, and no symlinks to tell apart.
#define stat _stat64
#define fstat _fstat64
#define lstat _stat64
#ifdef __cplusplus
#include <wchar.h>
inline int _stat64(const wchar_t* path, struct _stat64* info) { return _wstat64(path, info); }
#endif
