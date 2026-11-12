#ifndef FROND_GUEST_DATABASE_OPS_H
#define FROND_GUEST_DATABASE_OPS_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

#include "CardReinsert.h"
#include "GuestExport.h"
#include "NativeDatabase.h"
#include "app_launcher.h"
#include "db_backup.h"
#include "db_list.h"
#include "native_boundary.h"

// Shared desktop/console workflow. Native preparation makes no guest calls and no guest writes;
// all calls and allocations run under a recoverable whole-guest transaction. Host calls wake an idle
// UI task, once (Prepare on the T|X, else the 68k preparation); Ready is a look and never does.
namespace GuestDatabaseOps {
inline bool Ready(SoC& soc) {
    return NativeBoundary::Prepare(soc, false) || syscallDispatchPrepare(soc.GetSyscallDispatch(), SYSCALL_PREPARE_LOOK);
}

// After a failed NativeBoundary::Prepare: the T|X has already woken the UI task there.
inline SyscallPrepareMode FallbackMode(SoC& soc) {
    return soc.GetDeviceType() == DeviceType5::deviceTypeTX ? SYSCALL_PREPARE_AFTER_NATIVE : SYSCALL_PREPARE_WAKE;
}

// On a device with its store in internal NAND (NVFS: the T|X and E2), Palm OS writes a changed
// database's last pages from a background task that runs every 2 s, and a pin reset before then
// loses the change. HotSync's own reset goes through SysReset, and going to sleep, which writes
// them first. The guest time of the last install or delete, so a reset from the frontend can let
// that task run first.
struct StoreWrite {
    bool pending{false};
    uint64_t at{0};  // guest time, ns
};

inline StoreWrite& storeWrite() {
    static StoreWrite write;
    return write;
}

inline void noteStoreWrite(SoC& soc) {
    if (soc.GetNand()) storeWrite() = {true, soc.GetTime()};
}

// Before a pin reset: runs the guest until it has had delay of awake time since the last install
// or delete, or limit in all. Time since the change counts as awake even if the guest slept, as
// it wrote the store on the way to sleep. A guest that is asleep, or falls asleep, has written it,
// so the wait stops there. Cycles are counted at 100 MHz, like settleEject.
inline void SettleBeforeReset(SoC& soc, uint64_t delay = 3000000000, uint64_t limit = 10000000000) {
    auto& write = storeWrite();
    if (!write.pending) return;
    write.pending = false;
    const uint64_t start = soc.GetTime();
    if (start < write.at) return;  // a session loaded since
    constexpr uint64_t cyclesPerSecond = 100000000, slice = 2000000;  // 20 ms
    uint64_t awake = std::min(delay, start - write.at);
    while (awake < delay && soc.GetTime() - write.at < limit && !frondNative::guestAsleep(soc)) {
        const uint64_t before = soc.GetTime();
        soc.Run(slice, cyclesPerSecond);
        const uint64_t after = soc.GetTime();
        if (after == before) break;
        awake += after - before;
    }
}

inline int InstallNow(SoC& soc, size_t size, void* data) {
    if (!NativeBoundary::Prepare(soc)) {
        if (!syscallDispatchM68kSupport(soc.GetSyscallDispatch())) return DB_INSTALL_RESULT_ERR_NOT_SUPPORTED;
        if (!syscallDispatchPrepare(soc.GetSyscallDispatch(), FallbackMode(soc))) return DB_INSTALL_RESULT_ERR_NOT_CURRENTLY_POSSIBLE;
        {
            ReadOnlyGuest transaction(soc);
            const int result = dbInstallerInstall(soc.GetSyscallDispatch(), size, data);
            if (result < 0) return result;
            transaction.Commit();
            if (result != DB_INSTALL_RESULT_OK) return result;
        }
        // The sync notify and launcher relaunch run in their own transaction: if they fail, the
        // guest goes back to just after the install, which still succeeded.
        try {
            ReadOnlyGuest transaction(soc);
            dbInstallerFinish(soc.GetSyscallDispatch(), size, data);
            transaction.Commit();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "install: sync notify or launcher relaunch failed, the database is installed: %s\n", error.what());
        }
        return DB_INSTALL_RESULT_OK;
    }
    {
        ReadOnlyGuest transaction(soc);
        NativeBoundary::Session session(soc);
        NativeDatabase<NativeBoundary::Session> database(session);
        const int result = database.Install(static_cast<uint8_t*>(data), size);
        if (result < 0) return result;
        database.Finish();
        session.Close();
        transaction.Commit();
        if (result != DB_INSTALL_RESULT_OK) return result;
    }
    try {
        ReadOnlyGuest transaction(soc);
        NativeBoundary::Session session(soc);
        NativeDatabase<NativeBoundary::Session> database(session);
        database.FinishInstall(static_cast<const uint8_t*>(data), size);
        database.Finish();
        session.Close();
        transaction.Commit();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "install: sync notify or launcher relaunch failed, the database is installed: %s\n", error.what());
    }
    return DB_INSTALL_RESULT_OK;
}

inline int Install(SoC& soc, size_t size, void* data) {
    const int result = InstallNow(soc, size, data);
    if (result >= 0) noteStoreWrite(soc);
    return result;
}

enum class LaunchResult { notReady, failed, launched };
inline LaunchResult Launch(SoC& soc, const std::string& name) {
    if (!NativeBoundary::Prepare(soc)) {
        if (!syscallDispatchPrepare(soc.GetSyscallDispatch(), FallbackMode(soc))) return LaunchResult::notReady;
        ReadOnlyGuest transaction(soc);
        if (!launchAppByName(soc.GetSyscallDispatch(), name.c_str())) return LaunchResult::failed;
        transaction.Commit();
        return LaunchResult::launched;
    }
    ReadOnlyGuest transaction(soc);
    NativeBoundary::Session session(soc);
    NativeDatabase<NativeBoundary::Session> database(session);
    if (!database.Launch(name)) return LaunchResult::failed;
    database.Finish();
    session.Close();
    transaction.Commit();
    return LaunchResult::launched;
}

// Deleting commits like an install. Returns nullopt when no supported boundary is available. Deleting
// an application then relaunches the launcher in its own transaction: if that fails, the guest goes
// back to just after the delete, which still succeeded.
inline std::optional<DeleteDbResult> DeleteNow(SoC& soc, const std::string& name) {
    bool application = false;
    DeleteDbResult result;
    if (!NativeBoundary::Prepare(soc)) {
        if (!syscallDispatchPrepare(soc.GetSyscallDispatch(), FallbackMode(soc))) return std::nullopt;
        {
            ReadOnlyGuest transaction(soc);
            result = deleteDatabaseByName(soc.GetSyscallDispatch(), name.c_str(), &application);
            if (result == DeleteDbResult::deleted) transaction.Commit();
        }
        if (result == DeleteDbResult::deleted && application) {
            try {
                ReadOnlyGuest transaction(soc);
                relaunchLauncher(soc.GetSyscallDispatch());
                transaction.Commit();
            } catch (const std::exception& error) {
                std::fprintf(stderr, "delete: launcher relaunch failed, the database is deleted: %s\n", error.what());
            }
        }
        return result;
    }
    {
        ReadOnlyGuest transaction(soc);
        NativeBoundary::Session session(soc);
        NativeDatabase<NativeBoundary::Session> database(session);
        result = database.Delete(name, &application);
        database.Finish();
        session.Close();
        if (result == DeleteDbResult::deleted) transaction.Commit();
    }
    if (result == DeleteDbResult::deleted && application) {
        try {
            ReadOnlyGuest transaction(soc);
            NativeBoundary::Session session(soc);
            NativeDatabase<NativeBoundary::Session> database(session);
            database.RelaunchLauncher();
            database.Finish();
            session.Close();
            transaction.Commit();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "delete: launcher relaunch failed, the database is deleted: %s\n", error.what());
        }
    }
    return result;
}

inline std::optional<DeleteDbResult> Delete(SoC& soc, const std::string& name) {
    const auto result = DeleteNow(soc, name);
    if (result == DeleteDbResult::deleted) noteStoreWrite(soc);
    return result;
}

// The RAM databases' directory entries, without serialising any database. Read-only: the guest
// is rolled back afterwards like Export. Returns false only when no supported boundary is available.
inline bool List(SoC& soc, DbMetadataList& list) {
    if (NativeBoundary::Prepare(soc)) {
        ReadOnlyGuest transaction(soc);
        NativeBoundary::Session session(soc);
        NativeDatabase<NativeBoundary::Session> database(session);
        auto found = database.List();
        database.Finish();
        session.Close();
        list = std::move(found);
        return true;
    }
    GuestExport guest(soc, FallbackMode(soc));
    if (!guest.Ready()) return false;
    DbMetadataList all;
    if (!dbListGet(soc.GetSyscallDispatch(), all)) throw std::runtime_error("Could not read the Palm database list");
    guest.Finish();
    list.clear();
    for (const auto& db : all)
        if (!db.isRom && db.creator != 0x6136386bu) list.push_back(db);
    return true;
}

// Returns false only when no supported boundary is available. Errors throw;
// the caller publishes output only after this function's rollback completes.
inline bool Export(SoC& soc, const std::string& name, std::vector<uint8_t>& bytes,
                   int backupType = DbBackup::BACKUP_TYPE_RAM) {
    if (backupType == DbBackup::BACKUP_TYPE_RAM && NativeBoundary::Prepare(soc)) {
        ReadOnlyGuest transaction(soc);
        NativeBoundary::Session session(soc);
        NativeDatabase<NativeBoundary::Session> database(session);
        auto archive = database.Export(name);
        database.Finish();
        session.Close();
        bytes = std::move(archive);
        return true;
    }
    GuestExport guest(soc, backupType == DbBackup::BACKUP_TYPE_RAM ? FallbackMode(soc) : SYSCALL_PREPARE_WAKE);
    if (!guest.Ready()) return false;
    DbBackup backup(soc.GetSyscallDispatch(), backupType, name);
    if (!backup.Init()) throw std::runtime_error("Could not start database export");
    while (backup.GetState() == DbBackup::STATE_IN_PROGRESS) backup.Continue();
    if (backup.GetState() != DbBackup::STATE_DONE || !backup.GetArchiveData() || !backup.GetArchiveSize())
        throw std::runtime_error("Database export failed; no archive was written");
    const auto* data = static_cast<const uint8_t*>(backup.GetArchiveData());
    std::vector<uint8_t> result(data, data + backup.GetArchiveSize());
    guest.Finish();
    bytes = std::move(result);
    return true;
}
}
#endif
