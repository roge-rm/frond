#include "HotSync.h"

#include <algorithm>
#include <ctime>
#include <random>

#include "PalmConduit.h"
#include "Sha256.h"

namespace frondSync {

namespace {

std::string Hex(uint32_t v) {
    char text[9];
    snprintf(text, sizeof(text), "%08x", v);
    return text;
}

// The host's local time as Palm seconds, which is how Palm Desktop dates a sync.
uint32_t LocalPalmNow() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return PalmTime(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
}

uint32_t NewUserId() {
    std::random_device device;
    std::mt19937 generator(device());
    return std::uniform_int_distribution<uint32_t>(1, 0xfffffffe)(generator);
}

class Failure {
   public:
    explicit Failure(std::string why) : why(std::move(why)) {}
    std::string why;
};

// A DLP call that must work; a failed line or a refusal ends the sync.
void Must(int rc, DlpClient& dlp, const char* what) {
    if (rc < 0) throw Failure(std::string(what) + ": " + (dlp.LinkError().empty() ? "the line failed" : dlp.LinkError()));
    if (rc) throw Failure(std::string(what) + ": " + DlpErrorText(uint16_t(rc)));
}

struct Session {
    PacketLink& link;
    DlpClient dlp;
    const SyncOptions& options;
    SyncManager& manager;
    Transport transport;
    SyncLibrary library;
    ProfileSyncLock syncLock;
    SyncProfile profile;
    ProfileInfo info;
    Assignment assignment;
    SysInfo sys;
    UserInfo user;
    HotSyncResult result;
    std::vector<uint32_t> conduits;

    Session(PacketLink& link, const SyncOptions& options, SyncManager& manager, Transport transport)
        : link(link),
          dlp(link, transport == Transport::network && options.networkReplyIdleMs > 0
                        ? options.networkReplyIdleMs : DlpClient::defaultTimeoutMs),
          options(options), manager(manager), transport(transport) {}

    void Stage(const char* stage, int total = 0) {
        manager.Update([&](SyncStatus& s) {
            s.stage = stage, s.done = 0, s.total = total, s.item.clear();
        });
    }
    void Progress(int done, const std::string& item) {
        manager.Update([&](SyncStatus& s) { s.done = done, s.item = item; });
    }
    void Log(const std::string& text) {
        if (profile.IsOpen()) profile.Log(result.deviceKey, text);
    }
    bool CloseDbEx() const { return (sys.romVersion >> 24) >= 3; }

    // Picks the profile and device key, making the user on a new handheld.
    void Identify(const std::string& root) {
        std::string error;
        if (!library.Open(root, error)) throw Failure(error);
        const bool newUser = !user.userId || user.name.empty();
        auto keyFor = [&](const UserInfo& u) {
            return options.machineKey.empty() ? "palm:" + Hex(u.userId) + ":" + u.name : MachineDeviceKey(options.machineKey);
        };
        std::optional<ProfileInfo> chosen;
        if (!options.profile.empty()) {
            chosen = library.FindProfile(options.profile);
            if (!chosen) {
                ProfileInfo made;
                if (!library.CreateProfile(options.profile, made, error)) throw Failure(error);
                chosen = made;
            }
        } else if (!newUser || !options.machineKey.empty()) {
            if (auto a = library.FindAssignment(keyFor(user))) chosen = library.FindProfile(a->profileId);
        }
        if (!chosen) {
            chosen = library.FindProfile("Default");
            if (!chosen) {
                ProfileInfo made;
                if (!library.CreateProfile("Default", made, error)) throw Failure(error);
                chosen = made;
            }
        }
        if (!syncLock.Acquire(root, chosen->id, error)) throw Failure(error);
        const auto current = library.FindProfile(chosen->id);
        if (!current || current->folder != chosen->folder || current->created != chosen->created)
            throw Failure("The sync profile was changed");
        info = *current;
        if (!profile.Open(library.ProfilePath(info), error)) throw Failure(error);
        result.profile = info.name;
        if (newUser) {
            // A new or hard-reset handheld: name it and give it an ID, as Palm Desktop does
            // once the user picks a name.
            user.userId = NewUserId();
            user.name = (options.userName.empty() ? info.name : options.userName).substr(0, 40);
            user.viewerId = 0;
            Must(dlp.WriteUserInfo(user, 0x80 | 0x10), dlp, "Naming the handheld");
        }
        result.user = user.name;
        result.deviceKey = keyFor(user);
        if (!library.Assign(result.deviceKey, info.id, error)) throw Failure(error);
        const auto selected = library.FindAssignment(result.deviceKey);
        if (!selected || selected->profileId != info.id) throw Failure("The device's profile assignment changed");
        assignment = *selected;
        if (assignment.userId != user.userId || assignment.userName != user.name) {
            assignment.userName = user.name;
            assignment.userId = user.userId;
            if (!library.RememberAssignmentUser(assignment, error)) throw Failure(error);
        }
        result.fast = user.lastSyncPc == profile.PcId() && !assignment.slowNext && !newUser;
        manager.Update([&](SyncStatus& s) { s.profile = info.name, s.user = user.name; });
        Log(std::string(newUser ? "New user " : "HotSync with ") + user.name + " over " + TransportName(transport) +
            (result.fast ? ", fast sync" : ", slow sync"));
    }

    void Install() {
        auto items = profile.PendingInstalls(result.deviceKey);
        items.erase(std::remove_if(items.begin(), items.end(), [](const auto& item) { return item.target != "ram"; }), items.end());
        if (items.empty()) return;
        Must(dlp.OpenConduit(), dlp, "Starting Install");
        Stage("install", int(items.size()));
        int index = 0;
        for (const auto& item : items) {
            Progress(index++, item.dbName);
            std::string why;
            if (InstallOne(item, why)) {
                ++result.installed;
                if (!profile.FinishInstall(item.id, true, "Installed"))
                    throw Failure("Could not update the install queue");
                Log("Installed " + item.dbName);
            } else {
                ++result.installFailed;
                Log("Not installed " + item.dbName + ": " + why);
                if (!link.error.empty()) throw Failure("Install of " + item.dbName + ": " + why);
                if (!profile.FinishInstall(item.id, false, why))
                    throw Failure("Could not update the install queue");
            }
        }
        Progress(index, "");
    }

    bool InstallOne(const InstallItem& item, std::string& why) {
        Bytes file;
        PalmDb db;
        if (!profile.LoadInstall(item.id, file) || !PalmDb::Read(file, db, why)) {
            if (why.empty()) why = "The queued file is damaged.";
            return false;
        }
        if (!Installable(db, why)) return false;
        const auto ram = ListRam();
        const auto existing = std::find_if(ram.begin(), ram.end(), [&](const DbInfo& entry) { return entry.name == db.name; });
        PalmDb original;
        std::string recovery;
        if (existing != ram.end()) {
            if (!SaveRecovery(*existing, original, recovery, why)) {
                why = "Could not save a recovery copy: " + why;
                return false;
            }
            if (!Installable(original, why)) return false;
            const int removed = dlp.DeleteDB(0, db.name);
            if (removed)
                return why = removed < 0 ? dlp.LinkError() : "Could not replace it: " + DlpErrorText(uint16_t(removed)), false;
        }
        if (!WriteDatabase(db, why)) {
            if (!recovery.empty()) {
                if (link.error.empty()) {
                    std::string restoreError;
                    if (WriteDatabase(original, restoreError)) why += ". Restored the previous copy";
                    else why += ". Could not restore the previous copy: " + restoreError;
                }
                why += ". Recovery backup: " + recovery;
            }
            return false;
        }
        if (db.attributes & dbResetAfterInstall) result.reset = true;
        return true;
    }

    bool Installable(const PalmDb& db, std::string& why) const {
        if (db.appInfo.size() > 0xffff || db.sortInfo.size() > 0xffff)
            return why = "The database has a block over 64 KB.", false;
        for (const auto& r : db.records)
            if (r.data.size() > 0xffff - 10 || (sys.maxRecordSize && r.data.size() > sys.maxRecordSize))
                return why = "A record or resource is too large for this handheld.", false;
        return true;
    }

    bool WriteDatabase(const PalmDb& db, std::string& why) {
        const uint16_t flags = db.attributes & (dbResource | dbBackup | dbOkToInstallNewer | dbResetAfterInstall |
                                                dbCopyPrevention | dbStream | dbHidden | dbLaunchableData | dbRecyclable | dbBundle);
        uint8_t handle = 0;
        int rc = dlp.CreateDB(db.creator, db.type, 0, flags, db.version, db.name, handle);
        if (rc) return why = rc < 0 ? dlp.LinkError() : "The handheld refused it: " + DlpErrorText(uint16_t(rc)), false;
        auto fail = [&](int code, const char* what) {
            why = code < 0 ? dlp.LinkError() : std::string(what) + ": " + DlpErrorText(uint16_t(code));
            if (code >= 0) {
                dlp.CloseDB(handle);
                dlp.DeleteDB(0, db.name);
            }
            return false;
        };
        if (!db.appInfo.empty() && (rc = dlp.WriteAppBlock(handle, db.appInfo))) return fail(rc, "App info");
        if (!db.sortInfo.empty() && (rc = dlp.WriteSortBlock(handle, db.sortInfo))) return fail(rc, "Sort info");
        for (const auto& r : db.records) {
            if (db.Resource()) {
                if ((rc = dlp.WriteResource(handle, r))) return fail(rc, "Writing a resource");
            } else {
                if (r.attributes & recDeleted) continue;
                uint32_t uid = 0;
                if ((rc = dlp.WriteRecord(handle, 0, r, uid))) return fail(rc, "Writing a record");
            }
        }
        if ((rc = dlp.CloseDB(handle))) return fail(rc, "Closing it");
        return true;
    }

    std::vector<DbInfo> ListRam() {
        std::vector<DbInfo> all;
        const bool multiple = sys.dlpMajor > 1 || sys.dlpMinor >= 2;
        for (uint16_t start = 0;;) {
            std::vector<DbInfo> some;
            const int rc = dlp.ReadDBList(uint8_t(0x80 | (multiple ? 0x20 : 0)), 0, start, some);
            if (rc == dlpErrNotFound) break;
            Must(rc, dlp, "Listing databases");
            if (some.empty()) break;
            all.insert(all.end(), some.begin(), some.end());
            start = uint16_t(some.back().index + 1);
        }
        return all;
    }

    std::string PimScope() const { return result.deviceKey + "/" + Hex(user.userId) + "/" + assignment.generation; }

    void SyncPersonalData() {
        const auto scope = PimScope();
        const auto previous = profile.DeviceValue(scope, "pim-sync-date");
        const bool fast = result.fast && previous == std::to_string(user.lastSync);
        std::string error;
        if (!profile.SetDeviceValue(scope, "pim-sync-date", "", &error)) throw Failure(error);
        const auto ram = ListRam();
        for (const auto& database : SelectPalmPimDatabases(ram)) {
            Stage(PimKindName(database.kind));
            if (profile.GetPimAction(result.deviceKey, database.kind) == PimAction::desktopOverwrites) {
                const auto existing = std::find_if(ram.begin(), ram.end(), [&](const DbInfo& entry) { return entry.name == database.name; });
                PalmDb original;
                std::string recovery;
                if (!SaveRecovery(*existing, original, recovery, error))
                    throw Failure("Could not save a recovery copy of " + database.name + ": " + error);
            }
            if (!SyncPalmPim(dlp, profile, result.deviceKey, scope, database, fast, result.pim, error))
                throw Failure(error);
        }
    }

    void SyncVendorConduits(const std::atomic<bool>* stop) {
        const auto scope = PimScope();
        std::string error;
        auto wasFast = [&](uint32_t creator) {
            const auto key = "palm-conduit-sync-date-" + Hex(creator);
            const bool fast = result.fast && profile.DeviceValue(scope, key) == std::to_string(user.lastSync);
            if (!profile.SetDeviceValue(scope, key, "", &error)) throw Failure(error);
            if (conduits.empty()) Stage("conduits");
            conduits.push_back(creator);
            return fast;
        };
        auto recovery = [&](const std::string& name, uint8_t card, std::string& why) {
            if (card) {
                why = "Recovery is available for main memory databases.";
                return false;
            }
            const auto ram = ListRam();
            const auto existing = std::find_if(ram.begin(), ram.end(), [&](const DbInfo& entry) { return entry.name == name; });
            if (existing == ram.end()) return true;
            PalmDb original;
            std::string source;
            return SaveRecovery(*existing, original, source, why);
        };
        if (!RunPalmConduits(dlp, library.ProfilePath(info), result.deviceKey, user, stop, wasFast, recovery,
                            [&](const std::string& line) { Log(line); }, error))
            throw Failure(error);
    }

    void Backup() {
        Must(dlp.OpenConduit(), dlp, "Starting Backup");
        const auto list = ListRam();
        Stage("backup", int(list.size()));
        int index = 0;
        for (const auto& entry : list) {
            Progress(index++, entry.name);
            if (entry.misc & 0x80) {
                ++result.backupSkipped;  // the handheld asks for it to be left out
                continue;
            }
            const auto stored = profile.FindBackup(result.deviceKey, entry.name);
            const bool changed = !stored || stored->modNumber != entry.modNumber || stored->modified != entry.modified ||
                                 stored->attributes != (entry.attributes & ~dbOpen);
            const bool wanted = (entry.attributes & dbBackup) || entry.modified > entry.backedUp || stored;
            if (!wanted || !changed) {
                ++result.backupSkipped;
                continue;
            }
            std::string why;
            if (BackupOne(entry, why)) {
                ++result.backedUp;
            } else {
                ++result.backupFailed;
                Log("Not backed up " + entry.name + ": " + why);
                if (!link.error.empty()) throw Failure("Backup of " + entry.name + ": " + why);
            }
        }
        Progress(index, "");
    }

    bool ReadDatabase(const DbInfo& entry, PalmDb& db, uint8_t& handle, std::string& why, bool recovery) {
        db.name = entry.name;
        db.attributes = entry.attributes & ~dbOpen;
        db.version = entry.version;
        db.created = entry.created, db.modified = entry.modified, db.backedUp = entry.backedUp;
        db.modNumber = entry.modNumber;
        db.type = entry.type, db.creator = entry.creator;
        int rc = dlp.OpenDB(0, 0x80 | 0x10, entry.name, handle);
        if (rc) return why = rc < 0 ? dlp.LinkError() : "Couldn't open it: " + DlpErrorText(uint16_t(rc)), false;
        auto fail = [&](int code, const char* what) {
            why = code < 0 ? dlp.LinkError() : std::string(what) + ": " + DlpErrorText(uint16_t(code));
            if (code >= 0) dlp.CloseDB(handle);
            return false;
        };
        OpenDbInfo open;
        if ((rc = dlp.ReadOpenDBInfo(handle, open))) return fail(rc, "Reading its size");
        if ((rc = dlp.ReadAppBlock(handle, db.appInfo))) return fail(rc, "Reading app info");
        if ((rc = dlp.ReadSortBlock(handle, db.sortInfo))) return fail(rc, "Reading sort info");
        for (uint16_t i = 0; i < open.records; ++i) {
            PalmRecord r;
            rc = db.Resource() ? dlp.ReadResourceByIndex(handle, i, r) : dlp.ReadRecordByIndex(handle, i, r);
            if (rc == dlpErrNotFound && !db.Resource() && !recovery) continue;
            if (rc) return fail(rc, "Reading a record");
            db.records.push_back(std::move(r));
        }
        return true;
    }

    bool SaveRecovery(const DbInfo& entry, PalmDb& db, std::string& source, std::string& why) {
        uint8_t handle = 0;
        if (!ReadDatabase(entry, db, handle, why, true)) return false;
        const int rc = dlp.CloseDB(handle);
        if (rc) return why = rc < 0 ? dlp.LinkError() : DlpErrorText(uint16_t(rc)), false;
        Bytes file, verified;
        if (!db.Write(file, &why)) return false;
        source = result.deviceKey + " / recovery " + Sha256Hex(file.data(), file.size());
        if (!profile.StoreRecovery(source, db, file, why)) return false;
        if (!profile.LoadBackup(source, db.name, verified) || verified != file)
            return why = "The recovery backup could not be verified.", false;
        return true;
    }

    bool BackupOne(const DbInfo& entry, std::string& why) {
        PalmDb db;
        uint8_t handle = 0;
        if (!ReadDatabase(entry, db, handle, why, false)) return false;
        Bytes file;
        std::string error;
        const bool stored = db.Write(file, &error) && profile.StoreBackup(result.deviceKey, db, file, error);
        // Mark it backed up on the handheld only after the profile has the file.
        const int rc = dlp.CloseDB(handle, stored && CloseDbEx() ? 0x80 : 0);
        if (rc) return why = rc < 0 ? dlp.LinkError() : "Closing it: " + DlpErrorText(uint16_t(rc)), false;
        if (!stored) why = std::move(error);
        return stored;
    }

    void Finish() {
        Stage("finishing");
        UserInfo done = user;
        done.lastSyncPc = profile.PcId();
        done.lastSync = LocalPalmNow();
        Must(dlp.WriteUserInfo(done, 0x40 | 0x20), dlp, "Saving the sync");
        std::string summary = "Frond sync manager, profile " + info.name + ".\n";
        if (result.installed || result.installFailed)
            summary += "Install: " + std::to_string(result.installed) + " installed" +
                       (result.installFailed ? ", " + std::to_string(result.installFailed) + " failed" : "") + ".\n";
        if (result.pim.downloaded || result.pim.uploaded || result.pim.deleted || result.pim.conflicts)
            summary += "Personal data: " + std::to_string(result.pim.downloaded) + " received, " +
                       std::to_string(result.pim.uploaded) + " sent, " + std::to_string(result.pim.deleted) +
                       " deleted, " + std::to_string(result.pim.conflicts) + " conflicts.\n";
        summary += "Backup: " + std::to_string(result.backedUp) + " backed up" +
                   (result.backupFailed ? ", " + std::to_string(result.backupFailed) + " failed" : "") + ".\n";
        dlp.AddSyncLogEntry(summary);
        if (result.reset) dlp.ResetSystem();
        Must(dlp.EndOfSync(dlpTermNormal), dlp, "Ending the sync");
        profile.SetDeviceValue(PimScope(), "pim-sync-date", std::to_string(done.lastSync));
        for (const auto creator : conduits)
            profile.SetDeviceValue(PimScope(), "palm-conduit-sync-date-" + Hex(creator), std::to_string(done.lastSync));
        assignment.userName = user.name;
        assignment.userId = user.userId;
        assignment.lastSync = NowSeconds();
        assignment.slowNext = false;
        std::string error;
        library.FinishAssignment(assignment, error);
    }
};

}  // namespace

HotSyncResult RunHotSync(PacketLink& link, const SyncOptions& options, const std::string& root, Transport transport,
                         SyncManager& manager, const std::atomic<bool>* stop) {
    Session s(link, options, manager, transport);
    try {
        s.Stage("user");
        Must(s.dlp.ReadSysInfo(s.sys), s.dlp, "Reading the handheld's system info");
        Must(s.dlp.ReadUserInfo(s.user), s.dlp, "Reading the handheld's user");
        s.Identify(root);
        s.Install();
        s.SyncPersonalData();
        s.SyncVendorConduits(stop);
        s.Backup();
        s.Finish();
        s.result.ok = true;
        s.result.message = "Profile " + s.info.name + ", user " + s.user.name + ": " + std::to_string(s.result.installed) +
                           " installed" + (s.result.installFailed ? " (" + std::to_string(s.result.installFailed) + " failed)" : "") +
                           ", " + std::to_string(s.result.backedUp) + " backed up" +
                           (s.result.backupFailed ? " (" + std::to_string(s.result.backupFailed) + " failed)" : "") +
                           (s.result.fast ? ", fast" : ", slow");
        s.Log("Done: " + s.result.message);
    } catch (const Failure& failure) {
        s.result.ok = false;
        s.result.message = failure.why;
        s.Log("Failed: " + failure.why);
        // Tell the handheld if the line still works, so it shows the failure and stops waiting.
        if (link.error.empty()) s.dlp.EndOfSync(dlpTermOther);
    } catch (const std::exception& error) {
        s.result.ok = false;
        s.result.message = error.what();
        s.Log("Failed: " + s.result.message);
        if (link.error.empty()) s.dlp.EndOfSync(dlpTermOther);
    }
    return s.result;
}

}  // namespace frondSync
