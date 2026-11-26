#ifndef FROND_GUEST_DATABASE_OPS_H
#define FROND_GUEST_DATABASE_OPS_H

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
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
#include "zodiac_native.h"
#include <type_traits>

// Shared desktop/console workflow. Native preparation makes no guest calls and no guest writes;
// all calls and allocations run under a recoverable whole-guest transaction. Host calls wake a
// sleeping Palm (Wake) and an idle UI task, once (NativePrepare on the T|X and Zodiac, else the 68k preparation);
// Ready is a look and never does either.
namespace GuestDatabaseOps {
// The native boundary: the Zodiac's (ZodiacNative) or the T|X's (NativeBoundary).
inline bool NativePrepare(SoC& soc, bool wake = true) {
    return ZodiacNative::Applies(soc) ? ZodiacNative::Prepare(soc, wake) : NativeBoundary::Prepare(soc, wake);
}

// Runs f with the device's native session.
template <class F>
auto WithSession(SoC& soc, F&& f) {
    if (ZodiacNative::Applies(soc)) {
        ZodiacNative::Session session(soc);
        return f(session);
    }
    NativeBoundary::Session session(soc);
    return f(session);
}
template <class S>
using DatabaseOf = NativeDatabase<std::decay_t<S>>;

inline bool Ready(SoC& soc) {
    return NativePrepare(soc, false) || syscallDispatchPrepare(soc.GetSyscallDispatch(), SYSCALL_PREPARE_LOOK);
}

// Host calls the user starts (install, launch, delete, export, card files) wake a sleeping Palm
// first with a power button press, as a person would; lists and other looks don't. No press is
// added while one from the host is held or still queued, as the two would put it back to sleep. The
// press and release go through the key queue like the frontend's (the PXA holds a short press for
// 250 ms so the DAL stays up), then the guest runs until it's awake, past that hold and at a point
// that takes host calls, or awake for 3 s (a 68k app waiting with no timeout never shows Ready),
// kWakeSeconds of guest time at most. The T|X ignores power presses for about 2.6 s after any wake,
// a power-key one too, so a press right after the call can be lost. False when it's still asleep.
// Cycles are counted at 100 MHz.
inline bool Wake(SoC& soc) {
    if (!frondNative::guestAsleep(soc)) return true;
    if (!soc.PowerButtonSupported()) return false;
    if (!soc.PowerKeyPending()) {
        soc.KeyDown(keyIdPower);
        soc.KeyUp(keyIdPower);
    }
    constexpr uint64_t cyclesPerSecond = 100000000, slice = 2000000;  // 20 ms
    constexpr uint64_t kWakeSeconds = 5000000000, hold = 300000000, settle = 3000000000;  // ns
    const uint64_t start = soc.GetTime();
    uint64_t awake = 0;
    while (soc.GetTime() - start < kWakeSeconds) {
        if (awake >= settle || (awake && soc.GetTime() - start >= hold && Ready(soc))) break;
        const uint64_t before = soc.GetTime();
        soc.Run(slice, cyclesPerSecond);
        const uint64_t after = soc.GetTime();
        if (after == before) break;
        if (!frondNative::guestAsleep(soc)) awake += after - before;
    }
    return !frondNative::guestAsleep(soc);
}

// After a failed NativePrepare: the T|X and the Zodiac have already woken the UI task there.
inline SyscallPrepareMode FallbackMode(SoC& soc) {
    return soc.GetDeviceType() == DeviceType5::deviceTypeTX || ZodiacNative::Applies(soc) ? SYSCALL_PREPARE_AFTER_NATIVE
                                                                                          : SYSCALL_PREPARE_WAKE;
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
// so the wait stops there. Cycles are counted at 100 MHz.
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

// The database header (name, type and creator) of each install whose sync notify and launcher
// relaunch are still to come (InstallLater), and the host time of the last one, in microseconds.
struct PendingFinish {
    std::vector<std::array<uint8_t, 78>> headers;
    uint64_t at{0};
    unsigned tries{0};
};

inline PendingFinish& pendingFinish() {
    static PendingFinish pending;
    return pending;
}

// The sync notify for each header and one launcher relaunch after them, as HotSync does at the end
// of a sync, in one transaction of their own: if they fail, the guest goes back to just after the
// installs, which still succeeded. At the safe point NativePrepare or syscallDispatchPrepare found.
inline void FinishPrepared(SoC& soc, bool native, const std::vector<std::array<uint8_t, 78>>& headers) {
    constexpr uint32_t application = 0x6170706cu;
    bool app = false;
    for (const auto& header : headers) app |= (uint32_t(header[60]) << 24 | header[61] << 16 | header[62] << 8 | header[63]) == application;
    try {
        ReadOnlyGuest transaction(soc);
        if (!native) {
            for (const auto& header : headers) dbInstallerFinish(soc.GetSyscallDispatch(), header.size(), header.data(), false);
            if (app) relaunchLauncher(soc.GetSyscallDispatch());
        } else {
            WithSession(soc, [&](auto& session) {
                DatabaseOf<decltype(session)> database(session);
                for (const auto& header : headers) database.FinishInstall(header.data(), header.size(), false);
                if (app) database.RelaunchLauncher();
                database.Finish();
                session.Close();
            });
        }
        transaction.Commit();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "install: sync notify or launcher relaunch failed, the database is installed: %s\n", error.what());
    }
}

// Installs at the app's next event wait. With finish, the sync notify and the launcher relaunch run
// right after it; without, the header joins pendingFinish for FinishPending.
inline int InstallNow(SoC& soc, size_t size, void* data, bool finish) {
    if (!Wake(soc)) return DB_INSTALL_RESULT_ERR_NOT_CURRENTLY_POSSIBLE;
    const bool native = NativePrepare(soc);
    if (!native) {
        if (!syscallDispatchM68kSupport(soc.GetSyscallDispatch())) return DB_INSTALL_RESULT_ERR_NOT_SUPPORTED;
        if (!syscallDispatchPrepare(soc.GetSyscallDispatch(), FallbackMode(soc))) return DB_INSTALL_RESULT_ERR_NOT_CURRENTLY_POSSIBLE;
        ReadOnlyGuest transaction(soc);
        const int result = dbInstallerInstall(soc.GetSyscallDispatch(), size, data);
        if (result < 0) return result;
        transaction.Commit();
        if (result != DB_INSTALL_RESULT_OK) return result;
    } else {
        ReadOnlyGuest transaction(soc);
        const int result = WithSession(soc, [&](auto& session) {
            DatabaseOf<decltype(session)> database(session);
            const int result = database.Install(static_cast<uint8_t*>(data), size);
            if (result < 0) return result;
            database.Finish();
            session.Close();
            return result;
        });
        if (result < 0) return result;
        transaction.Commit();
        if (result != DB_INSTALL_RESULT_OK) return result;
    }
    if (size < 78) return DB_INSTALL_RESULT_OK;
    std::array<uint8_t, 78> header;
    std::memcpy(header.data(), data, header.size());
    if (finish) {
        // The guest is still at the safe point the install ran at.
        FinishPrepared(soc, native, {header});
    } else {
        pendingFinish().headers.push_back(header);
    }
    return DB_INSTALL_RESULT_OK;
}

inline int Install(SoC& soc, size_t size, void* data) {
    const int result = InstallNow(soc, size, data, true);
    if (result >= 0) noteStoreWrite(soc);
    return result;
}

// Installs one file of what may be a batch: the sync notifies and the one launcher relaunch wait for
// FinishPending, so files sent one after another don't each relaunch the launcher and take a second
// checkpoint. now is host time in microseconds.
inline int InstallLater(SoC& soc, size_t size, void* data, uint64_t now) {
    const int result = InstallNow(soc, size, data, false);
    if (result >= 0) noteStoreWrite(soc);
    if (!pendingFinish().headers.empty()) pendingFinish().at = now;
    return result;
}

// How long after the last InstallLater the batch counts as done, in microseconds of host time.
constexpr uint64_t kFinishDelay = 500000;
inline bool FinishDue(uint64_t now) {
    const auto& pending = pendingFinish();
    return !pending.headers.empty() && now - pending.at >= kFinishDelay;
}
inline bool FinishWaiting() { return !pendingFinish().headers.empty(); }

// Sends what InstallLater left: the notifies and one relaunch. A sleeping Palm isn't woken for it;
// it's tried again kFinishDelay later, and dropped after 20 tries (the databases are installed
// either way). now is host time in microseconds.
inline void FinishPending(SoC& soc, uint64_t now) {
    auto& pending = pendingFinish();
    if (pending.headers.empty()) return;
    bool native = false;
    const bool ready = !frondNative::guestAsleep(soc) &&
                       ((native = NativePrepare(soc)) ||
                        (syscallDispatchM68kSupport(soc.GetSyscallDispatch()) &&
                         syscallDispatchPrepare(soc.GetSyscallDispatch(), FallbackMode(soc))));
    if (!ready) {
        pending.at = now;
        if (++pending.tries < 20) return;
        std::fprintf(stderr, "install: sync notify and launcher relaunch dropped, Palm OS isn't ready\n");
        pending = {};
        return;
    }
    const auto headers = std::move(pending.headers);
    pending = {};
    FinishPrepared(soc, native, headers);
}

enum class LaunchResult { notReady, failed, launched };
inline LaunchResult Launch(SoC& soc, const std::string& name) {
    if (!Wake(soc)) return LaunchResult::notReady;
    if (!NativePrepare(soc)) {
        if (!syscallDispatchPrepare(soc.GetSyscallDispatch(), FallbackMode(soc))) return LaunchResult::notReady;
        ReadOnlyGuest transaction(soc);
        if (!launchAppByName(soc.GetSyscallDispatch(), name.c_str())) return LaunchResult::failed;
        transaction.Commit();
        return LaunchResult::launched;
    }
    ReadOnlyGuest transaction(soc);
    const bool launched = WithSession(soc, [&](auto& session) {
        DatabaseOf<decltype(session)> database(session);
        if (!database.Launch(name)) return false;
        database.Finish();
        session.Close();
        return true;
    });
    if (!launched) return LaunchResult::failed;
    transaction.Commit();
    return LaunchResult::launched;
}

// Deleting commits like an install. Returns nullopt when no supported boundary is available. Deleting
// an application then relaunches the launcher in its own transaction: if that fails, the guest goes
// back to just after the delete, which still succeeded.
inline std::optional<DeleteDbResult> DeleteNow(SoC& soc, const std::string& name) {
    if (!Wake(soc)) return std::nullopt;
    bool application = false;
    DeleteDbResult result;
    if (!NativePrepare(soc)) {
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
        result = WithSession(soc, [&](auto& session) {
            DatabaseOf<decltype(session)> database(session);
            const DeleteDbResult result = database.Delete(name, &application);
            database.Finish();
            session.Close();
            return result;
        });
        if (result == DeleteDbResult::deleted) transaction.Commit();
    }
    if (result == DeleteDbResult::deleted && application) {
        try {
            ReadOnlyGuest transaction(soc);
            WithSession(soc, [&](auto& session) {
                DatabaseOf<decltype(session)> database(session);
                database.RelaunchLauncher();
                database.Finish();
                session.Close();
            });
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

// The RAM databases' directory entries, without serialising any database. Only wakes a sleeping
// Palm with wake (a lookup for a delete the user asked for). Read-only: the guest
// is rolled back afterwards like Export. Returns false only when no supported boundary is available.
inline bool List(SoC& soc, DbMetadataList& list, bool wake = false) {
    if (wake ? !Wake(soc) : frondNative::guestAsleep(soc)) return false;
    if (NativePrepare(soc)) {
        ReadOnlyGuest transaction(soc);
        list = WithSession(soc, [&](auto& session) {
            DatabaseOf<decltype(session)> database(session);
            auto found = database.List();
            database.Finish();
            session.Close();
            return found;
        });
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
    if (!Wake(soc)) return false;
    if (backupType == DbBackup::BACKUP_TYPE_RAM && NativePrepare(soc)) {
        ReadOnlyGuest transaction(soc);
        bytes = WithSession(soc, [&](auto& session) {
            DatabaseOf<decltype(session)> database(session);
            auto archive = database.Export(name);
            database.Finish();
            session.Close();
            return archive;
        });
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
