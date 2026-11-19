#pragma once
#include <errno.h>
#include_next <stdio.h>
#ifdef __cplusplus
extern "C" {
extern int __stdcall MoveFileExA_(const char*, const char*, unsigned long) __asm__("MoveFileExA");
// The link step wraps rename (-Wl,--wrap=rename) so it replaces an existing file like POSIX rename.
__attribute__((weak)) int __wrap_rename(const char* from, const char* to) {
    if (MoveFileExA_(from, to, 1 /* MOVEFILE_REPLACE_EXISTING */ | 8 /* MOVEFILE_WRITE_THROUGH */)) return 0;
    errno = EACCES;
    return -1;
}
}
#endif
