// Compile the SynCE RAPI 2 file calls and buffer with a Frond byte stream.
#include "SynceRapi.h"

#define HAVE_CONFIG_H 1
#include "vendor/synce/rapi_buffer.c"
#include "vendor/synce/file_calls.c"

struct _SynceSocket {
    FrondRapiStream stream;
    bool closed;
};

struct FrondRapi {
    RapiContext rapi;
    SynceSocket socket;
};

bool synce_socket_read(SynceSocket* socket, void* data, size_t size) {
    if (socket->closed) return false;
    if (socket->stream.read(socket->stream.context, data, size, 15000)) return true;
    synce_socket_close(socket);
    return false;
}

bool synce_socket_write(SynceSocket* socket, const void* data, size_t size) {
    if (socket->closed) return false;
    if (socket->stream.write(socket->stream.context, data, size)) return true;
    synce_socket_close(socket);
    return false;
}

bool synce_socket_wait(SynceSocket* socket, int seconds, short* events) {
    (void)seconds;  // Read waits with a deadline.
    *events = socket->closed ? 0 : EVENT_READ;
    return !socket->closed;
}

bool synce_socket_close(SynceSocket* socket) {
    if (!socket->closed) {
        socket->closed = true;
        socket->stream.close(socket->stream.context);
    }
    return true;
}

size_t wstrlen(LPCWSTR s) {
    size_t size = 0;
    while (s[size]) ++size;
    return size;
}

void _synce_log(int level, const char* file, int line, const char* format, ...) {
    (void)level;
    (void)file;
    (void)line;
    (void)format;
}

bool rapi_context_begin_command(RapiContext* context, uint32_t command) {
    context->last_error = 0;
    rapi_buffer_free_data(context->send_buffer);
    return rapi_buffer_write_uint32(context->send_buffer, command);
}

bool rapi2_context_call(RapiContext* context) {
    if (rapi_buffer_send(context->send_buffer, context->socket) &&
        rapi_buffer_recv(context->recv_buffer, context->socket)) return true;
    synce_socket_close(context->socket);
    return false;
}

FrondRapi* FrondRapiOpen(FrondRapiStream stream) {
    FrondRapi* client = calloc(1, sizeof(*client));
    if (!client) return NULL;
    client->socket.stream = stream;
    client->rapi.socket = &client->socket;
    client->rapi.send_buffer = rapi_buffer_new();
    client->rapi.recv_buffer = rapi_buffer_new();
    if (client->rapi.send_buffer && client->rapi.recv_buffer) return client;
    FrondRapiClose(client);
    return NULL;
}

void FrondRapiClose(FrondRapi* client) {
    if (!client) return;
    rapi_buffer_free(client->rapi.send_buffer);
    rapi_buffer_free(client->rapi.recv_buffer);
    synce_socket_close(&client->socket);
    free(client);
}

bool FrondRapiReady(FrondRapi* client) {
    RapiContext* rapi = &client->rapi;
    // CeGetVersionEx; the trailing version structure is device-sized.
    if (!rapi_context_begin_command(rapi, 0x43) || !rapi2_context_call(rapi)) return false;
    uint32_t result = 0;
    return rapi_buffer_read_uint32(rapi->recv_buffer, &rapi->last_error) &&
           rapi_buffer_read_uint32(rapi->recv_buffer, &result) && result;
}

bool FrondRapiConnected(const FrondRapi* client) { return !client->socket.closed; }

uint32_t FrondRapiError(FrondRapi* client) { return client->rapi.last_error; }

bool FrondRapiProvision(FrondRapi* client, const uint16_t* config, uint16_t* reply, size_t capacity) {
    RapiContext* rapi = &client->rapi;
    // CeProcessConfig with CONFIG_PROCESS.
    if (!rapi_context_begin_command(rapi, 0x0e) ||
        !rapi2_buffer_write_string(rapi->send_buffer, config) ||
        !rapi_buffer_write_uint32(rapi->send_buffer, 1) || !rapi2_context_call(rapi)) return false;
    uint32_t status, bytes;
    if (!rapi_buffer_read_uint32(rapi->recv_buffer, &rapi->last_error) ||
        !rapi_buffer_read_uint32(rapi->recv_buffer, &status) || (status & 0x80000000u) ||
        !rapi_buffer_read_uint32(rapi->recv_buffer, &bytes) ||
        bytes % sizeof(*reply) || bytes / sizeof(*reply) >= capacity ||
        !rapi_buffer_read_data(rapi->recv_buffer, reply, bytes)) return false;
    reply[bytes / sizeof(*reply)] = 0;
    return true;
}


uint32_t FrondRapiOpenFile(FrondRapi* client, const uint16_t* path) {
    return _CeCreateFile2(&client->rapi, path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, 0);
}

bool FrondRapiRead(FrondRapi* client, uint32_t handle, void* data, uint32_t capacity, uint32_t* count) {
    RapiContext* rapi = &client->rapi;
    uint32_t ok;
    return rapi_context_begin_command(rapi, 0x17) &&
           rapi_buffer_write_uint32(rapi->send_buffer, handle) &&
           rapi_buffer_write_uint32(rapi->send_buffer, capacity) && rapi2_context_call(rapi) &&
           rapi_buffer_read_uint32(rapi->recv_buffer, &rapi->last_error) &&
           rapi_buffer_read_uint32(rapi->recv_buffer, &ok) && ok &&
           rapi_buffer_read_uint32(rapi->recv_buffer, count) && *count <= capacity &&
           rapi_buffer_read_data(rapi->recv_buffer, data, *count);
}

static bool FileCommand(FrondRapi* client, uint32_t opcode, const uint16_t* source, const uint16_t* destination) {
    RapiContext* rapi = &client->rapi;
    uint32_t ok;
    return rapi_context_begin_command(rapi, opcode) && rapi2_buffer_write_string(rapi->send_buffer, source) &&
           (!destination || rapi2_buffer_write_string(rapi->send_buffer, destination)) && rapi2_context_call(rapi) &&
           rapi_buffer_read_uint32(rapi->recv_buffer, &rapi->last_error) &&
           rapi_buffer_read_uint32(rapi->recv_buffer, &ok) && ok;
}

bool FrondRapiMakeDirectory(FrondRapi* client, const uint16_t* path) {
    return FileCommand(client, 0x28, path, NULL) || client->rapi.last_error == ERROR_ALREADY_EXISTS;
}

bool FrondRapiMove(FrondRapi* client, const uint16_t* source, const uint16_t* destination) {
    return FileCommand(client, 0x2b, source, destination);
}

bool FrondRapiList(FrondRapi* client, const uint16_t* pattern, FrondRapiEntry entry, void* context) {
    RapiContext* rapi = &client->rapi;
    // CeFindAllFiles requests the name, attributes and full size.
    const uint32_t flags = 0xb1;
    uint32_t status, count;
    if (!rapi_context_begin_command(rapi, 0x1a) ||
        !rapi2_buffer_write_string(rapi->send_buffer, pattern) ||
        !rapi_buffer_write_uint32(rapi->send_buffer, flags) || !rapi2_context_call(rapi) ||
        !rapi_buffer_read_uint32(rapi->recv_buffer, &rapi->last_error) ||
        !rapi_buffer_read_uint32(rapi->recv_buffer, &status) ||
        !rapi_buffer_read_uint32(rapi->recv_buffer, &count) || count > 1024)
        return false;
    if (rapi->last_error) return rapi->last_error == ERROR_FILE_NOT_FOUND && !count;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t length, attributes, high, low;
        uint16_t name[260];
        if (!rapi_buffer_read_uint32(rapi->recv_buffer, &length) || length < 2 ||
            length > sizeof(name) || length % 2 ||
            !rapi_buffer_read_uint32(rapi->recv_buffer, &attributes) ||
            !rapi_buffer_read_uint32(rapi->recv_buffer, &high) ||
            !rapi_buffer_read_uint32(rapi->recv_buffer, &low))
            return false;
        for (size_t j = 0; j < length / 2; ++j) {
            uint8_t word[2];
            if (!rapi_buffer_read_data(rapi->recv_buffer, word, sizeof(word))) return false;
            name[j] = (uint16_t)word[0] | ((uint16_t)word[1] << 8);
        }
        if (name[length / 2 - 1] != 0 || !entry(context, name, length / 2 - 1, attributes,
                                               ((uint64_t)high << 32) | low))
            return false;
    }
    return true;
}

uint32_t FrondRapiCreate(FrondRapi* client, const uint16_t* path) {
    return _CeCreateFile2(&client->rapi, path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0);
}

bool FrondRapiWrite(FrondRapi* client, uint32_t handle, const void* data, uint32_t size) {
    uint32_t written = 0;
    return _CeWriteFile2(&client->rapi, handle, data, size, &written, NULL) && written == size;
}

bool FrondRapiCloseFile(FrondRapi* client, uint32_t handle) {
    return _CeCloseHandle2(&client->rapi, handle);
}

bool FrondRapiRemove(FrondRapi* client, const uint16_t* path) {
    return _CeDeleteFile2(&client->rapi, path);
}

bool FrondRapiRun(FrondRapi* client, const uint16_t* path, const uint16_t* args) {
    PROCESS_INFORMATION info = {0};
    if (!_CeCreateProcess2(&client->rapi, path, args, NULL, NULL, false, 0, NULL, NULL, NULL, &info))
        return false;
    if (info.hThread) _CeCloseHandle2(&client->rapi, info.hThread);
    if (info.hProcess) _CeCloseHandle2(&client->rapi, info.hProcess);
    return true;
}
