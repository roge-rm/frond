#pragma once
#include <sys/types.h>
#include_next <sys/stat.h>
#ifdef __cplusplus
#include <wchar.h>
extern "C++" {
// stat on a std::filesystem path (wchar_t here), and lstat, which has no links to tell apart.
inline int stat(const wchar_t* path, struct stat* info) {
    struct _stat64 wide{};
    if (_wstat64(path, &wide) < 0) return -1;
    info->st_dev = wide.st_dev;
    info->st_ino = wide.st_ino;
    info->st_mode = wide.st_mode;
    info->st_nlink = wide.st_nlink;
    info->st_uid = wide.st_uid;
    info->st_gid = wide.st_gid;
    info->st_rdev = wide.st_rdev;
    info->st_size = wide.st_size;
    info->st_atime = wide.st_atime;
    info->st_mtime = wide.st_mtime;
    info->st_ctime = wide.st_ctime;
    return 0;
}
inline int lstat(const char* path, struct stat* info) { return stat(path, info); }
inline int lstat(const wchar_t* path, struct stat* info) { return stat(path, info); }
}
#endif
