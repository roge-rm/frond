#include "db_backup.h"
#include "db_archive_name.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include "Defer.h"
#include "cputil.h"
#include "pace.h"
#include "syscall_dispatch.h"
#include "zip.h"

using namespace std;

namespace {
    constexpr int COMPRESSION_LEVEL = 1;
    constexpr uint32_t MAX_CHUNK_SIZE = 64 * 1024;

    constexpr uint32_t DB_CREATOR_A68K = 0x6136386b;  // 'a68k'

    string getEntryName(DbMetadata& meta) {
        return dbArchiveName(meta.name, meta.attr, meta.creator, meta.isRom);
    }
}  // namespace

DbBackup::DbBackup(SyscallDispatch* syscallDispatch, int backupType, std::string databaseName)
    : sd(syscallDispatch), databaseName(std::move(databaseName)), backupType(backupType) {
    memset(lastProcessedDb, 0, sizeof(lastProcessedDb));
}

DbBackup::~DbBackup() {
    if (zip) zip_stream_close(zip);
    if (archiveData) free(archiveData);
}

bool DbBackup::Init() {
    if (state != STATE_CREATED) ERR("DB backup: already initialized");

    if (!dbListGet(sd, dbList)) {
        state = STATE_ERROR;
        return false;
    }

    if(!databaseName.empty() && std::none_of(dbList.begin(),dbList.end(),
        [&](const auto& db){return !db.isRom && databaseName==db.name;})){
        state=STATE_ERROR;return false;
    }
    dbIterator = dbList.begin();
    if (dbIterator == dbList.end()) {
        state = STATE_ERROR;
        cout << "DB backup: empty DB list" << endl;
        return false;
    }

    zip = zip_stream_open(nullptr, 0, COMPRESSION_LEVEL, 'w');
    if (!zip) {
        state = STATE_ERROR;
        return false;
    }
    chunkBuffer = make_unique<uint8_t[]>(MAX_CHUNK_SIZE);
    state = STATE_IN_PROGRESS;

    return true;
}

int DbBackup::GetState() const { return state; }

bool DbBackup::Continue() {
    lastProcessedDb[0] = '\0';
    if (state != STATE_IN_PROGRESS) ERR("DB backup: not in progress");

    while (dbIterator != dbList.end() && !Include(*dbIterator)) dbIterator++;
    if (dbIterator == dbList.end()) {
        state = STATE_DONE;
        return true;
    }

    strncpy(lastProcessedDb, dbIterator->name, sizeof(lastProcessedDb));

    if (!syscallDispatchM68kSupport(sd)) {
        cerr << "DB backup: m68k syscalls not supported";
        state = STATE_ERROR;
        return false;
    }

    if (!syscallDispatchPrepare(sd)) {
        cerr << "DB backup: failed to prepare safe environment for syscalls";
        state = STATE_ERROR;
        return false;
    }

    const string entryName = getEntryName(*dbIterator);
    if (zip_entry_open(zip, entryName.c_str()) != 0) {
        state = STATE_ERROR;
        return false;
    }
    Defer closeEntry([&]() {
        if (zip_entry_close(zip) != 0) state = STATE_ERROR;
    });

    const uint32_t scratch = syscall68k_MemPtrNew(sd, SC_EXECUTE_PURE, 3);
    if (!scratch) {
        state = STATE_ERROR;
        return false;
    }
    bool writeFailed = false;
    Defer freeScratch([=]() { syscall68k_MemPtrFree(sd, SC_EXECUTE_PURE, scratch); });

    const uint32_t writeProcP = scratch;
    const uint32_t nameP = scratch + 2;

    const uint8_t emptyName = 0;
    if (!syscallDispatch_memcpy_fromHost(sd, nameP, &emptyName, 1))
        throw std::runtime_error("could not initialise DB name");

    syscallDispatchRegisterM68kStub(
        sd, writeProcP,
        [&](struct ArmCpu*, uint32_t parameterBase, std::function<void()> deadMansSwitch) {
            deadMansSwitch();

            paceSetDreg(0, 1);

            uint32_t dataP = paceGet32(parameterBase);
            const uint32_t sizeP = paceGet32(parameterBase + 4);
            syscallDispatchValidateMemory(sd, sizeP, 4, false);
            uint32_t size = paceGet32(sizeP);

            syscallDispatchValidateMemory(sd, dataP, size, false);
            while (size > 0) {
                const uint32_t transferSize = min(size, MAX_CHUNK_SIZE);

                if (!syscallDispatch_memcpy_toHost(sd, chunkBuffer.get(), dataP, transferSize)) {
                    writeFailed = true;
                    cout << "DB backup: failed to copy chunk" << endl;
                    return;
                }

                if (zip_entry_write(zip, chunkBuffer.get(), transferSize) != 0) {
                    writeFailed = true;
                    cout << "DB backup: failed to write chunk" << endl;
                    return;
                }

                size -= transferSize;
                dataP += transferSize;
            }

            paceSetDreg(0, 0);
        }, 12);

    Defer unregisterStub([=]() { syscallDispatchUnregisterM68kStub(sd, writeProcP); });

    const uint16_t dbWriteResult = syscall68k_ExgDBWrite(sd, SC_EXECUTE_FULL, writeProcP, 0, nameP,
                                                         dbIterator->localID, dbIterator->cardNo);

    if (writeFailed || dbWriteResult != 0) {
        if(dbWriteResult) cerr << "DB backup: " << dbIterator->name << " failed with Palm error 0x"
             << std::hex << dbWriteResult << std::dec << endl;
        else cerr << "DB backup: " << dbIterator->name << " could not be copied to the archive" << endl;
        state = STATE_ERROR;
        return false;
    }
    state = ++dbIterator == dbList.end() ? STATE_DONE : STATE_IN_PROGRESS;
    return true;
}

bool DbBackup::HasLastProcessedDb() const { return lastProcessedDb[0] != '\0'; }

const char* DbBackup::GetLastProcessedDb() const { return lastProcessedDb; }

const void* DbBackup::GetArchiveData() {
    CopyArchiveData();
    return archiveData;
}

size_t DbBackup::GetArchiveSize() {
    CopyArchiveData();
    return archiveSize >= 0 ? archiveSize : 0;
}

bool DbBackup::Include(DbMetadata& db) {
    if(!databaseName.empty() && databaseName!=db.name)return false;
    switch (backupType) {
        case BACKUP_TYPE_RAM:
            return !db.isRom && db.creator != DB_CREATOR_A68K;

        case BACKUP_TYPE_RAM_ROM:
            return db.creator != DB_CREATOR_A68K;

        default:
            return true;
    }
}

void DbBackup::CopyArchiveData() {
    if (state != STATE_DONE) return;

    if (!archiveData) {
        if (zip_stream_copy(zip, &archiveData, &archiveSize) < 0) {
            state = STATE_ERROR;
            archiveSize = 0;
        }

        zip_stream_close(zip);
        zip = nullptr;
    }
}
