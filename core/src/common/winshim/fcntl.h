#pragma once
#include_next <fcntl.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0  // a folder can't be opened here
#endif
#ifdef __cplusplus
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <wchar.h>
#include <io.h>
// Files open as binary. A std::filesystem path (wchar_t here) opens through CreateFileW with every
// share mode, so a file can be renamed or removed while another descriptor has it open, as on Linux.
namespace frondWin {
inline const int binaryMode = (_set_fmode(_O_BINARY), 0);
extern "C++" void* __stdcall createFile(const wchar_t*, unsigned long, unsigned long, void*, unsigned long, unsigned long,
                                        void*) __asm__("CreateFileW");
}
extern "C++" inline int open(const wchar_t* path, int flags, int mode = 0) {
    (void)mode;
    const int access = flags & (_O_RDONLY | _O_WRONLY | _O_RDWR);
    unsigned long rights = access == _O_RDONLY ? 0x80000000ul : access == _O_WRONLY ? 0x40000000ul : 0xC0000000ul;
    unsigned long how = 3;  // OPEN_EXISTING
    if ((flags & _O_CREAT) && (flags & _O_EXCL)) how = 1;
    else if ((flags & _O_CREAT) && (flags & _O_TRUNC)) how = 2;
    else if (flags & _O_CREAT) how = 4;
    else if (flags & _O_TRUNC) how = 5;
    void* handle = frondWin::createFile(path, rights, 7, nullptr, how, 0x80, nullptr);
    if (handle == reinterpret_cast<void*>(~uintptr_t(0))) {
        errno = (how == 1 || how == 2) ? EEXIST : ENOENT;
        return -1;
    }
    return _open_osfhandle(intptr_t(handle), (flags & _O_APPEND) | _O_BINARY | (access == _O_RDONLY ? _O_RDONLY : 0));
}
#endif
