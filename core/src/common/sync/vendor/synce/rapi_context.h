#ifndef FROND_SYNCE_CONTEXT_H
#define FROND_SYNCE_CONTEXT_H
#include "rapi_buffer.h"

typedef struct {
    RapiBuffer* send_buffer;
    RapiBuffer* recv_buffer;
    SynceSocket* socket;
    uint32_t last_error;
} RapiContext;

bool rapi_context_begin_command(RapiContext* context, uint32_t command);
bool rapi2_context_call(RapiContext* context);

#endif
