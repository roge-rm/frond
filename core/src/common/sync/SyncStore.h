#ifndef FROND_SYNC_STORE_H
#define FROND_SYNC_STORE_H

// The sync manager's storage, the same files on every platform. A root folder holds
// profiles.db (the profiles and which device syncs to which) and one folder per profile with
// its profile.db (backups, the install queue, per-device sync state and the log). Text is
// UTF-8 except Palm database names, which are kept as the handheld's bytes; times are UTC
// seconds since 1970. No host paths are stored, so a root or a profile folder can move to
// another computer or phone. Each object is used from one thread at a time.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "PalmDb.h"
#include "PimRecord.h"

struct sqlite3;

namespace frondSync {

struct ProfileInfo {
    int64_t id{0};
    std::string name;
    std::string folder;  // folder name under the root
    int64_t created{0};
};

// One handheld. Palm devices are keyed by "palm:" and the machine key the frontend gives, or
// by the Palm user ID and name when it gives none.
struct Assignment {
    std::string deviceKey;
    int64_t profileId{0};
    std::string kind{"palm"};  // palm or winmo
    std::string userName;
    uint32_t userId{0};
    int64_t lastSync{0};
    bool slowNext{false};  // the next sync is a slow one (moved to another profile)
    std::string generation;  // changes when the device leaves this assignment
};

// Serializes guest syncs and profile deletion across frontend and core processes.
class ProfileSyncLock {
   public:
    ProfileSyncLock() = default;
    ~ProfileSyncLock();
    ProfileSyncLock(const ProfileSyncLock&) = delete;
    ProfileSyncLock& operator=(const ProfileSyncLock&) = delete;

    bool Acquire(const std::string& root, int64_t profileId, std::string& error);

   private:
    int fd{-1};
};

class SyncLibrary {
   public:
    SyncLibrary();
    ~SyncLibrary();
    SyncLibrary(const SyncLibrary&) = delete;
    SyncLibrary& operator=(const SyncLibrary&) = delete;

    // Opens or makes root/profiles.db, making the root folder too.
    bool Open(const std::string& root, std::string& error);
    void Close();
    const std::string& Root() const { return root; }

    std::vector<ProfileInfo> Profiles();
    std::optional<ProfileInfo> FindProfile(int64_t id);
    std::optional<ProfileInfo> FindProfile(const std::string& name);
    // A new profile and its folder. Names are unique, compared without case.
    bool CreateProfile(const std::string& name, ProfileInfo& out, std::string& error);
    bool RenameProfile(int64_t id, const std::string& name, std::string& error);
    // Deletes the profile, its folder and its assignments.
    bool DeleteProfile(int64_t id, std::string& error);
    // The folder's full path.
    std::string ProfilePath(const ProfileInfo& profile) const;

    std::optional<Assignment> FindAssignment(const std::string& deviceKey);
    std::vector<Assignment> Assignments();
    // Assigns or moves a device. Moving it to another profile makes its next sync slow.
    bool Assign(const std::string& deviceKey, int64_t profileId, std::string& error, const std::string& kind = {});
    bool UpdateAssignment(const Assignment& assignment, std::string& error);
    bool RememberAssignmentUser(const Assignment& assignment, std::string& error);
    bool FinishAssignment(const Assignment& assignment, std::string& error);
    bool Unassign(const std::string& deviceKey, std::string& error);

   private:
    std::string root;
    sqlite3* db{nullptr};
};

struct BackupEntry {
    std::string deviceKey;
    std::string name;
    uint32_t type{0}, creator{0};
    uint16_t attributes{0}, version{0};
    uint32_t modified{0}, modNumber{0};  // the handheld's, Palm seconds
    int64_t size{0};
    std::string sha256;
    int64_t storedAt{0};
};

struct RecoveryCopy {
    std::string source;  // the key to give Backups, LoadBackup and QueueRestore
    int64_t storedAt{0};
};

// True for a recovery source; device gets the key of the device it belongs to.
bool IsRecoverySource(const std::string& source, std::string* device = nullptr);

struct InstallItem {
    int64_t id{0};
    std::string deviceKey;  // empty: the next device in this profile to sync
    std::string fileName;   // as given, for showing
    std::string dbName;
    std::string target{"ram"};  // ram for Palm databases, cab for Windows Mobile
    std::string state{"queued"};  // queued, done, failed
    std::string message;
    std::string sha256;
    int64_t size{0};
    int64_t queuedAt{0}, doneAt{0};
};

// Whether a queued item applies to a device of this assignment kind (palm or winmo). An empty kind keeps every item.
inline bool InstallItemFitsKind(const InstallItem& item, const std::string& kind) {
    if (kind.empty()) return true;
    return (item.target == "cab") == (kind == "winmo");
}

struct SyncLogEntry {
    std::string deviceKey;
    int64_t time{0};
    std::string text;
};

struct PimLink {
    uint32_t guestId{0};
    int64_t recordId{0};
    int64_t revision{0};
    std::string guestHash;
};

struct PimPending {
    int64_t recordId{0};
    int64_t revision{0};
    std::string guestHash;
};

class SyncProfile {
   public:
    static constexpr int schemaVersion = 3;

    SyncProfile();
    ~SyncProfile();
    SyncProfile(const SyncProfile&) = delete;
    SyncProfile& operator=(const SyncProfile&) = delete;

    // Opens or makes folder/profile.db. A newer schema than this build knows is refused.
    bool Open(const std::string& folder, std::string& error);
    void Close();
    bool IsOpen() const { return db; }
    // This profile's HotSync PC ID, made once at random; a handheld whose last sync PC is this
    // one can fast sync.
    uint32_t PcId() const { return pcId; }

    // Sources of stored backups. Recovery copies ("<device> / recovery <sha>") are listed only when asked for.
    std::vector<std::string> BackupDevices(bool withRecovery = false);
    // The recovery copies kept for a device, oldest first, with when each was stored.
    std::vector<RecoveryCopy> RecoveryCopies(const std::string& deviceKey);
    std::vector<BackupEntry> Backups(const std::string& deviceKey);
    std::optional<BackupEntry> FindBackup(const std::string& deviceKey, const std::string& name);
    // Stores the file form of a database as the latest backup of it from this device.
    bool StoreBackup(const std::string& deviceKey, const PalmDb& db, const Bytes& file, std::string& error);
    // Flushes the recovery backup before the guest database can be changed.
    bool StoreRecovery(const std::string& source, const PalmDb& db, const Bytes& file, std::string& error);
    bool LoadBackup(const std::string& deviceKey, const std::string& name, Bytes& file);
    bool DeleteBackup(const std::string& deviceKey, const std::string& name, std::string& error);

    // Validates and queues a Palm database or, with target "cab", a Windows Mobile CAB.
    bool QueueInstall(const std::string& deviceKey, const std::string& fileName, const Bytes& file, InstallItem& out,
                      std::string& error, const std::string& target = "ram");
    // Queues every backup from fromDevice for installing on toDevice, as after a hard reset.
    int QueueRestore(const std::string& fromDevice, const std::string& toDevice, std::string& error);
    std::vector<InstallItem> InstallQueue(bool pendingOnly = false);
    // Queued items for this device and for any device, oldest first.
    std::vector<InstallItem> PendingInstalls(const std::string& deviceKey);
    bool LoadInstall(int64_t id, Bytes& file);
    bool FinishInstall(int64_t id, bool ok, const std::string& message);
    bool RemoveInstall(int64_t id, std::string& error);

    std::vector<PimRecord> PimRecords(PimKind kind, bool includeDeleted = false);
    bool ReadPimRecords(PimKind kind, bool includeDeleted, std::vector<PimRecord>& records, std::string& error);
    std::optional<PimRecord> FindPimRecord(int64_t id);
    bool SavePimRecord(PimRecord& record, std::string& error);
    bool DeletePimRecord(int64_t id, int64_t revision, std::string& error);

    PimAction GetPimAction(const std::string& deviceKey, PimKind kind);
    // Sync and skip are defaults. Either overwrite is consumed after a successful conduit.
    bool SetPimAction(const std::string& deviceKey, PimKind kind, PimAction action, std::string& error);

    bool FinishPimAction(const std::string& deviceKey, PimKind kind, PimAction action, std::string& error);

    // Links are scoped by device, Palm user and database, since guest IDs can be reused.
    std::vector<PimLink> PimLinks(const std::string& scope, std::string& error);
    bool SavePimLink(const std::string& scope, const PimLink& link, std::string& error);
    bool RemovePimLink(const std::string& scope, uint32_t guestId, std::string& error);
    std::vector<PimPending> PendingPimWrites(const std::string& scope, std::string& error);
    bool SetPendingPimWrite(const std::string& scope, const PimPending& pending, std::string& error);
    bool ClearPendingPimWrite(const std::string& scope, int64_t recordId, std::string& error);
    bool BeginPimChange(std::string& error);
    bool CommitPimChange(std::string& error);
    void RollBackPimChange();

    void Log(const std::string& deviceKey, const std::string& text);
    std::vector<SyncLogEntry> LogEntries(const std::string& deviceKey, int limit = 100);

    bool HotSyncUsesNetwork(const std::string& deviceKey);
    bool SetHotSyncNetwork(const std::string& deviceKey, bool network, std::string& error);

    bool ActiveSyncUsesSerial(const std::string& deviceKey);
    bool SetActiveSyncSerial(const std::string& deviceKey, bool serial, std::string& error);
    // "requested" (also with nothing chosen yet), "confirmed", or empty when the user cancelled (stored as "").
    std::string ActiveSyncPairing(const std::string& deviceKey);
    bool RequestActiveSyncPairing(const std::string& deviceKey, bool request, std::string& error);
    bool ConfirmActiveSyncPairing(const std::string& deviceKey, std::string& error);
    // Back to nothing chosen, which pairs on the next sync.
    bool ForgetActiveSyncPairing(const std::string& deviceKey, std::string& error);
    bool ClearConfirmedActiveSyncPairing(const std::string& deviceKey, std::string& error);

    // Small per-device values kept between syncs (last backup time and the like).
    std::string DeviceValue(const std::string& deviceKey, const std::string& key);
    bool SetDeviceValue(const std::string& deviceKey, const std::string& key, const std::string& value,
                        std::string* error = nullptr);

   private:
    bool AddInstall(const std::string& deviceKey, const std::string& fileName, const Bytes& file, InstallItem& out,
                    std::string& error, const std::string& target);
    bool PutBlob(const Bytes& data, std::string& sha, std::string& error);
    bool GetBlob(const std::string& sha, Bytes& data);
    void DropUnusedBlobs();

    sqlite3* db{nullptr};
    uint32_t pcId{0};
};

int64_t NowSeconds();
// The platform data folder used when a sync root was not specified.
std::string DefaultSyncRoot();
bool IsCabFileName(const std::string& name);
std::string ActiveSyncFilesFolder(const std::string& profileFolder, bool create = false);

inline std::string MachineDeviceKey(const std::string& id) { return "machine:" + id; }

}  // namespace frondSync

#endif
