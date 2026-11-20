#pragma once
#include <errno.h>
#include_next <stdio.h>
#ifdef __cplusplus
extern "C" {
extern int __stdcall MoveFileExA_(const char*, const char*, unsigned long) __asm__("MoveFileExA");
extern int __stdcall MoveFileExW_(const wchar_t*, const wchar_t*, unsigned long) __asm__("MoveFileExW");
extern int __stdcall MultiByteToWideChar_(unsigned, unsigned long, const char*, int, wchar_t*, int) __asm__("MultiByteToWideChar");
// The link step wraps rename (-Wl,--wrap=rename) so it replaces an existing file like POSIX rename.
inline __attribute__((used)) int __wrap_rename(const char* from, const char* to) {
    if (MoveFileExA_(from, to, 1 /* MOVEFILE_REPLACE_EXISTING */ | 8 /* MOVEFILE_WRITE_THROUGH */)) return 0;
    errno = EACCES;
    return -1;
}
}
// rename with std::filesystem path arguments (wchar_t here), alone or with a char* name.
namespace frondWin {
struct Wide {
    wchar_t text[1024];
    Wide(const char* narrow) { text[0] = 0; MultiByteToWideChar_(0, 0, narrow, -1, text, 1024); }
    Wide(const wchar_t* wide) { int i = 0; for (; i < 1023 && wide[i]; ++i) text[i] = wide[i]; text[i] = 0; }
};
inline int renameWide(const Wide& from, const Wide& to) {
    if (MoveFileExW_(from.text, to.text, 1 | 8)) return 0;
    errno = EACCES;
    return -1;
}
}  // namespace frondWin
extern "C++" {
inline int rename(const wchar_t* from, const wchar_t* to) { return frondWin::renameWide(from, to); }
inline int rename(const char* from, const wchar_t* to) { return frondWin::renameWide(from, to); }
inline int rename(const wchar_t* from, const char* to) { return frondWin::renameWide(from, to); }
}
#endif
