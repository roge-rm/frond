#pragma once
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
// Shared mappings (the Android camera frames file) aren't ported: mmap fails, so that source is off.
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_FAILED ((void*)-1)
static inline void* mmap(void*, size_t, int, int, int, off_t) { errno = ENOSYS; return MAP_FAILED; }
static inline int munmap(void*, size_t) { return 0; }
