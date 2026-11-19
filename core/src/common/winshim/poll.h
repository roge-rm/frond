#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef __cplusplus
inline int poll(WSAPOLLFD* fds, unsigned long count, int timeout) { return WSAPoll(fds, count, timeout); }
#endif
