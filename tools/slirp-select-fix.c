/* LD_PRELOAD shim for Debian's slirp 1.0.17 on 64-bit hosts: with nothing pending it asks select()
   for a tv_usec of (u_int)-1, meaning no timeout, which glibc and Linux refuse with EINVAL and
   slirp then exits. Such a timeout becomes a wait with no limit.
   Build: cc -shared -fPIC -O2 -o slirp-select-fix.so slirp-select-fix.c -ldl */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/select.h>

int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, struct timeval* timeout) {
    static int (*real)(int, fd_set*, fd_set*, fd_set*, struct timeval*);
    if (!real) real = (int (*)(int, fd_set*, fd_set*, fd_set*, struct timeval*))dlsym(RTLD_NEXT, "select");
    if (timeout && (timeout->tv_usec < 0 || timeout->tv_usec >= 1000000)) {
        if (timeout->tv_sec == 0 && timeout->tv_usec == 0xffffffffL) timeout = 0;
        else {
            timeout->tv_sec += timeout->tv_usec / 1000000;
            timeout->tv_usec %= 1000000;
            if (timeout->tv_usec < 0) timeout->tv_usec = 0;
        }
    }
    return real(nfds, readfds, writefds, exceptfds, timeout);
}
