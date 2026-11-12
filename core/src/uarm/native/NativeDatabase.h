#ifndef FROND_NATIVE_DATABASE_H
#define FROND_NATIVE_DATABASE_H

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "CPU.h"
#include "Defer.h"
#include "app_launcher.h"
#include "db_installer.h"
#include "db_archive_name.h"
#include "db_list.h"
#include "palmos_errors.h"
#include "zip.h"

// Native Palm database operations. Session owns a verified UI-task call context;
// the caller owns a guest transaction spanning construction and destruction.
// Session::Call restores the stopped CPU after every synchronous native call.
template<class Session>
class NativeDatabase {
 public:
    explicit NativeDatabase(Session& session) : session(session) {
        scratch = session.DataAddress();
        if (!scratch || (scratch & 3) || session.DataSize() < 256)
            throw std::runtime_error("Palm scratch allocation failed");
        uint8_t zero[256]{};
        write(scratch, zero, sizeof(zero));
    }

    // The call session owns this scratch allocation and releases it on Close().
    void Finish() { scratch = 0; }

    uint32_t Find(const std::string& name) {
        setName(name);
        return call(0x815c, {scratch + 64, 0, 0, 0});
    }

    bool Launch(const std::string& name) {
        const uint32_t database = Find(name);
        return database && call(0x88f8, {database, 0, 0, 0}) == 0;
    }

    // DmDeleteDatabase on a RAM database by name. The running application is checked first.
    // application, when given, tells whether the database was an application.
    DeleteDbResult Delete(const std::string& name, bool* application = nullptr) {
        if (application) *application = false;
        const uint32_t database = Find(name);
        if (!database) return DeleteDbResult::missing;
        if (!(database & 0x80000000u)) return DeleteDbResult::rom;
        uint32_t current = 0;
        write(scratch + 96, &current, 4);
        if (call(0x8800, {scratch + 96, 0, 0, 0}) == 0) read(scratch + 96, &current, 4);
        if (current == database) return DeleteDbResult::running;
        uint32_t type = 0;
        write(scratch + 100, &type, 4);
        if (session.Call(0x812c, {database, 0, 0, 0}, {0, 0, 0, 0, 0, 0, scratch + 100, 0}) == 0) read(scratch + 100, &type, 4);
        const uint32_t error = call(0x8148, {database, 0, 0, 0});
        if (error == 0) {
            if (application) *application = type == applicationType;
            return DeleteDbResult::deleted;
        }
        return error == dmErrDatabaseOpen ? DeleteDbResult::open : DeleteDbResult::failed;
    }

    int Install(const uint8_t* bytes, size_t length) {
        if (!bytes || length < 78 || length > maxDatabaseBytes) return DB_INSTALL_RESULT_ERR_CORRUPT;
        size_t offset = 0;
        bool callbackFailed = false, deleteFailed = false;
        unsigned callbacks = 0;
        session.RegisterCallback(scratch + 32, [&](ArmCpu* cpu) {
            auto* r = cpuGetRegisters(cpu);
            uint32_t requested = 0;
            if (++callbacks > maxCallbacks || !session.Read(r[1], &requested, 4)) {
                callbackFailed = true; r[0] = 1; return;
            }
            const uint32_t count = uint32_t(std::min<size_t>({requested, length - offset, maxChunk}));
            if (!session.Write(r[0], bytes + offset, count) || !session.Write(r[1], &count, 4)) {
                callbackFailed = true; r[0] = 1; return;
            }
            offset += count; r[0] = 0;
        });
        session.RegisterCallback(scratch + 36, [&](ArmCpu* cpu) {
            auto* r = cpuGetRegisters(cpu);
            if (++callbacks > maxCallbacks) { deleteFailed = true; r[0] = 0; return; }
            const uint32_t database = r[2];
            const bool okay = call(0x8148, {database, 0, 0, 0}) == 0;
            deleteFailed |= !okay;
            cpuGetRegisters(cpu)[0] = okay;
        });
        Defer unregister([&] { session.UnregisterCallback(scratch + 32); session.UnregisterCallback(scratch + 36); });
        uint32_t zero = 0;
        write(scratch + 48, &zero, 4);
        write(scratch + 52, &zero, 4);
        const uint32_t error = session.Call(0x82fc, {scratch + 32, scratch + 36, 0, scratch + 48}, {scratch + 52, 1});
        uint8_t reset = 0;
        read(scratch + 52, &reset, 1);
        if (deleteFailed) return DB_INSTALL_RESULT_ERR_COULD_NOT_OVERWRITE;
        if (callbackFailed) return DB_INSTALL_RESULT_ERR_UNKNOWN;
        switch (error) {
            case 0: return reset ? DB_INSTALL_RESULT_NEEDS_RESET : DB_INSTALL_RESULT_OK;
            case dmErrMemError: return DB_INSTALL_RESULT_ERR_OOM;
            case dmErrCorruptDatabase: return DB_INSTALL_RESULT_ERR_CORRUPT;
            case dmErrDatabaseOpen: return DB_INSTALL_RESULT_ERR_IS_OPEN;
            default: return DB_INSTALL_RESULT_ERR_UNKNOWN;
        }
    }

    // What Palm OS gets after a HotSync install, for the database just installed from bytes: the
    // app with its creator gets sysAppLaunchCmdSyncNotify unless it's the app in front, and an app
    // install while the launcher is in front relaunches the launcher so it lists the app, unless an
    // event is waiting: the notified app may have queued one (an app switch queues appStopEvent),
    // or a pen or key event came in. Native calls take no card number; SysEventAvail is the 68k
    // EvtEventAvail.
    void FinishInstall(const uint8_t* bytes, size_t length) {
        if (!bytes || length < 78) return;
        const auto* text = reinterpret_cast<const char*>(bytes);
        const uint32_t installed = Find(std::string(text, strnlen(text, 31)));
        if (!installed) return;
        const uint32_t type = be32(bytes + 60), creator = be32(bytes + 64);
        // Current app +96, found app +100, launch result +104, search state +128.
        uint8_t zero[128]{};
        write(scratch + 96, zero, sizeof(zero));
        uint32_t current = 0, currentCreator = 0;
        if (call(0x8800, {scratch + 96, 0, 0, 0}) == 0) read(scratch + 96, &current, 4);
        if (current) {
            write(scratch + 48, zero, 4);
            if (session.Call(0x812c, {current, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, scratch + 48}) == 0)
                read(scratch + 48, &currentCreator, 4);
        }
        uint32_t app = 0;
        if (type == applicationType) app = installed;
        else if (session.Call(0x8198, {1, scratch + 128, applicationType, creator}, {1, scratch + 100}) == 0)
            read(scratch + 100, &app, 4);
        if (app && app != current) {
            const uint32_t error = session.Call(0x87c8, {app, 0, syncNotify, 0}, {scratch + 104});
            if (error) std::fprintf(stderr, "install: sync notify failed: Palm OS error %u\n", error);
        }
        if (type != applicationType || currentCreator != launcherCreator) return;
        if (call(0x881c, {}) & 0xff || call(0x82e0, {1, 0, 0, 0}) & 0xff) return;
        if (call(0x88f8, {current, 0, 0, 0}) != 0) throw std::runtime_error("Could not relaunch the launcher");
    }

    // Relaunches the launcher when it is in front, unless an event is waiting, so it drops a deleted
    // application's icon.
    void RelaunchLauncher() {
        uint8_t zero[16]{};
        write(scratch + 96, zero, sizeof(zero));
        uint32_t current = 0, currentCreator = 0;
        if (call(0x8800, {scratch + 96, 0, 0, 0}) == 0) read(scratch + 96, &current, 4);
        if (!current) return;
        if (session.Call(0x812c, {current, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, scratch + 48}) != 0) return;
        read(scratch + 48, &currentCreator, 4);
        if (currentCreator != launcherCreator || call(0x881c, {}) & 0xff || call(0x82e0, {1, 0, 0, 0}) & 0xff) return;
        if (call(0x88f8, {current, 0, 0, 0}) != 0) throw std::runtime_error("Could not relaunch the launcher");
    }

    // Name, attributes, type and creator of each RAM database, read from the database directory
    // without opening any. Skips a68k like Export.
    DbMetadataList List() {
        const uint32_t count = call(0x81d4, {});
        if (count > 65535) throw std::runtime_error("Invalid Palm database count");
        DbMetadataList list;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t db = call(0x8188, {i, 0, 0, 0});
            if (!db) throw std::runtime_error("Could not enumerate Palm databases");
            if (!(db & 0x80000000u)) continue;
            // Native order at scratch: creator +48, type +52, attributes +56, name +64.
            uint8_t zero[48]{};
            write(scratch + 48, zero, sizeof(zero));
            if (session.Call(0x812c, {db, scratch + 64, scratch + 56, 0}, {0, 0, 0, 0, 0, 0, scratch + 52, scratch + 48}))
                throw std::runtime_error("Could not read Palm database metadata");
            DbMetadata meta{};
            uint16_t attributes = 0;
            read(scratch + 48, &meta.creator, 4);
            read(scratch + 52, &meta.type, 4);
            read(scratch + 56, &attributes, 2);
            read(scratch + 64, meta.name, sizeof(meta.name));
            meta.name[sizeof(meta.name) - 1] = 0;
            if (meta.creator == 0x6136386bu || !meta.name[0]) continue;
            meta.attr = attributes;
            meta.index = uint16_t(i);
            meta.localID = db;
            list.push_back(meta);
        }
        return list;
    }

    std::vector<uint8_t> Export(const std::string& name) {
        std::vector<uint32_t> databases;
        if (!name.empty()) {
            const uint32_t db = Find(name);
            if (!db || !(db & 0x80000000u)) throw std::runtime_error("RAM database not found");
            databases.push_back(db);
        } else {
            const uint32_t count = call(0x81d4, {});
            if (count > 65535) throw std::runtime_error("Invalid Palm database count");
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t db = call(0x8188, {i, 0, 0, 0});
                if (!db) throw std::runtime_error("Could not enumerate Palm databases");
                if (db & 0x80000000u) databases.push_back(db);
            }
        }
        zip_t* zip = zip_stream_open(nullptr, 0, 1, 'w');
        if (!zip) throw std::runtime_error("Could not allocate database archive");
        Defer close([&] { zip_stream_close(zip); });
        size_t total = 0;
        for (const uint32_t db : databases) {
            uint32_t creator = 0;
            write(scratch + 48, &creator, sizeof(creator));
            if (session.Call(0x812c, {db, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, scratch + 48}))
                throw std::runtime_error("Could not read Palm database metadata");
            read(scratch + 48, &creator, sizeof(creator));
            if (creator == 0x6136386bu) continue;
            auto pdb = exportOne(db);
            total += pdb.size();
            if (total > maxArchiveBytes) throw std::runtime_error("Database archive exceeds limit");
            std::string entry = entryName(pdb);
            if (zip_entry_open(zip, entry.c_str())) throw std::runtime_error("Could not open archive entry");
            const bool written = zip_entry_write(zip, pdb.data(), pdb.size()) == 0;
            const bool closed = zip_entry_close(zip) == 0;
            if (!written || !closed) throw std::runtime_error("Could not write archive entry");
        }
        void* output = nullptr; ssize_t size = 0;
        if (zip_stream_copy(zip, &output, &size) < 0 || !output || !size)
            throw std::runtime_error("Could not finish database archive");
        Defer freeOutput([&] { free(output); });
        const auto* data = static_cast<const uint8_t*>(output);
        return {data, data + size};
    }

 private:
    static constexpr uint32_t applicationType = 0x6170706cu, launcherCreator = 0x6c6e6368u, syncNotify = 3;
    static uint32_t be32(const uint8_t* p) {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    static constexpr size_t maxDatabaseBytes = 64 * 1024 * 1024, maxArchiveBytes = 256 * 1024 * 1024;
    static constexpr unsigned maxCallbacks = 65536, maxChunk = 65536;
    Session& session;
    uint32_t scratch{0};
    uint32_t call(uint16_t trap, std::array<uint32_t, 4> args) { return session.Call(trap, args, {}); }
    void read(uint32_t address, void* data, size_t size) {
        if (!session.Read(address, data, size)) throw std::runtime_error("Palm memory read failed");
    }
    void write(uint32_t address, const void* data, size_t size) {
        if (!session.Write(address, data, size)) throw std::runtime_error("Palm memory write failed");
    }
    void setName(const std::string& name) {
        if (name.empty() || name.size() > 31 || name.find('\0') != std::string::npos)
            throw std::runtime_error("Invalid Palm database name");
        write(scratch + 64, name.c_str(), name.size() + 1);
    }
    std::vector<uint8_t> exportOne(uint32_t database) {
        std::vector<uint8_t> bytes;
        bool failed = false; unsigned callbacks = 0;
        session.RegisterCallback(scratch + 32, [&](ArmCpu* cpu) {
            auto* r = cpuGetRegisters(cpu); uint32_t size = 0;
            if (++callbacks > maxCallbacks || !session.Read(r[1], &size, 4) ||
                size > maxDatabaseBytes - bytes.size()) { failed = true; r[0] = 1; return; }
            if (uint64_t(r[0]) + size > 0x100000000ull) { failed = true; r[0] = 1; return; }
            const size_t previous = bytes.size(); bytes.resize(previous + size);
            for (uint32_t done = 0; done < size;) {
                const uint32_t chunk = std::min<uint32_t>(maxChunk, size - done);
                if (!session.Read(r[0] + done, bytes.data() + previous + done, chunk)) {
                    failed = true; r[0] = 1; return;
                }
                done += chunk;
            }
            r[0] = 0;
        });
        Defer unregister([&] { session.UnregisterCallback(scratch + 32); });
        const uint8_t zero = 0; write(scratch + 64, &zero, 1);
        const uint32_t error = call(0x8304, {scratch + 32, 0, scratch + 64, database});
        if (error || failed || bytes.size() < 78) throw std::runtime_error("Palm database export failed");
        return bytes;
    }
    static std::string entryName(const std::vector<uint8_t>& pdb) {
        const auto end = std::find(pdb.begin(), pdb.begin() + 32, 0);
        if (end == pdb.begin() || end == pdb.begin() + 32) throw std::runtime_error("Invalid exported database name");
        const std::string name(pdb.begin(), end);
        const uint16_t attributes = (uint16_t(pdb[32]) << 8) | pdb[33];
        const uint32_t creator = (uint32_t(pdb[64]) << 24) | (uint32_t(pdb[65]) << 16) |
                                 (uint32_t(pdb[66]) << 8) | pdb[67];
        return dbArchiveName(name, attributes, creator, false);
    }
};
#endif
