#pragma once

#include <cstddef>
#include <windows.h>

namespace frondConduit {

#pragma pack(push, 1)
struct SyncProperties {
    DWORD type;
    char userPath[256];
    char localName[256];
    char user[256];
    char* remoteNames[32];
    void** remoteDbList;
    int remoteCount;
    DWORD creator;
    WORD card;
    DWORD dbType, appInfoSize, sortInfoSize, firstDevice, connection;
    char registryPath[256];
    HKEY registryRoot;
    DWORD reserved;
};

struct DbBlock {
    char name[32];
    WORD size, bytesRead;
    BYTE* data;
    DWORD reserved;
};

struct Record {
    BYTE handle;
    DWORD id;
    WORD index;
    BYTE attributes;
    short category;
    int conduit;
    DWORD size;
    WORD bufferSize;
    BYTE* data;
    DWORD reserved;
};

struct Resource {
    BYTE handle;
    DWORD type;
    WORD id;
    BYTE unused[7];
    WORD size;
    DWORD unusedSize;
    BYTE* data;
    DWORD reserved;
};

struct Preference {
    WORD version;
    DWORD creator;
    WORD id, requested, returned, actual;
    BOOL saved;
    long bufferSize;
    BYTE* data;
    DWORD reserved;
};

struct CreateDb {
    BYTE handle;
    DWORD creator, flags;
    BYTE card;
    char name[32];
    DWORD type;
    WORD version;
    DWORD reserved;
};

struct DbList {
    int card;
    WORD flags;
    DWORD type;
    char name[32];
    DWORD creator;
    WORD version;
    DWORD modNumber;
    WORD index;
    long created, modified, backedUp;
    DWORD misc, count, reserved;
};

struct DbInfo {
    DbList base;
    DWORD records, totalBytes, dataBytes, appBlockSize, sortBlockSize, maxRecordSize;
    DWORD reserved, localId, openRef;
};

struct FindName {
    BYTE options;
    DWORD card;
    char* name;
};

struct FindType {
    BYTE options, search;
    DWORD type, creator;
};

struct User {
    char name[64];
    int nameLength;
    char password[64];
    int passwordLength;
    long lastSync;
    DWORD lastPc, id, viewerId, reserved;
};

struct CardInfo {
    BYTE card;
    WORD version;
    long created;
    DWORD romSize, ramSize, freeRam;
    BYTE nameLength, manufacturerLength;
    char name[32], manufacturer[32];
    WORD romDbs, ramDbs;
    DWORD reserved;
};
#pragma pack(pop)

static_assert(sizeof(void*) == 4, "Palm conduits use the 32-bit ABI");
static_assert(sizeof(SyncProperties) == 1198, "Unexpected Palm sync properties layout");
static_assert(sizeof(Record) == 28 && sizeof(Preference) == 30 && sizeof(CreateDb) == 52);
static_assert(sizeof(Resource) == 28 && offsetof(Resource, size) == 14 && offsetof(Resource, data) == 20);
static_assert(sizeof(DbBlock) == 44 && sizeof(DbList) == 78 && sizeof(User) == 156);
static_assert(sizeof(DbInfo) == 114 && sizeof(CardInfo) == 93);

using Progress = long (*)(char*);
using OpenConduit = long (*)(Progress, SyncProperties&);

}  // namespace frondConduit
