#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef __cplusplus
extern "C++" {
#ifndef POLLIN  // the headers here don't declare WSAPoll's types
struct pollfd { SOCKET fd; short events; short revents; };
#define POLLERR 0x0001
#define POLLHUP 0x0002
#define POLLNVAL 0x0004
#define POLLWRNORM 0x0010
#define POLLOUT POLLWRNORM
#define POLLRDNORM 0x0100
#define POLLRDBAND 0x0200
#define POLLIN (POLLRDNORM | POLLRDBAND)
#define POLLPRI 0x0400
int __stdcall WSAPoll_(struct pollfd*, unsigned long, int) __asm__("WSAPoll");
inline int poll(struct pollfd* fds, unsigned long count, int timeout) { return WSAPoll_(fds, count, timeout); }
#else
inline int poll(WSAPOLLFD* fds, unsigned long count, int timeout) { return WSAPoll(fds, count, timeout); }
#endif
}
#endif
