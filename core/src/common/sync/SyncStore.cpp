#include "SyncStore.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <random>

#include "Sha256.h"
#include "vendor/sqlite/sqlite3.h"

namespace fs = std::filesystem;

namespace frondSync {

std::string DefaultSyncRoot() {
#if defined(_WIN32)
    if (const char* appData = std::getenv("APPDATA"); appData && *appData) return std::string(appData) + "\\Frond\\sync";
    return "Frond\\sync";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home) return std::string(home) + "/Library/Application Support/Frond/sync";
    return "Frond/sync";
#else
    if (const char* data = std::getenv("XDG_DATA_HOME"); data && *data == '/') return std::string(data) + "/frond/sync";
    if (const char* home = std::getenv("HOME"); home && *home) return std::string(home) + "/.local/share/frond/sync";
    return "frond-sync";
#endif
}

int64_t NowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

namespace {

// One prepared statement; finalized when it goes.
class Query {
   public:
    Query(sqlite3* db, const char* sql) { ok = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK; }
    ~Query() { sqlite3_finalize(stmt); }
    Query& Bind(int i, int64_t v) { return sqlite3_bind_int64(stmt, i, v), *this; }
    Query& Bind(int i, const std::string& v) { return sqlite3_bind_text(stmt, i, v.data(), int(v.size()), SQLITE_TRANSIENT), *this; }
    Query& BindBlob(int i, const Bytes& v) {
        return sqlite3_bind_blob(stmt, i, v.empty() ? "" : static_cast<const void*>(v.data()), int(v.size()), SQLITE_TRANSIENT), *this;
    }
    // A row is ready.
    bool Row() { return ok && sqlite3_step(stmt) == SQLITE_ROW; }
    // Runs to the end; false on an error.
    bool Run() {
        if (!ok) return false;
        int rc;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {}
        return rc == SQLITE_DONE;
    }
    int64_t Int(int i) { return sqlite3_column_int64(stmt, i); }
    std::string Text(int i) {
        const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
        return p ? std::string(p, size_t(sqlite3_column_bytes(stmt, i))) : std::string();
    }
    Bytes Blob(int i) {
        const auto* p = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, i));
        return p ? Bytes(p, p + sqlite3_column_bytes(stmt, i)) : Bytes();
    }
    bool ok{false};

   private:
    sqlite3_stmt* stmt{nullptr};
};

bool Exec(sqlite3* db, const char* sql, std::string& error) {
    char* message = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &message) == SQLITE_OK) return true;
    error = message ? message : "SQLite error";
    sqlite3_free(message);
    return false;
}

std::string LastError(sqlite3* db) { return db ? sqlite3_errmsg(db) : "not open"; }

bool OpenDb(const fs::path& path, sqlite3*& db, std::string& error) {
    if (sqlite3_open_v2(path.string().c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        error = "Could not open " + path.string() + ": " + LastError(db);
        sqlite3_close_v2(db);
        db = nullptr;
        return false;
    }
    sqlite3_busy_timeout(db, 5000);
    return Exec(db, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;", error);
}

int SchemaVersion(sqlite3* db) {
    Query q(db, "PRAGMA user_version");
    return q.Row() ? int(q.Int(0)) : -1;
}

bool Migrate(sqlite3* db, int version, const char* sql, std::string& error) {
    if (!Exec(db, "BEGIN IMMEDIATE", error)) return false;
    if ((SchemaVersion(db) >= version || Exec(db, sql, error)) && Exec(db, "COMMIT", error)) return true;
    std::string ignored;
    Exec(db, "ROLLBACK", ignored);
    return false;
}

uint32_t RandomId() {
    std::random_device device;
    std::mt19937 generator(device());
    std::uniform_int_distribution<uint32_t> pick(1, 0xfffffffe);
    return pick(generator);
}


// A folder name from a profile name: letters, digits and dashes, unique under the root.
std::string FolderFor(const fs::path& root, const std::string& name) {
    std::string base;
    for (unsigned char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) base += char(c);
        else if (c >= 'A' && c <= 'Z') base += char(c + 32);
        else if (!base.empty() && base.back() != '-') base += '-';
    }
    while (!base.empty() && base.back() == '-') base.pop_back();
    if (base.empty()) base = "profile";
    if (base.size() > 40) base.resize(40);
    std::string folder = base;
    std::error_code ec;
    for (int n = 2; fs::exists(root / folder, ec); ++n) folder = base + "-" + std::to_string(n);
    return folder;
}

}  // namespace

ProfileSyncLock::~ProfileSyncLock() {
    if (fd >= 0) close(fd);
}

bool ProfileSyncLock::Acquire(const std::string& root, int64_t profileId, std::string& error) {
    if (fd >= 0) return error = "The profile lock is already held", false;
    const auto path = (fs::path(root) / (".sync-" + std::to_string(profileId) + ".lock")).string();
    fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return error = "Could not open the profile sync lock", false;
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) return true;
    close(fd);
    fd = -1;
    return error = "This profile is already syncing", false;
}

SyncLibrary::SyncLibrary() = default;
SyncLibrary::~SyncLibrary() { Close(); }

void SyncLibrary::Close() {
    if (db) sqlite3_close_v2(db);
    db = nullptr;
    root.clear();
}

bool SyncLibrary::Open(const std::string& folder, std::string& error) {
    Close();
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        error = "Could not make " + folder + ": " + ec.message();
        return false;
    }
    if (!OpenDb(fs::path(folder) / "profiles.db", db, error)) return false;
    const int version = SchemaVersion(db);
    if (version > 2) {
        error = "profiles.db is from a newer Frond.";
        Close();
        return false;
    }
    if (version < 1 && !Migrate(db, 1,
                             "CREATE TABLE IF NOT EXISTS profile(id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE COLLATE NOCASE,"
                             " folder TEXT NOT NULL UNIQUE, created INTEGER NOT NULL);"
                             "CREATE TABLE IF NOT EXISTS assignment(device_key TEXT PRIMARY KEY,"
                             " profile_id INTEGER NOT NULL REFERENCES profile(id) ON DELETE CASCADE, kind TEXT NOT NULL,"
                             " user_name TEXT NOT NULL DEFAULT '', user_id INTEGER NOT NULL DEFAULT 0,"
                             " last_sync INTEGER NOT NULL DEFAULT 0, slow_next INTEGER NOT NULL DEFAULT 0);"
                             "PRAGMA user_version=1;",
                             error)) {
        Close();
        return false;
    }
    if (version < 2 &&
        !Migrate(db, 2, "ALTER TABLE assignment ADD COLUMN generation TEXT NOT NULL DEFAULT '';"
                  "UPDATE assignment SET generation=lower(hex(randomblob(16))), slow_next=1;"
                  "PRAGMA user_version=2;", error)) {
        Close();
        return false;
    }
    root = folder;
    return true;
}

std::vector<ProfileInfo> SyncLibrary::Profiles() {
    std::vector<ProfileInfo> out;
    Query q(db, "SELECT id,name,folder,created FROM profile ORDER BY name COLLATE NOCASE");
    while (q.Row()) out.push_back({q.Int(0), q.Text(1), q.Text(2), q.Int(3)});
    return out;
}

std::optional<ProfileInfo> SyncLibrary::FindProfile(int64_t id) {
    Query q(db, "SELECT id,name,folder,created FROM profile WHERE id=?");
    q.Bind(1, id);
    if (!q.Row()) return std::nullopt;
    return ProfileInfo{q.Int(0), q.Text(1), q.Text(2), q.Int(3)};
}

std::optional<ProfileInfo> SyncLibrary::FindProfile(const std::string& name) {
    Query q(db, "SELECT id,name,folder,created FROM profile WHERE name=? COLLATE NOCASE");
    q.Bind(1, name);
    if (!q.Row()) return std::nullopt;
    return ProfileInfo{q.Int(0), q.Text(1), q.Text(2), q.Int(3)};
}

std::string SyncLibrary::ProfilePath(const ProfileInfo& profile) const { return (fs::path(root) / profile.folder).string(); }

bool SyncLibrary::CreateProfile(const std::string& name, ProfileInfo& out, std::string& error) {
    if (!db) return error = "The sync folder isn't open.", false;
    if (name.empty() || name.size() > 100 || name.find('\0') != std::string::npos) return error = "Give the profile a name.", false;
    if (FindProfile(name)) return error = "There is already a profile called " + name + ".", false;
    const std::string folder = FolderFor(root, name);
    std::error_code ec;
    fs::create_directories(fs::path(root) / folder, ec);
    if (ec) return error = "Could not make the profile folder: " + ec.message(), false;
    SyncProfile profile;
    if (!profile.Open((fs::path(root) / folder).string(), error)) return false;
    Query q(db, "INSERT INTO profile(name,folder,created) VALUES(?,?,?)");
    q.Bind(1, name).Bind(2, folder).Bind(3, NowSeconds());
    if (!q.Run()) return error = LastError(db), false;
    out = {sqlite3_last_insert_rowid(db), name, folder, NowSeconds()};
    return true;
}

bool SyncLibrary::RenameProfile(int64_t id, const std::string& name, std::string& error) {
    if (name.empty() || name.size() > 100) return error = "Give the profile a name.", false;
    auto other = FindProfile(name);
    if (other && other->id != id) return error = "There is already a profile called " + name + ".", false;
    Query q(db, "UPDATE profile SET name=? WHERE id=?");
    q.Bind(1, name).Bind(2, id);
    if (!q.Run()) return error = LastError(db), false;
    if (!sqlite3_changes(db)) return error = "No such profile.", false;
    return true;
}

bool SyncLibrary::DeleteProfile(int64_t id, std::string& error) {
    ProfileSyncLock lock;
    if (!lock.Acquire(root, id, error)) return false;
    auto profile = FindProfile(id);
    if (!profile) return error = "No such profile.", false;
    Query q(db, "DELETE FROM profile WHERE id=?");
    q.Bind(1, id);
    if (!q.Run()) return error = LastError(db), false;
    std::error_code ec;
    fs::remove_all(ProfilePath(*profile), ec);
    if (ec) return error = "The profile is gone but its folder stays: " + ec.message(), false;
    return true;
}

std::optional<Assignment> SyncLibrary::FindAssignment(const std::string& deviceKey) {
    Query q(db, "SELECT device_key,profile_id,kind,user_name,user_id,last_sync,slow_next,generation FROM assignment WHERE device_key=?");
    q.Bind(1, deviceKey);
    if (!q.Row()) return std::nullopt;
    return Assignment{q.Text(0), q.Int(1), q.Text(2), q.Text(3), uint32_t(q.Int(4)), q.Int(5), q.Int(6) != 0, q.Text(7)};
}

std::vector<Assignment> SyncLibrary::Assignments() {
    std::vector<Assignment> out;
    Query q(db, "SELECT device_key,profile_id,kind,user_name,user_id,last_sync,slow_next,generation FROM assignment ORDER BY device_key");
    while (q.Row()) out.push_back({q.Text(0), q.Int(1), q.Text(2), q.Text(3), uint32_t(q.Int(4)), q.Int(5), q.Int(6) != 0, q.Text(7)});
    return out;
}

bool SyncLibrary::Assign(const std::string& deviceKey, int64_t profileId, std::string& error, const std::string& kind) {
    if (!FindProfile(profileId)) return error = "No such profile.", false;
    auto current = FindAssignment(deviceKey);
    if (current && current->profileId == profileId && (kind.empty() || current->kind == kind)) return true;
    Assignment a = current.value_or(Assignment{});
    a.deviceKey = deviceKey;
    a.slowNext = current && (current->profileId != profileId || current->slowNext);
    if (!kind.empty()) a.kind = kind;
    a.profileId = profileId;
    return UpdateAssignment(a, error);
}

bool SyncLibrary::UpdateAssignment(const Assignment& a, std::string& error) {
    Query q(db,
            "INSERT INTO assignment(device_key,profile_id,kind,user_name,user_id,last_sync,slow_next,generation) VALUES(?,?,?,?,?,?,?,lower(hex(randomblob(16))))"
            " ON CONFLICT(device_key) DO UPDATE SET profile_id=excluded.profile_id, kind=excluded.kind,"
            " user_name=excluded.user_name, user_id=excluded.user_id, last_sync=excluded.last_sync,"
            " slow_next=CASE WHEN assignment.profile_id<>excluded.profile_id OR assignment.kind<>excluded.kind THEN 1"
            " ELSE excluded.slow_next END,"
            " generation=CASE WHEN assignment.profile_id<>excluded.profile_id OR assignment.kind<>excluded.kind"
            " THEN excluded.generation ELSE assignment.generation END");
    q.Bind(1, a.deviceKey).Bind(2, a.profileId).Bind(3, a.kind).Bind(4, a.userName).Bind(5, int64_t(a.userId)).Bind(6, a.lastSync)
        .Bind(7, a.slowNext ? 1 : 0);
    if (!q.Run()) return error = LastError(db), false;
    return true;
}

bool SyncLibrary::RememberAssignmentUser(const Assignment& a, std::string& error) {
    Query q(db, "UPDATE assignment SET user_name=?,user_id=?"
                " WHERE device_key=? AND profile_id=? AND generation=?");
    q.Bind(1, a.userName).Bind(2, a.userId).Bind(3, a.deviceKey).Bind(4, a.profileId).Bind(5, a.generation);
    if (!q.Run()) return error = LastError(db), false;
    return true;
}

bool SyncLibrary::FinishAssignment(const Assignment& a, std::string& error) {
    // Respect an assignment changed by the frontend during the sync.
    Query q(db, "UPDATE assignment SET user_name=?,user_id=?,last_sync=?,slow_next=0,kind=?"
                " WHERE device_key=? AND profile_id=? AND generation=?");
    q.Bind(1, a.userName).Bind(2, a.userId).Bind(3, a.lastSync).Bind(4, a.kind).Bind(5, a.deviceKey).Bind(6, a.profileId).Bind(7, a.generation);
    if (!q.Run()) return error = LastError(db), false;
    return true;
}

bool SyncLibrary::Unassign(const std::string& deviceKey, std::string& error) {
    Query q(db, "DELETE FROM assignment WHERE device_key=?");
    q.Bind(1, deviceKey);
    if (!q.Run()) return error = LastError(db), false;
    return true;
}

SyncProfile::SyncProfile() = default;
SyncProfile::~SyncProfile() { Close(); }

void SyncProfile::Close() {
    if (db) sqlite3_close_v2(db);
    db = nullptr;
    pcId = 0;
}

bool SyncProfile::Open(const std::string& folder, std::string& error) {
    Close();
    if (!OpenDb(fs::path(folder) / "profile.db", db, error)) return false;
    const int version = SchemaVersion(db);
    if (version > schemaVersion) {
        error = "This profile is from a newer Frond.";
        Close();
        return false;
    }
    // Version 1. Later versions add their steps after this one.
    if (version < 1 &&
        !Migrate(db, 1,
              "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
              "CREATE TABLE IF NOT EXISTS blob(sha256 TEXT PRIMARY KEY, data BLOB NOT NULL);"
              "CREATE TABLE IF NOT EXISTS backup(device_key TEXT NOT NULL, name BLOB NOT NULL, type INTEGER NOT NULL,"
              " creator INTEGER NOT NULL, attributes INTEGER NOT NULL, version INTEGER NOT NULL, modified INTEGER NOT NULL,"
              " mod_number INTEGER NOT NULL, size INTEGER NOT NULL, sha256 TEXT NOT NULL REFERENCES blob(sha256),"
              " stored_at INTEGER NOT NULL, PRIMARY KEY(device_key, name));"
              "CREATE TABLE IF NOT EXISTS install_queue(id INTEGER PRIMARY KEY, device_key TEXT NOT NULL DEFAULT '',"
              " file_name TEXT NOT NULL, db_name BLOB NOT NULL, target TEXT NOT NULL DEFAULT 'ram',"
              " state TEXT NOT NULL DEFAULT 'queued', message TEXT NOT NULL DEFAULT '',"
              " sha256 TEXT NOT NULL REFERENCES blob(sha256), size INTEGER NOT NULL, queued_at INTEGER NOT NULL,"
              " done_at INTEGER NOT NULL DEFAULT 0);"
              "CREATE TABLE IF NOT EXISTS device_value(device_key TEXT NOT NULL, key TEXT NOT NULL, value TEXT NOT NULL,"
              " PRIMARY KEY(device_key, key));"
              "CREATE TABLE IF NOT EXISTS log(id INTEGER PRIMARY KEY, device_key TEXT NOT NULL, time INTEGER NOT NULL,"
              " text TEXT NOT NULL);"
              "CREATE INDEX IF NOT EXISTS log_device ON log(device_key, time);"
              "PRAGMA user_version=1;",
              error)) {
        Close();
        return false;
    }
    if (version < 2 &&
        !Migrate(db, 2,
              "CREATE TABLE IF NOT EXISTS pim_record(id INTEGER PRIMARY KEY, kind INTEGER NOT NULL,"
              " revision INTEGER NOT NULL, category TEXT NOT NULL, secret INTEGER NOT NULL, archived INTEGER NOT NULL,"
              " deleted INTEGER NOT NULL, payload BLOB NOT NULL);"
              "CREATE INDEX IF NOT EXISTS pim_record_kind ON pim_record(kind,deleted,id);"
              "CREATE TABLE IF NOT EXISTS pim_link(scope TEXT NOT NULL, guest_id INTEGER NOT NULL,"
              " record_id INTEGER NOT NULL REFERENCES pim_record(id), revision INTEGER NOT NULL, guest_hash TEXT NOT NULL,"
              " PRIMARY KEY(scope,guest_id), UNIQUE(scope,record_id));"
              "CREATE TABLE IF NOT EXISTS pim_pending(scope TEXT NOT NULL, record_id INTEGER NOT NULL REFERENCES pim_record(id),"
              " revision INTEGER NOT NULL, guest_hash TEXT NOT NULL, PRIMARY KEY(scope,record_id));"
              "CREATE TABLE IF NOT EXISTS pim_action(device_key TEXT NOT NULL, kind INTEGER NOT NULL,"
              " action INTEGER NOT NULL, next_action INTEGER, PRIMARY KEY(device_key,kind));"
              "PRAGMA user_version=2;",
              error)) {
        Close();
        return false;
    }
    if (version < 3 &&
        !Migrate(db, 3, "ALTER TABLE install_queue RENAME TO install_queue_old;"
                  "CREATE TABLE install_queue(id INTEGER PRIMARY KEY AUTOINCREMENT, device_key TEXT NOT NULL DEFAULT '',"
                  " file_name TEXT NOT NULL, db_name BLOB NOT NULL, target TEXT NOT NULL DEFAULT 'ram',"
                  " state TEXT NOT NULL DEFAULT 'queued', message TEXT NOT NULL DEFAULT '',"
                  " sha256 TEXT NOT NULL REFERENCES blob(sha256), size INTEGER NOT NULL, queued_at INTEGER NOT NULL,"
                  " done_at INTEGER NOT NULL DEFAULT 0);"
                  "INSERT INTO install_queue SELECT * FROM install_queue_old;"
                  "DROP TABLE install_queue_old;"
                  "PRAGMA user_version=3;", error)) {
        Close();
        return false;
    }
    Query q(db, "SELECT value FROM meta WHERE key='pc_id'");
    if (q.Row()) {
        const auto value = q.Text(0);
        const auto end = value.data() + value.size();
        const auto parsed = std::from_chars(value.data(), end, pcId);
        if (parsed.ec != std::errc{} || parsed.ptr != end || !pcId) {
            error = "The profile has an invalid HotSync PC ID.";
            Close();
            return false;
        }
    } else {
        pcId = RandomId();
        Query put(db, "INSERT INTO meta(key,value) VALUES('pc_id',?),('created',?)");
        put.Bind(1, std::to_string(pcId)).Bind(2, std::to_string(NowSeconds()));
        if (!put.Run()) {
            error = LastError(db);
            Close();
            return false;
        }
    }
    return true;
}

bool SyncProfile::PutBlob(const Bytes& data, std::string& sha, std::string& error) {
    sha = Sha256Hex(data.data(), data.size());
    Query q(db, "INSERT OR IGNORE INTO blob(sha256,data) VALUES(?,?)");
    q.Bind(1, sha).BindBlob(2, data);
    if (!q.Run()) return error = LastError(db), false;
    return true;
}

bool SyncProfile::GetBlob(const std::string& sha, Bytes& data) {
    Query q(db, "SELECT data FROM blob WHERE sha256=?");
    q.Bind(1, sha);
    if (!q.Row()) return false;
    data = q.Blob(0);
    return Sha256Hex(data.data(), data.size()) == sha;
}

void SyncProfile::DropUnusedBlobs() {
    std::string ignored;
    Exec(db,
         "DELETE FROM blob WHERE sha256 NOT IN (SELECT sha256 FROM backup) AND sha256 NOT IN (SELECT sha256 FROM install_queue)",
         ignored);
}

std::vector<std::string> SyncProfile::BackupDevices() {
    std::vector<std::string> out;
    Query q(db, "SELECT DISTINCT device_key FROM backup ORDER BY device_key");
    while (q.Row()) out.push_back(q.Text(0));
    return out;
}

std::vector<BackupEntry> SyncProfile::Backups(const std::string& deviceKey) {
    std::vector<BackupEntry> out;
    Query q(db,
            "SELECT device_key,name,type,creator,attributes,version,modified,mod_number,size,sha256,stored_at FROM backup"
            " WHERE device_key=? ORDER BY name");
    q.Bind(1, deviceKey);
    while (q.Row())
        out.push_back({q.Text(0), q.Text(1), uint32_t(q.Int(2)), uint32_t(q.Int(3)), uint16_t(q.Int(4)), uint16_t(q.Int(5)),
                       uint32_t(q.Int(6)), uint32_t(q.Int(7)), q.Int(8), q.Text(9), q.Int(10)});
    return out;
}

std::optional<BackupEntry> SyncProfile::FindBackup(const std::string& deviceKey, const std::string& name) {
    Query q(db,
            "SELECT device_key,name,type,creator,attributes,version,modified,mod_number,size,sha256,stored_at FROM backup"
            " WHERE device_key=? AND name=?");
    q.Bind(1, deviceKey).Bind(2, name);
    if (!q.Row()) return std::nullopt;
    return BackupEntry{q.Text(0), q.Text(1), uint32_t(q.Int(2)), uint32_t(q.Int(3)), uint16_t(q.Int(4)), uint16_t(q.Int(5)),
                       uint32_t(q.Int(6)), uint32_t(q.Int(7)), q.Int(8), q.Text(9), q.Int(10)};
}

bool SyncProfile::StoreBackup(const std::string& deviceKey, const PalmDb& palm, const Bytes& file, std::string& error) {
    std::string sha;
    if (!Exec(db, "BEGIN", error)) return false;
    if (!PutBlob(file, sha, error)) {
        std::string ignored;
        Exec(db, "ROLLBACK", ignored);
        return false;
    }
    Query q(db,
            "INSERT OR REPLACE INTO backup(device_key,name,type,creator,attributes,version,modified,mod_number,size,sha256,stored_at)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    q.Bind(1, deviceKey).Bind(2, palm.name).Bind(3, palm.type).Bind(4, palm.creator).Bind(5, palm.attributes).Bind(6, palm.version)
        .Bind(7, palm.modified).Bind(8, palm.modNumber).Bind(9, int64_t(file.size())).Bind(10, sha).Bind(11, NowSeconds());
    if (q.Run()) {
        DropUnusedBlobs();
        if (Exec(db, "COMMIT", error)) return true;
    } else {
        error = LastError(db);
    }
    std::string ignored;
    Exec(db, "ROLLBACK", ignored);
    return false;
}

bool SyncProfile::StoreRecovery(const std::string& source, const PalmDb& palm, const Bytes& file, std::string& error) {
    if (!Exec(db, "PRAGMA synchronous=FULL", error)) return false;
    const bool stored = StoreBackup(source, palm, file, error);
    std::string ignored;
    Exec(db, "PRAGMA synchronous=NORMAL", ignored);
    return stored;
}

bool SyncProfile::LoadBackup(const std::string& deviceKey, const std::string& name, Bytes& file) {
    auto entry = FindBackup(deviceKey, name);
    return entry && GetBlob(entry->sha256, file);
}

bool SyncProfile::DeleteBackup(const std::string& deviceKey, const std::string& name, std::string& error) {
    Query q(db, "DELETE FROM backup WHERE device_key=? AND name=?");
    q.Bind(1, deviceKey).Bind(2, name);
    if (!q.Run()) return error = LastError(db), false;
    DropUnusedBlobs();
    return true;
}

bool IsCabFileName(const std::string& name) {
    if (name.size() <= 4 || name[name.size() - 4] != '.') return false;
    const auto end = name.size();
    return (name[end - 3] == 'c' || name[end - 3] == 'C') &&
           (name[end - 2] == 'a' || name[end - 2] == 'A') &&
           (name[end - 1] == 'b' || name[end - 1] == 'B');
}

bool SyncProfile::QueueInstall(const std::string& deviceKey, const std::string& fileName, const Bytes& file, InstallItem& out,
                               std::string& error, const std::string& target) {
    if (!Exec(db, "BEGIN IMMEDIATE", error)) return false;
    if (AddInstall(deviceKey, fileName, file, out, error, target)) {
        DropUnusedBlobs();
        if (Exec(db, "COMMIT", error)) return true;
    }
    std::string ignored;
    Exec(db, "ROLLBACK", ignored);
    return false;
}

bool SyncProfile::AddInstall(const std::string& deviceKey, const std::string& fileName, const Bytes& file, InstallItem& out,
                              std::string& error, const std::string& target) {
    std::string name;
    if (target == "cab") {
        if (!IsCabFileName(fileName) || fileName.size() > 255 || fileName.find_first_of("/\\") != std::string::npos ||
            file.size() < 36 || file.size() > 64 * 1024 * 1024 ||
            !std::equal(file.begin(), file.begin() + 4, "MSCF"))
            return error = "Choose a valid Windows Mobile CAB file.", false;
        name = fileName;
    } else {
        if (target != "ram") return error = "Installing to a card isn't done yet.", false;
        PalmDb palm;
        if (!PalmDb::Read(file, palm, error)) return false;
        name = palm.name;
    }
    std::string sha;
    if (!PutBlob(file, sha, error)) return false;
    // A newer copy of the same database for the same device replaces a queued one.
    Query drop(db, "DELETE FROM install_queue WHERE device_key=? AND db_name=? AND target=? AND state='queued'");
    drop.Bind(1, deviceKey).Bind(2, name).Bind(3, target);
    Query q(db, "INSERT INTO install_queue(device_key,file_name,db_name,target,sha256,size,queued_at) VALUES(?,?,?,?,?,?,?)");
    q.Bind(1, deviceKey).Bind(2, fileName).Bind(3, name).Bind(4, target).Bind(5, sha).Bind(6, int64_t(file.size()))
        .Bind(7, NowSeconds());
    if (!drop.Run() || !q.Run()) return error = LastError(db), false;
    out = {sqlite3_last_insert_rowid(db), deviceKey, fileName, name, target, "queued", "", sha, int64_t(file.size()), NowSeconds(), 0};
    return true;
}

int SyncProfile::QueueRestore(const std::string& fromDevice, const std::string& toDevice, std::string& error) {
    error.clear();
    if (!Exec(db, "BEGIN IMMEDIATE", error)) return -1;
    int count = 0;
    for (const auto& entry : Backups(fromDevice)) {
        // Copy-protected databases go back only to the device they came from.
        if ((entry.attributes & dbCopyPrevention) && fromDevice != toDevice &&
            fromDevice != toDevice + " / recovery " + entry.sha256) continue;
        Bytes file;
        InstallItem item;
        if (!GetBlob(entry.sha256, file)) {
            error = "A stored backup of " + entry.name + " is damaged.";
            break;
        }
        const auto name = entry.name + (entry.attributes & dbResource ? ".prc" : ".pdb");
        if (!AddInstall(toDevice, name, file, item, error, "ram")) break;
        ++count;
    }
    if (error.empty()) {
        DropUnusedBlobs();
        if (Exec(db, "COMMIT", error)) return count;
    }
    std::string ignored;
    Exec(db, "ROLLBACK", ignored);
    return -1;
}

namespace {
const char* installColumns =
    "SELECT id,device_key,file_name,db_name,target,state,message,sha256,size,queued_at,done_at FROM install_queue ";
}

std::vector<InstallItem> SyncProfile::InstallQueue(bool pendingOnly) {
    std::vector<InstallItem> out;
    Query q(db, (std::string(installColumns) + (pendingOnly ? "WHERE state='queued' " : "") + "ORDER BY id").c_str());
    while (q.Row())
        out.push_back({q.Int(0), q.Text(1), q.Text(2), q.Text(3), q.Text(4), q.Text(5), q.Text(6), q.Text(7), q.Int(8), q.Int(9), q.Int(10)});
    return out;
}

std::vector<InstallItem> SyncProfile::PendingInstalls(const std::string& deviceKey) {
    std::vector<InstallItem> out;
    Query q(db, (std::string(installColumns) + "WHERE state='queued' AND (device_key=? OR device_key='') ORDER BY id").c_str());
    q.Bind(1, deviceKey);
    while (q.Row())
        out.push_back({q.Int(0), q.Text(1), q.Text(2), q.Text(3), q.Text(4), q.Text(5), q.Text(6), q.Text(7), q.Int(8), q.Int(9), q.Int(10)});
    return out;
}

bool SyncProfile::LoadInstall(int64_t id, Bytes& file) {
    Query q(db, "SELECT sha256 FROM install_queue WHERE id=?");
    q.Bind(1, id);
    return q.Row() && GetBlob(q.Text(0), file);
}

bool SyncProfile::FinishInstall(int64_t id, bool ok, const std::string& message) {
    Query q(db, "UPDATE install_queue SET state=?, message=?, done_at=? WHERE id=? AND state='queued'");
    q.Bind(1, std::string(ok ? "done" : "failed")).Bind(2, message).Bind(3, NowSeconds()).Bind(4, id);
    return q.Run() && sqlite3_changes(db) == 1;
}

bool SyncProfile::RemoveInstall(int64_t id, std::string& error) {
    Query q(db, "DELETE FROM install_queue WHERE id=?");
    q.Bind(1, id);
    if (!q.Run()) return error = LastError(db), false;
    DropUnusedBlobs();
    return true;
}

void SyncProfile::Log(const std::string& deviceKey, const std::string& text) {
    Query q(db, "INSERT INTO log(device_key,time,text) VALUES(?,?,?)");
    q.Bind(1, deviceKey).Bind(2, NowSeconds()).Bind(3, text);
    q.Run();
}

std::vector<SyncLogEntry> SyncProfile::LogEntries(const std::string& deviceKey, int limit) {
    std::vector<SyncLogEntry> out;
    Query q(db, "SELECT device_key,time,text FROM log WHERE device_key=? OR ?='' ORDER BY id DESC LIMIT ?");
    q.Bind(1, deviceKey).Bind(2, deviceKey).Bind(3, limit);
    while (q.Row()) out.push_back({q.Text(0), q.Int(1), q.Text(2)});
    std::reverse(out.begin(), out.end());
    return out;
}

bool SyncProfile::HotSyncUsesNetwork(const std::string& deviceKey) {
    return DeviceValue(deviceKey, "hotsync-transport") == "network";
}

bool SyncProfile::SetHotSyncNetwork(const std::string& deviceKey, bool network, std::string& error) {
    return SetDeviceValue(deviceKey, "hotsync-transport", network ? "network" : "local", &error);
}

std::string ActiveSyncFilesFolder(const std::string& profileFolder, bool create) {
    const auto folder = fs::u8path(profileFolder) / "Files";
    if (create) fs::create_directory(folder);
    if (fs::is_directory(fs::symlink_status(folder))) return folder.u8string();
    if (create) throw std::runtime_error("Couldn't open the profile Files folder.");
    return {};
}

bool SyncProfile::ActiveSyncUsesSerial(const std::string& deviceKey) {
    return DeviceValue(deviceKey, "activesync-transport") == "serial";
}

bool SyncProfile::SetActiveSyncSerial(const std::string& deviceKey, bool serial, std::string& error) {
    return SetDeviceValue(deviceKey, "activesync-transport", serial ? "serial" : "usb", &error);
}

std::string SyncProfile::ActiveSyncPairing(const std::string& deviceKey) {
    return DeviceValue(deviceKey, "activesync-pair");
}

bool SyncProfile::RequestActiveSyncPairing(const std::string& deviceKey, bool request, std::string& error) {
    if (!request && ActiveSyncPairing(deviceKey) != "requested") return true;
    return SetDeviceValue(deviceKey, "activesync-pair", request ? "requested" : "", &error);
}

bool SyncProfile::ConfirmActiveSyncPairing(const std::string& deviceKey, std::string& error) {
    return SetDeviceValue(deviceKey, "activesync-pair", "confirmed", &error);
}

bool SyncProfile::ClearConfirmedActiveSyncPairing(const std::string& deviceKey, std::string& error) {
    Query q(db, "DELETE FROM device_value WHERE device_key=? AND key='activesync-pair' AND value='confirmed'");
    q.Bind(1, deviceKey);
    if (q.Run()) return true;
    error = LastError(db);
    return false;
}

std::string SyncProfile::DeviceValue(const std::string& deviceKey, const std::string& key) {
    Query q(db, "SELECT value FROM device_value WHERE device_key=? AND key=?");
    q.Bind(1, deviceKey).Bind(2, key);
    return q.Row() ? q.Text(0) : std::string();
}

bool SyncProfile::SetDeviceValue(const std::string& deviceKey, const std::string& key, const std::string& value,
                                 std::string* error) {
    Query q(db, "INSERT OR REPLACE INTO device_value(device_key,key,value) VALUES(?,?,?)");
    q.Bind(1, deviceKey).Bind(2, key).Bind(3, value);
    if (q.Run()) return true;
    if (error) *error = LastError(db);
    return false;
}

namespace {

void PutPimPart(Bytes& out, const uint8_t* data, size_t size) {
    for (int shift : {24, 16, 8, 0}) out.push_back(uint8_t(size >> shift));
    if (size) out.insert(out.end(), data, data + size);
}

void PutPimPart(Bytes& out, const std::string& value) {
    PutPimPart(out, reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

// Each entry is a kind byte followed by a length-prefixed key and value.
Bytes PimPayload(const PimRecord& record) {
    Bytes out;
    for (const auto& field : record.fields) {
        out.push_back(0);
        PutPimPart(out, field.first);
        PutPimPart(out, field.second);
    }
    for (const auto& field : record.extras) {
        out.push_back(1);
        PutPimPart(out, field.first);
        PutPimPart(out, field.second.data(), field.second.size());
    }
    return out;
}

bool ReadPimPayload(const Bytes& data, PimRecord& record) {
    size_t pos = 0;
    auto take = [&](Bytes& part) {
        if (data.size() - pos < 4) return false;
        size_t count = 0;
        for (int i = 0; i < 4; ++i) count = (count << 8) | data[pos++];
        if (count > data.size() - pos) return false;
        part.assign(data.begin() + pos, data.begin() + pos + count);
        pos += count;
        return true;
    };
    while (pos < data.size()) {
        const uint8_t kind = data[pos++];
        Bytes key, value;
        if (kind > 1 || !take(key) || !take(value) || key.empty()) return false;
        std::string name(key.begin(), key.end());
        if (kind) record.extras.emplace(std::move(name), std::move(value));
        else record.fields.emplace(std::move(name), std::string(value.begin(), value.end()));
    }
    return true;
}

std::optional<PimRecord> ReadPimRecord(Query& q) {
    const int64_t kind = q.Int(1);
    if (kind < 0 || kind > int(PimKind::memos)) return std::nullopt;
    PimRecord r;
    r.id = q.Int(0);
    r.kind = PimKind(kind);
    r.revision = q.Int(2);
    r.category = q.Text(3);
    r.secret = q.Int(4) != 0;
    r.archived = q.Int(5) != 0;
    r.deleted = q.Int(6) != 0;
    if (!ReadPimPayload(q.Blob(7), r)) return std::nullopt;
    return r;
}

}  // namespace

std::vector<PimRecord> SyncProfile::PimRecords(PimKind kind, bool includeDeleted) {
    std::vector<PimRecord> records;
    std::string error;
    ReadPimRecords(kind, includeDeleted, records, error);
    return records;
}

bool SyncProfile::ReadPimRecords(PimKind kind, bool includeDeleted, std::vector<PimRecord>& records, std::string& error) {
    Query q(db, "SELECT id,kind,revision,category,secret,archived,deleted,payload FROM pim_record"
                " WHERE kind=? AND (deleted=0 OR ?) ORDER BY id");
    q.Bind(1, int(kind)).Bind(2, includeDeleted ? 1 : 0);
    std::vector<PimRecord> result;
    while (q.Row()) {
        auto r = ReadPimRecord(q);
        if (!r) return error = "Invalid record in the sync profile", false;
        result.push_back(std::move(*r));
    }
    if (sqlite3_errcode(db) != SQLITE_DONE) return error = LastError(db), false;
    records = std::move(result);
    return true;
}

std::optional<PimRecord> SyncProfile::FindPimRecord(int64_t id) {
    Query q(db, "SELECT id,kind,revision,category,secret,archived,deleted,payload FROM pim_record WHERE id=?");
    q.Bind(1, id);
    return q.Row() ? ReadPimRecord(q) : std::nullopt;
}

bool SyncProfile::SavePimRecord(PimRecord& r, std::string& error) {
    const bool existing = r.id > 0;
    Query q(db, existing
        ? "UPDATE pim_record SET category=?,secret=?,archived=?,deleted=?,payload=?,revision=revision+1"
          " WHERE kind=? AND id=? AND revision=? RETURNING id,revision"
        : "INSERT INTO pim_record(category,secret,archived,deleted,payload,kind,revision)"
          " VALUES(?,?,?,?,?,?,1) RETURNING id,revision");
    q.Bind(1, r.category).Bind(2, r.secret ? 1 : 0).Bind(3, r.archived ? 1 : 0)
        .Bind(4, r.deleted ? 1 : 0).BindBlob(5, PimPayload(r)).Bind(6, int(r.kind));
    if (existing) q.Bind(7, r.id).Bind(8, r.revision);
    if (!q.Row()) {
        error = existing && sqlite3_errcode(db) == SQLITE_DONE ? "The record changed. Open it again." : LastError(db);
        return false;
    }
    const int64_t id = q.Int(0), revision = q.Int(1);
    if (!q.Run()) return error = LastError(db), false;
    r.id = id;
    r.revision = revision;
    return true;
}

bool SyncProfile::DeletePimRecord(int64_t id, int64_t revision, std::string& error) {
    Query q(db, "UPDATE pim_record SET deleted=1, revision=revision+1 WHERE id=? AND revision=? AND deleted=0");
    q.Bind(1, id).Bind(2, revision);
    if (!q.Run()) return error = LastError(db), false;
    if (sqlite3_changes(db) == 0) return error = "The record changed. Open it again.", false;
    return true;
}

PimAction SyncProfile::GetPimAction(const std::string& deviceKey, PimKind kind) {
    Query q(db, "SELECT coalesce(next_action,action) FROM pim_action WHERE device_key=? AND kind=?");
    q.Bind(1, deviceKey).Bind(2, int(kind));
    if (!q.Row() || q.Int(0) < 0 || q.Int(0) > int(PimAction::skip)) return PimAction::synchronize;
    return PimAction(q.Int(0));
}

bool SyncProfile::SetPimAction(const std::string& deviceKey, PimKind kind, PimAction action, std::string& error) {
    const bool once = action == PimAction::desktopOverwrites || action == PimAction::handheldOverwrites;
    Query q(db, once ?
        "INSERT INTO pim_action(device_key,kind,action,next_action) VALUES(?,?,0,?)"
        " ON CONFLICT(device_key,kind) DO UPDATE SET next_action=excluded.next_action" :
        "INSERT INTO pim_action(device_key,kind,action) VALUES(?,?,?)"
        " ON CONFLICT(device_key,kind) DO UPDATE SET action=excluded.action,next_action=NULL");
    q.Bind(1, deviceKey).Bind(2, int(kind)).Bind(3, int(action));
    if (!q.Run()) return error = LastError(db), false;
    return true;
}

bool SyncProfile::FinishPimAction(const std::string& deviceKey, PimKind kind, PimAction action, std::string& error) {
    if (action != PimAction::desktopOverwrites && action != PimAction::handheldOverwrites) return true;
    Query q(db, "UPDATE pim_action SET next_action=NULL WHERE device_key=? AND kind=? AND next_action=?");
    q.Bind(1, deviceKey).Bind(2, int(kind)).Bind(3, int(action));
    if (!q.Run()) return error = LastError(db), false;
    return true;
}

std::vector<PimLink> SyncProfile::PimLinks(const std::string& scope, std::string& error) {
    Query q(db, "SELECT guest_id,record_id,revision,guest_hash FROM pim_link WHERE scope=?");
    q.Bind(1, scope);
    std::vector<PimLink> links;
    while (q.Row()) links.push_back({uint32_t(q.Int(0)), q.Int(1), q.Int(2), q.Text(3)});
    if (sqlite3_errcode(db) != SQLITE_DONE) error = LastError(db);
    return links;
}

bool SyncProfile::SavePimLink(const std::string& scope, const PimLink& link, std::string& error) {
    Query q(db, "INSERT INTO pim_link(scope,guest_id,record_id,revision,guest_hash) VALUES(?,?,?,?,?)"
                " ON CONFLICT(scope,guest_id) DO UPDATE SET record_id=excluded.record_id,revision=excluded.revision,"
                " guest_hash=excluded.guest_hash");
    q.Bind(1, scope).Bind(2, link.guestId).Bind(3, link.recordId).Bind(4, link.revision).Bind(5, link.guestHash);
    if (!q.Run()) return error = LastError(db), false;
    return true;
}

bool SyncProfile::RemovePimLink(const std::string& scope, uint32_t guestId, std::string& error) {
    Query q(db, "DELETE FROM pim_link WHERE scope=? AND guest_id=?");
    q.Bind(1, scope).Bind(2, guestId);
    if (!q.Run()) return error = LastError(db), false;
    return true;
}

std::vector<PimPending> SyncProfile::PendingPimWrites(const std::string& scope, std::string& error) {
    Query q(db, "SELECT record_id,revision,guest_hash FROM pim_pending WHERE scope=?");
    q.Bind(1, scope);
    std::vector<PimPending> pending;
    while (q.Row()) pending.push_back({q.Int(0), q.Int(1), q.Text(2)});
    if (sqlite3_errcode(db) != SQLITE_DONE) error = LastError(db);
    return pending;
}

bool SyncProfile::SetPendingPimWrite(const std::string& scope, const PimPending& pending, std::string& error) {
    Query q(db, "INSERT INTO pim_pending(scope,record_id,revision,guest_hash) VALUES(?,?,?,?)"
                " ON CONFLICT(scope,record_id) DO UPDATE SET revision=excluded.revision,guest_hash=excluded.guest_hash");
    q.Bind(1, scope).Bind(2, pending.recordId).Bind(3, pending.revision).Bind(4, pending.guestHash);
    if (!q.Run()) return error = LastError(db), false;
    return true;
}

bool SyncProfile::ClearPendingPimWrite(const std::string& scope, int64_t id, std::string& error) {
    Query q(db, "DELETE FROM pim_pending WHERE scope=? AND record_id=?");
    q.Bind(1, scope).Bind(2, id);
    if (!q.Run()) return error = LastError(db), false;
    return true;
}

bool SyncProfile::BeginPimChange(std::string& error) { return Exec(db, "BEGIN IMMEDIATE", error); }
bool SyncProfile::CommitPimChange(std::string& error) { return Exec(db, "COMMIT", error); }
void SyncProfile::RollBackPimChange() {
    std::string ignored;
    Exec(db, "ROLLBACK", ignored);
}

}  // namespace frondSync
