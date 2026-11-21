#ifndef FROND_SYNCE_RAPI_H
#define FROND_SYNCE_RAPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FrondRapi FrondRapi;

typedef struct {
    void* context;
    // Reads exactly size bytes within the timeout. Closes on failure.
    bool (*read)(void*, void* data, size_t size, int timeoutMs);
    bool (*write)(void*, const void* data, size_t size);
    void (*close)(void*);
} FrondRapiStream;

FrondRapi* FrondRapiOpen(FrondRapiStream stream);
void FrondRapiClose(FrondRapi* client);
bool FrondRapiReady(FrondRapi* client);
bool FrondRapiConnected(const FrondRapi* client);
uint32_t FrondRapiError(FrondRapi* client);
bool FrondRapiProvision(FrondRapi* client, const uint16_t* config, uint16_t* reply, size_t capacity);
uint32_t FrondRapiOpenFile(FrondRapi* client, const uint16_t* path);
bool FrondRapiRead(FrondRapi* client, uint32_t handle, void* data, uint32_t capacity, uint32_t* count);
bool FrondRapiMakeDirectory(FrondRapi* client, const uint16_t* path);
bool FrondRapiMove(FrondRapi* client, const uint16_t* source, const uint16_t* destination);
typedef bool (*FrondRapiEntry)(void* context, const uint16_t* name, size_t length, uint32_t attributes, uint64_t size);
bool FrondRapiList(FrondRapi* client, const uint16_t* pattern, FrondRapiEntry entry, void* context);
uint32_t FrondRapiCreate(FrondRapi* client, const uint16_t* path);
bool FrondRapiWrite(FrondRapi* client, uint32_t handle, const void* data, uint32_t size);
bool FrondRapiCloseFile(FrondRapi* client, uint32_t handle);
bool FrondRapiRemove(FrondRapi* client, const uint16_t* path);
bool FrondRapiRun(FrondRapi* client, const uint16_t* path, const uint16_t* args);

#ifdef __cplusplus
}
#endif

#endif
