#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef __cplusplus
inline int poll(WSAPOLLFD* fds, unsigned long count, int timeout) { return WSAPoll(fds, count, timeout); }
#endif
