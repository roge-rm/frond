#include "app_launcher.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "cputil.h"
#include "db_list.h"
#include "pace.h"
#include "palmos_errors.h"
#include "syscall_dispatch.h"

#define MAX_NAME_LEN 31
#define sysAppLaunchCmdNormalLaunch 0

bool launchAppByName(struct SyscallDispatch* sd, const char* name) {
    const size_t nameLen = strlen(name);
    if (nameLen > MAX_NAME_LEN) {
        fprintf(stderr, "failed to launch app: name length exceeds limit\n");
        return false;
    }

    if (!syscallDispatchM68kSupport(sd)) {
        fprintf(stderr, "failed to launch app: m68k syscalls not supported\n");
        return false;
    }

    if (!syscallDispatchPrepare(sd)) {
        fprintf(stderr, "failed to launch app: unable to prepare syscall\n");
        return false;
    }

    const uint32_t scratch = syscall68k_MemPtrNew(sd, SC_EXECUTE_PURE, MAX_NAME_LEN + 1);
    if (!scratch) {
        fprintf(stderr, "failed to launch app: unable to allocate scratch space\n");
        return false;
    }

    if (!syscallDispatch_memcpy_fromHost(sd, scratch, name, nameLen + 1)) {
        ERR("failed to copy application name to memory");
    }

    const uint32_t dbID = syscall68k_DmFindDatabase(sd, SC_EXECUTE_FULL, 0, scratch);
    bool result = false;

    if (!dbID) {
        fprintf(stderr, "failed to launch app: %s not found\n", name);
    } else {
        result = syscall68k_SysUIAppSwitch(sd, SC_EXECUTE_FULL, 0, dbID,
                                           sysAppLaunchCmdNormalLaunch, 0) == 0;
    }

    if (syscall68k_MemPtrFree(sd, SC_EXECUTE_PURE, scratch) != 0)
        ERR("failed to release scratch memory");

    return result;
}

DeleteDbResult deleteDatabaseByName(struct SyscallDispatch* sd, const char* name, bool* application) {
    if (application) *application = false;
    if (!syscallDispatchM68kSupport(sd) || !syscallDispatchPrepare(sd)) return DeleteDbResult::failed;
    const uint32_t scratch = syscall68k_MemPtrNew(sd, SC_EXECUTE_PURE, MAX_NAME_LEN + 1 + 8);
    if (!scratch) return DeleteDbResult::failed;
    DeleteDbResult result = DeleteDbResult::failed;
    if (strlen(name) > MAX_NAME_LEN || !syscallDispatch_memcpy_fromHost(sd, scratch + 8, name, strlen(name) + 1)) {
        syscall68k_MemPtrFree(sd, SC_EXECUTE_PURE, scratch);
        return result;
    }
    const uint32_t dbID = syscall68k_DmFindDatabase(sd, SC_EXECUTE_FULL, 0, scratch + 8);
    if (!dbID) {
        result = DeleteDbResult::missing;
    } else {
        bool rom = false;
        DbMetadataList all;
        if (dbListGet(sd, all))
            for (const auto& db : all) {
            if (db.localID != dbID) continue;
            rom = rom || db.isRom;
            if (application) *application = db.type == 0x6170706cu;
        }
        const bool running = syscall68k_SysCurAppDatabase(sd, SC_EXECUTE_FULL, scratch, scratch + 2) == 0 &&
                             paceGet32(scratch + 2) == dbID;
        if (rom) {
            result = DeleteDbResult::rom;
        } else if (running) {
            result = DeleteDbResult::running;
        } else {
            const uint16_t err = syscall68k_DmDeleteDatabase(sd, SC_EXECUTE_FULL, 0, dbID);
            result = err == 0 ? DeleteDbResult::deleted : err == dmErrDatabaseOpen ? DeleteDbResult::open : DeleteDbResult::failed;
        }
    }
    syscall68k_MemPtrFree(sd, SC_EXECUTE_PURE, scratch);
    return result;
}

bool relaunchLauncher(struct SyscallDispatch* sd) {
    if (!syscallDispatchM68kSupport(sd) || !syscallDispatchPrepare(sd)) return false;
    // Card +0, database +2, creator +6.
    const uint32_t scratch = syscall68k_MemPtrNew(sd, SC_EXECUTE_PURE, 16);
    if (!scratch) return false;
    bool ok = true;
    if (syscall68k_SysCurAppDatabase(sd, SC_EXECUTE_FULL, scratch, scratch + 2) == 0) {
        const uint32_t current = paceGet32(scratch + 2);
        if (current && syscall68k_DmDatabaseInfo(sd, SC_EXECUTE_FULL, paceGet16(scratch), current, 0, 0, 0, 0, 0, 0, 0,
                                                 0, 0, 0, scratch + 6) == 0 &&
            paceGet32(scratch + 6) == 0x6c6e6368u && !syscall68k_EvtEventAvail(sd, SC_EXECUTE_FULL) &&
            !syscall68k_EvtSysEventAvail(sd, SC_EXECUTE_FULL, true))
            ok = syscall68k_SysUIAppSwitch(sd, SC_EXECUTE_FULL, 0, current, 0, 0) == 0;
    }
    syscall68k_MemPtrFree(sd, SC_EXECUTE_PURE, scratch);
    return ok;
}
