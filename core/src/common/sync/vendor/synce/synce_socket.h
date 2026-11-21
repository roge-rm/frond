#ifndef FROND_SYNCE_SOCKET_H
#define FROND_SYNCE_SOCKET_H

#include "synce.h"

typedef struct _SynceSocket SynceSocket;
enum { EVENT_READ = 2 };
bool synce_socket_read(SynceSocket* socket, void* data, size_t size);
bool synce_socket_write(SynceSocket* socket, const void* data, size_t size);
bool synce_socket_wait(SynceSocket* socket, int seconds, short* events);
bool synce_socket_close(SynceSocket* socket);

#endif
