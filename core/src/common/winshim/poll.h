#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#define poll(fds, count, timeout) WSAPoll((fds), (count), (timeout))
