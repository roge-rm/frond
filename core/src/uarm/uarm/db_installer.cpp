#include "db_installer.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>

#include "CPU.h"
#include "Defer.h"
#include "cputil.h"
#include "pace.h"
#include "palmos_errors.h"
#include "patch_dispatch.h"
#include "syscall_dispatch.h"

using namespace std;

int32_t dbInstallerInstall(struct SyscallDispatch* sd, size_t len, void* data) {
    if (!syscallDispatchM68kSupport(sd)) return DB_INSTALL_RESULT_ERR_NOT_SUPPORTED;
    if (!syscallDispatchPrepare(sd)) return DB_INSTALL_RESULT_ERR_NOT_CURRENTLY_POSSIBLE;

    const uint32_t scratch = syscall68k_MemPtrNew(sd, SC_EXECUTE_PURE, 10);
    if (!scratch) return DB_INSTALL_RESULT_ERR_UNKNOWN;

    const uint32_t deleteProcP = scratch;
    const uint32_t readProcP = scratch + 2;
    const uint32_t needsResetP = scratch + 4;
    const uint32_t dbIDP = scratch + 6;
    size_t bytesRead = 0;
    bool couldNotOverwrite = false;

    syscallDispatchRegisterM68kStub(
        sd, deleteProcP,
        [&](struct ArmCpu*, uint32_t parameterBase, std::function<void()> deadMansSwitch) {
            deadMansSwitch();
            paceSetDreg(0, 0);

            const uint16_t cardNo = paceGet16(parameterBase + 6);
            const uint32_t dbID = paceGet32(parameterBase + 8);

            const uint16_t err = syscall68k_DmDeleteDatabase(sd, SC_EXECUTE_FULL, cardNo, dbID);
            deadMansSwitch();

            couldNotOverwrite = couldNotOverwrite || err;

            paceSetDreg(0, !err);
        }, 12);

    syscallDispatchRegisterM68kStub(
        sd, readProcP,
        [&](struct ArmCpu* cpu, uint32_t parameterBase, std::function<void()> deadMansSwitch) {
            deadMansSwitch();

            paceSetDreg(0, -1);

            const uint32_t dataP = paceGet32(parameterBase);
            const uint32_t sizeP = paceGet32(parameterBase + 4);
            syscallDispatchValidateMemory(sd, sizeP, 4);
            uint32_t size = paceGet32(sizeP);
            if (bytesRead > len) throw std::runtime_error("database callback exceeded input");

            if (size > len - bytesRead) size = len - bytesRead;

            if (!syscallDispatch_memcpy_fromHost(
                    sd, dataP, reinterpret_cast<uint8_t*>(data) + bytesRead, size)) {
                throw std::runtime_error("memory error copying DB data");
            }

            const uint8_t sizeBytes[]{uint8_t(size >> 24), uint8_t(size >> 16), uint8_t(size >> 8), uint8_t(size)};
            if (!syscallDispatch_memcpy_fromHost(sd, sizeP, sizeBytes, sizeof(sizeBytes)))
                throw std::runtime_error("memory error returning DB read size");

            bytesRead += size;
            paceSetDreg(0, 0);
        }, 12);

    Defer unregisterStubs([=]() {
        syscallDispatchUnregisterM68kStub(sd, readProcP);
        syscallDispatchUnregisterM68kStub(sd, deleteProcP);
    });

    uint16_t err = syscall68k_ExgDBRead(sd, SC_EXECUTE_FULL, readProcP, deleteProcP, 0, dbIDP, 0,
                                        needsResetP, true);
    bool needsReset = paceGet8(needsResetP);

    syscallDispatchUnregisterM68kStub(sd, readProcP);
    syscallDispatchUnregisterM68kStub(sd, deleteProcP);
    if (syscall68k_MemPtrFree(sd, SC_EXECUTE_PURE, scratch) != 0)
        throw std::runtime_error("failed to release scratch memory");

    if (couldNotOverwrite) return DB_INSTALL_RESULT_ERR_COULD_NOT_OVERWRITE;

    switch (err) {
        case 0:
            return needsReset ? DB_INSTALL_RESULT_NEEDS_RESET : DB_INSTALL_RESULT_OK;

        case dmErrMemError:
            return DB_INSTALL_RESULT_ERR_OOM;

        case dmErrCorruptDatabase:
            return DB_INSTALL_RESULT_ERR_CORRUPT;

        case dmErrDatabaseOpen:
            return DB_INSTALL_RESULT_ERR_IS_OPEN;

        default:
            return DB_INSTALL_RESULT_ERR_UNKNOWN;
    }
}

#define sysFileTApplication 0x6170706cu
#define sysFileCLauncher 0x6c6e6368u
#define sysAppLaunchCmdSyncNotify 3

static uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void dbInstallerFinish(struct SyscallDispatch* sd, size_t len, const void* data) {
    if (len < 78 || !syscallDispatchPrepare(sd)) return;
    const auto* header = static_cast<const uint8_t*>(data);
    char name[32]{};
    memcpy(name, header, 31);
    const uint32_t type = be32(header + 60), creator = be32(header + 64);

    // Name +0, card +32, database +34, type +38, creator +42, search state +48, launch result +80.
    const uint32_t scratch = syscall68k_MemPtrNew(sd, SC_EXECUTE_PURE, 96);
    if (!scratch) throw std::runtime_error("no scratch memory");
    Defer release([=]() { syscall68k_MemPtrFree(sd, SC_EXECUTE_PURE, scratch); });
    if (!syscallDispatch_memcpy_fromHost(sd, scratch, name, strlen(name) + 1))
        throw std::runtime_error("memory error copying the database name");

    const uint32_t installed = syscall68k_DmFindDatabase(sd, SC_EXECUTE_FULL, 0, scratch);
    if (!installed) return;

    uint32_t current = 0, currentCreator = 0;
    if (syscall68k_SysCurAppDatabase(sd, SC_EXECUTE_FULL, scratch + 32, scratch + 34) == 0) {
        current = paceGet32(scratch + 34);
        if (current && syscall68k_DmDatabaseInfo(sd, SC_EXECUTE_FULL, paceGet16(scratch + 32), current, 0, 0, 0,
                                                 0, 0, 0, 0, 0, 0, scratch + 38, scratch + 42) == 0)
            currentCreator = paceGet32(scratch + 42);
    }

    // HotSync sends the notify once no app of the user's is running, so the app in front never
    // gets it while its own instance has its databases open.
    uint32_t app = 0;
    if (type == sysFileTApplication)
        app = installed;
    else if (syscall68k_DmGetNextDatabaseByTypeCreator(sd, SC_EXECUTE_FULL, true, scratch + 48, sysFileTApplication,
                                                       creator, true, scratch + 32, scratch + 34) == 0)
        app = paceGet32(scratch + 34);
    if (app && app != current) {
        const uint16_t err =
            syscall68k_SysAppLaunch(sd, SC_EXECUTE_FULL, 0, app, 0, sysAppLaunchCmdSyncNotify, 0, scratch + 80);
        if (err) fprintf(stderr, "install: sync notify failed: Palm OS error %u\n", err);
    }

    // The relaunch goes through SysUIAppSwitch like the launch command. Any event already waiting,
    // a tap or the appStopEvent of a switch under way, goes first and the launcher keeps its list.
    if (type != sysFileTApplication || currentCreator != sysFileCLauncher ||
        syscall68k_EvtEventAvail(sd, SC_EXECUTE_FULL) || syscall68k_EvtSysEventAvail(sd, SC_EXECUTE_FULL, true))
        return;
    if (syscall68k_SysUIAppSwitch(sd, SC_EXECUTE_FULL, 0, current, 0, 0) != 0)
        throw std::runtime_error("could not relaunch the launcher");
}
