#include "PalmConduit.h"

#if !defined(__ANDROID__)

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

#include "../../../../installer-shims/PalmConduitProtocol.h"
#include "PalmConduitProcess.h"
#include "PalmConduitStorage.h"
#ifdef _WIN32
#include "SyncStore.h"
#endif

namespace frondSync {

namespace {

using frondConduit::Operation;
using frondConduit::Reader;
using frondConduit::Status;
using frondConduit::Writer;

Status StatusFor(int error) {
    switch (error) {
        case dlpErrNone: return frondConduit::success;
        case dlpErrSystem: return frondConduit::remoteSystem;
        case dlpErrMemory: return frondConduit::remoteMemory;
        case dlpErrParam: return frondConduit::badRemoteArgument;
        case dlpErrNotFound: return frondConduit::notFound;
        case dlpErrNoneOpen: return frondConduit::notOpen;
        case dlpErrAlreadyOpen:
        case dlpErrOpen: return frondConduit::alreadyOpen;
        case dlpErrTooManyOpen: return frondConduit::tooManyOpen;
        case dlpErrAlreadyExists: return frondConduit::exists;
        case dlpErrDeleted: return frondConduit::recordDeleted;
        case dlpErrBusy: return frondConduit::recordBusy;
        case dlpErrIllegalReq:
        case dlpErrNotSupp: return frondConduit::unsupported;
        case dlpErrReadOnly: return frondConduit::readOnly;
        case dlpErrSpace: return frondConduit::remoteSpace;
        case dlpErrLimit: return frondConduit::limit;
        case dlpErrSize: return frondConduit::smallBuffer;
        default: return error < 0 ? frondConduit::disconnected : frondConduit::unknown;
    }
}

Reader Payload(const Bytes& request) { return Reader{request, 1}; }

uint32_t Number(Reader& read, uint32_t max) {
    const auto value = read.Get();
    read.ok &= value <= max;
    return value;
}

std::string DbName(Reader& read) {
    auto name = read.Text(31);
    read.ok &= !name.empty();
    return name;
}

std::string Hex(uint32_t number) {
    char bytes[9];
    std::snprintf(bytes, sizeof(bytes), "%08x", number);
    return bytes;
}

void PutDb(Writer& out, const DlpFindInfo& found) {
    const auto& db = found.db;
    out.Put(found.card);
    out.Put(db.attributes);
    out.Put(db.type);
    out.Text(db.name);
    out.Put(db.creator);
    out.Put(db.version);
    out.Put(db.modNumber);
    out.Put(db.index);
    out.Put(db.created);
    out.Put(db.modified);
    out.Put(db.backedUp);
    out.Put(db.misc);
    out.Put(found.records);
    out.Put(found.totalBytes);
    out.Put(found.dataBytes);
    out.Put(found.localId);
    out.Put(found.openRef);
}

struct Runner {
    DlpClient& dlp;
    const UserInfo& user;
    const ConduitAsset& saveRecovery;
    const ConduitLog& log;
    std::map<uint8_t, bool> open;
    std::set<std::pair<uint8_t, std::string>> saved;
    bool registered{};
    bool completed{};
    uint32_t exitStatus{};

    bool ReadyToOverwrite(uint8_t card, const std::string& name) {
        if (saved.count({card, name})) return true;
        std::string problem;
        if (!saveRecovery(name, card, problem)) {
            log("Could not make the recovery copy for " + name + ": " + problem);
            return false;
        }
        saved.emplace(card, name);
        return true;
    }

    Status Handle(const Bytes& request, Writer& reply) {
        if (request.empty() || completed) return frondConduit::badArgument;
        auto read = Payload(request);
        const auto operation = Operation(request[0]);
        switch (operation) {
            case Operation::complete: {
                const auto result = read.Get();
                if (!read.End()) return frondConduit::badArgument;
                exitStatus = result;
                completed = true;
                return frondConduit::success;
            }
            case Operation::check:
                return read.End() ? frondConduit::success : frondConduit::badArgument;
            case Operation::registerConduit: {
                if (!read.End()) return frondConduit::badArgument;
                if (registered) return frondConduit::alreadyOpen;
                registered = true;
                return frondConduit::success;
            }
            case Operation::unregisterConduit: {
                if (!read.End()) return frondConduit::badArgument;
                if (!registered) return frondConduit::disconnected;
                if (!open.empty()) return frondConduit::alreadyOpen;
                registered = false;
                return frondConduit::success;
            }
            case Operation::log:
            case Operation::deviceLog: {
                const auto message = read.Text(1024);
                if (!read.End()) return frondConduit::badArgument;
                if (message.empty()) return frondConduit::success;
                if (operation == Operation::deviceLog) return StatusFor(dlp.AddSyncLogEntry(message));
                log(message);
                return frondConduit::success;
            }
            default: break;
        }
        switch (operation) {
            case Operation::user:
                if (!read.End()) return frondConduit::badArgument;
                reply.Text(user.name.substr(0, 63));
                reply.Put(user.lastSync);
                reply.Put(user.lastSyncPc);
                reply.Put(user.userId);
                reply.Put(user.viewerId);
                return frondConduit::success;
            case Operation::open: {
                const auto card = uint8_t(Number(read, 255));
                const auto mode = uint8_t(Number(read, 0xf0));
                const auto name = DbName(read);
                if (!read.End() || mode & 0x0f) return frondConduit::badArgument;
                if ((mode & 0x40) && !ReadyToOverwrite(card, name)) return frondConduit::readOnly;
                uint8_t handle = 0;
                const auto rc = dlp.OpenDB(card, mode, name, handle);
                if (!rc) {
                    open.emplace(handle, (mode & 0x40) != 0);
                    reply.Put(handle);
                }
                return StatusFor(rc);
            }
            case Operation::create: {
                const auto card = uint8_t(Number(read, 255));
                const auto creator = read.Get();
                const auto type = read.Get();
                const auto flags = uint16_t(Number(read, 65535));
                const auto version = uint16_t(Number(read, 65535));
                const auto name = DbName(read);
                if (!read.End()) return frondConduit::badArgument;
                uint8_t handle = 0;
                const auto rc = dlp.CreateDB(creator, type, card, flags, version, name, handle);
                if (!rc) {
                    open.emplace(handle, true);
                    reply.Put(handle);
                }
                return StatusFor(rc);
            }
            case Operation::removeDb: {
                const auto card = uint8_t(Number(read, 255));
                const auto name = DbName(read);
                if (!read.End()) return frondConduit::badArgument;
                if (!ReadyToOverwrite(card, name)) return frondConduit::readOnly;
                return StatusFor(dlp.DeleteDB(card, name));
            }
            case Operation::findName:
            case Operation::findTypeCreator: {
                const auto options = uint8_t(Number(read, 255));
                const auto cardOrSearch = uint8_t(Number(read, 255));
                DlpFindInfo db;
                int rc;
                if (operation == Operation::findName) {
                    const auto name = DbName(read);
                    if (!read.End()) return frondConduit::badArgument;
                    rc = dlp.FindByName(options, cardOrSearch, name, db);
                } else {
                    const auto type = read.Get();
                    const auto creator = read.Get();
                    if (!read.End()) return frondConduit::badArgument;
                    rc = dlp.FindByTypeCreator(options, cardOrSearch, type, creator, db);
                }
                if (!rc) PutDb(reply, db);
                return StatusFor(rc);
            }
            case Operation::readPreference:
            case Operation::writePreference: {
                const auto creator = read.Get();
                const auto id = uint16_t(Number(read, 65535));
                const auto value = uint16_t(Number(read, 65535));
                const bool backedUp = Number(read, 1) != 0;
                if (operation == Operation::writePreference) {
                    const auto data = read.Blob(frondConduit::maxRecord);
                    if (!read.End()) return frondConduit::badArgument;
                    const char* preferences = backedUp ? "Saved Preferences" : "Unsaved Preferences";
                    if (!ReadyToOverwrite(0, preferences)) return frondConduit::readOnly;
                    return StatusFor(dlp.WriteAppPreference(creator, id, value, backedUp, data));
                }
                if (!read.End()) return frondConduit::badArgument;
                uint16_t version = 0, size = 0;
                Bytes data;
                const auto rc = dlp.ReadAppPreference(creator, id, value, backedUp, version, size, data);
                if (!rc) {
                    reply.Put(version);
                    reply.Put(size);
                    reply.Blob(data.data(), data.size());
                }
                return StatusFor(rc);
            }
            case Operation::readFeature: {
                const auto creator = read.Get();
                const auto number = uint16_t(Number(read, 65535));
                if (!read.End()) return frondConduit::badArgument;
                uint32_t value = 0;
                const auto rc = dlp.ReadFeature(creator, number, value);
                if (!rc) reply.Put(value);
                return StatusFor(rc);
            }
            case Operation::dateTime: {
                if (!read.End()) return frondConduit::badArgument;
                uint32_t value = 0;
                const auto rc = dlp.GetSysDateTime(value);
                if (!rc) reply.Put(value);
                return StatusFor(rc);
            }
            case Operation::cardInfo: {
                const auto card = uint8_t(Number(read, 255));
                if (!read.End()) return frondConduit::badArgument;
                DlpCardInfo value;
                const auto rc = dlp.ReadCardInfo(card, value);
                if (!rc) {
                    reply.Put(value.version);
                    reply.Put(value.created);
                    reply.Put(value.romSize);
                    reply.Put(value.ramSize);
                    reply.Put(value.freeRam);
                    reply.Text(value.name.substr(0, 31));
                    reply.Text(value.manufacturer.substr(0, 31));
                    reply.Put(value.romDbs);
                    reply.Put(value.ramDbs);
                }
                return StatusFor(rc);
            }
            default: return HandleDb(operation, read, reply);
        }
    }

    Status HandleDb(Operation operation, Reader& read, Writer& reply) {
        const auto handle = uint8_t(Number(read, 255));
        if (!read.ok) return frondConduit::badArgument;
        const auto entry = open.find(handle);
        if (entry == open.end()) return frondConduit::notOpen;
        const bool writable = entry->second;
        switch (operation) {
            case Operation::readIndex:
            case Operation::readId:
            case Operation::readModified: {
                const bool id = operation == Operation::readId;
                const bool modified = operation == Operation::readModified;
                const auto key = modified ? 0 : Number(read, id ? 0xffffff : 65535);
                if (!read.End()) return frondConduit::badArgument;
                DlpRecord record;
                uint16_t index = 0;
                const auto rc = dlp.ReadConduitRecord(handle, key, id, modified, index, record);
                if (!rc) {
                    reply.Put(record.uid);
                    reply.Put(index);
                    reply.Put(record.flags);
                    reply.Put(record.category);
                    reply.Blob(record.data.data(), record.data.size());
                }
                return StatusFor(rc);
            }
            case Operation::write: {
                if (!writable) return frondConduit::readOnly;
                DlpRecord record;
                record.uid = Number(read, 0xffffff);
                record.flags = uint8_t(Number(read, 255));
                record.category = uint8_t(Number(read, 15));
                record.data = read.Blob(frondConduit::maxRecord);
                if (!read.End()) return frondConduit::badArgument;
                uint32_t uid = 0;
                const auto rc = dlp.WriteConduitRecord(handle, record, uid);
                if (!rc) reply.Put(uid);
                return StatusFor(rc);
            }
            case Operation::writeResource: {
                if (!writable) return frondConduit::readOnly;
                PalmRecord resource;
                resource.type = read.Get();
                resource.id = uint16_t(Number(read, 65535));
                resource.data = read.Blob(frondConduit::maxRecord);
                if (!read.End()) return frondConduit::badArgument;
                if (resource.data.size() > frondConduit::maxRecord - 10) return frondConduit::limit;
                return StatusFor(dlp.WriteResource(handle, resource));
            }
            case Operation::remove: {
                if (!writable) return frondConduit::readOnly;
                const auto uid = Number(read, 0xffffff);
                return read.End() && uid ? StatusFor(dlp.DeleteRecord(handle, uid)) : frondConduit::badArgument;
            }
            case Operation::writeAppBlock: {
                if (!writable) return frondConduit::readOnly;
                const auto data = read.Blob(frondConduit::maxRecord);
                return read.End() ? StatusFor(dlp.WriteAppBlock(handle, data)) : frondConduit::badArgument;
            }
            default: break;
        }
        if (!read.End()) return frondConduit::badArgument;
        switch (operation) {
            case Operation::close: {
                const auto rc = dlp.CloseDB(handle);
                if (!rc) open.erase(handle);
                return StatusFor(rc);
            }
            case Operation::count: {
                OpenDbInfo value;
                const auto rc = dlp.ReadOpenDBInfo(handle, value);
                if (!rc) reply.Put(value.records);
                return StatusFor(rc);
            }
            case Operation::readAppBlock: {
                Bytes data;
                const auto rc = dlp.ReadAppBlock(handle, data);
                if (!rc && data.size() > frondConduit::maxRecord) return frondConduit::smallBuffer;
                if (!rc) reply.Blob(data.data(), data.size());
                return StatusFor(rc);
            }
            case Operation::purgeAll: return writable ? StatusFor(dlp.PurgeRecords(handle)) : frondConduit::readOnly;
            case Operation::purgeDeleted:
                return writable ? StatusFor(dlp.CleanUpDatabase(handle)) : frondConduit::readOnly;
            case Operation::resetFlags:
                return writable ? StatusFor(dlp.ResetSyncFlags(handle)) : frondConduit::readOnly;
            default: return frondConduit::unsupported;
        }
    }

    bool Serve(ConduitProcess& child, std::string& error) {
        // Some conduits use the active HotSync session without acquiring a separate SDK token.
        const int start = dlp.OpenConduit();
        if (start != dlpErrNone) {
            error = start < 0 ? dlp.LinkError() : DlpErrorText(uint16_t(start));
            return false;
        }
        Bytes frame;
        bool acknowledged = false;
        while (!completed &&
                frondConduit::ReadFrame([&](void* data, size_t size) { return child.Read(data, size); }, frame)) {
            Writer payload;
            const auto result = Handle(frame, payload);
            Writer reply;
            reply.Put(result);
            if (result == frondConduit::success)
                reply.data.insert(reply.data.end(), payload.data.begin(), payload.data.end());
            if (!frondConduit::WriteFrame([&](const void* data, size_t size) { return child.Write(data, size); }, reply.data))
                break;
            acknowledged = completed;
        }
        bool ok = acknowledged && child.Finished() && exitStatus == 0;
        if (child.Stopping()) {
            error = "The conduit stopped or timed out.";
            return false;
        }
        if (!dlp.LinkError().empty()) {
            error = dlp.LinkError();
            return false;
        }
        for (const auto& handle : open) ok &= dlp.CloseDB(handle.first) == dlpErrNone;
        if (ok) return true;
        if (exitStatus) error = "The conduit returned 0x" + Hex(exitStatus) + ".";
        else error = "The conduit process did not finish normally.";
        return false;
    }
};

struct Registration {
    uint32_t creator{};
    uint32_t priority{2};
    std::map<std::string, std::string> settings;
    std::string Value(const std::string& key) const {
        const auto found = settings.find(key);
        return found != settings.end() ? found->second : "";
    }
};

bool LoadRegistration(const fs::path& path, Registration& registration) {
    const auto title = path.stem().u8string();
    if (title.size() != 8 || title.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
    registration.creator = uint32_t(std::stoul(title, nullptr, 16));
    if (!registration.creator) return false;
    std::error_code error;
    if (!fs::is_regular_file(fs::symlink_status(path, error)) || fs::file_size(path, error) > 8192 || error) return false;
    std::ifstream source(path);
    std::string line;
    while (std::getline(source, line)) {
        std::istringstream item(line);
        std::string key, value;
        if (!(item >> key >> std::quoted(value)) || value.size() > 1024 || value.find('\0') != std::string::npos ||
            !registration.settings.emplace(std::move(key), std::move(value)).second)
            return false;
        item >> std::ws;
        if (!item.eof()) return false;
    }
    const auto priority = registration.Value("priority");
    if (!priority.empty()) {
        const auto last = priority.data() + priority.size();
        const auto parsed = std::from_chars(priority.data(), last, registration.priority);
        if (parsed.ec != std::errc{} || parsed.ptr != last) return false;
    }
    return source.eof() && !registration.Value("name").empty();
}

fs::path FindHost() {
    std::error_code error;
    if (const char* override = std::getenv("FROND_PALM_CONDUIT_HOST")) {
        auto path = fs::u8path(override);
        if (fs::is_regular_file(path, error)) return fs::absolute(path, error);
    }
    const auto directory = ConduitProcess::ExecutableDirectory();
    for (const auto& suffix : {"installer-shims/x86/PalmConduitHost.exe", "installer-shims/PalmConduitHost.exe"}) {
        auto path = directory / suffix;
        if (!directory.empty() && fs::is_regular_file(path, error)) return path;
    }
    return {};
}

std::string WindowsPath(const fs::path& path) {
#ifdef _WIN32
    return path.u8string();
#else
    std::string result = "Z:" + path.u8string();
    std::replace(result.begin(), result.end(), '/', '\\');
    return result;
#endif
}

bool PrepareData(const fs::path& seed, const fs::path& userPath, std::string& error) {
    std::error_code fsError;
    const auto userType = fs::symlink_status(userPath, fsError).type();
    if (userType == fs::file_type::directory) return true;
    if (userType != fs::file_type::not_found) {
        error = "Couldn't open the Palm conduit's user data directory.";
        return false;
    }
    const auto seedType = fs::symlink_status(seed, fsError).type();
    if (seedType == fs::file_type::not_found) {
        fs::create_directories(userPath, fsError);
        if (!fsError) return true;
    } else if (seedType == fs::file_type::directory) {
        // Publish the seed as a whole. The profile lock serialises installation and sync.
        auto staged = userPath;
        staged += ".initial";
        fs::remove_all(staged, fsError);
        if (!fsError) fs::create_directories(staged, fsError);
        if (!fsError) fs::copy(seed, staged, fs::copy_options::recursive | fs::copy_options::skip_symlinks, fsError);
        if (!fsError) fs::rename(staged, userPath, fsError);
        if (!fsError) return true;
        std::error_code cleanup;
        fs::remove_all(staged, cleanup);
    }
    error = "Couldn't initialise the Palm conduit's user data directory.";
    return false;
}

bool SafeDll(const std::string& value) {
    if (value.size() <= 7 || value[1] != ':' || value[2] != '\\' || value.find('"') != std::string::npos) return false;
    auto suffix = value.substr(value.size() - 4);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return std::tolower(c); });
    return suffix == ".dll";
}

}  // namespace

bool RunPalmConduits(DlpClient& dlp, const std::string& profile, const std::string& deviceKey,
                     const UserInfo& user, const std::atomic<bool>* stop,
                     const ConduitMark& wasFast, const ConduitAsset& saveRecovery, const ConduitLog& log,
                     std::string& error) {
    const auto home = fs::absolute(fs::u8path(profile)) / "conduits";
    if (fs::is_regular_file(fs::symlink_status(home / "disabled"))) return true;
    if (fs::exists(fs::symlink_status(home / "installing"))) {
        log("Palm conduits skipped because an installer did not finish.");
        return true;
    }
    auto registered = home / "registered";
    if (fs::symlink_status(registered).type() == fs::file_type::not_found) registered = home / "registered.previous";
    const auto type = fs::symlink_status(registered).type();
    if (type == fs::file_type::not_found) return true;
    if (type != fs::file_type::directory) {
        error = "Couldn't read the Palm conduit registration folder.";
        return false;
    }
    std::vector<Registration> conduits;
    for (const auto& entry : fs::directory_iterator(registered)) {
        if (entry.path().extension() != ".conf") continue;
        Registration registration;
        if (conduits.size() >= 32 || !LoadRegistration(entry.path(), registration)) {
            error = "Couldn't read the Palm conduit registration.";
            return false;
        }
        conduits.push_back(std::move(registration));
    }
    if (conduits.empty()) return true;
#ifdef _WIN32
    const auto syncRoot = home.parent_path().parent_path();
    ProfileSyncLock nativeLock;
    std::string nativeError;
    if (!nativeLock.Acquire(syncRoot.u8string(), PalmConduitNativeLockId, nativeError)) {
        log("Palm conduits skipped: " + nativeError);
        return true;
    }
    if (fs::exists(fs::symlink_status(syncRoot / PalmConduitNativeInstallMarker))) {
        log("Palm conduits skipped because a Windows installer did not finish.");
        return true;
    }
#endif
    const auto host = FindHost();
    if (host.empty()) {
        error = "Couldn't find the Palm conduit host.";
        return false;
    }
    std::sort(conduits.begin(), conduits.end(), [](const Registration& first, const Registration& second) {
        if (first.priority != second.priority) return first.priority < second.priority;
        return first.creator < second.creator;
    });
    const auto userPath = home / "data" / PalmConduitDataKey(deviceKey, user.userId);
    if (!PrepareData(home / "seed", userPath, error)) return false;
    for (const auto& conduit : conduits) {
        const auto dll = conduit.Value("name");
        if (!SafeDll(dll)) {
            error = "The registered Palm conduit has no absolute DLL path.";
            return false;
        }
        const auto directory = conduit.Value("directory");
        if (directory == "." || directory == ".." || directory.find_first_of("\\/:") != std::string::npos) {
            error = "The registered Palm conduit has an invalid data directory.";
            return false;
        }
        const auto conduitPath = directory.empty() ? userPath : userPath / fs::u8path(directory);
        std::error_code fsError;
        fs::create_directories(conduitPath, fsError);
        if (fsError) {
            error = "Couldn't create the Palm conduit's local data directory.";
            return false;
        }
        const auto title = conduit.Value("title");
        const auto localName = conduit.Value("file");
        const auto remote = conduit.Value("remote");
        const auto registry = conduit.Value("registry");
        const auto localPath = WindowsPath(conduitPath) + "\\";
        const auto windowsPath = WindowsPath(userPath);
        if (localPath.size() >= 256 || localName.size() >= 256 || remote.size() >= 32 || registry.size() >= 256) {
            error = "A Palm conduit file or registry name is too long.";
            return false;
        }
        ConduitProcess child(stop);
        if (!child.Start(host, dll, home / "wine")) {
            error = "Couldn't start the Palm conduit host.";
            return false;
        }
        Writer context;
        context.Put(frondConduit::version);
        context.Put(wasFast(conduit.creator) ? 0 : 1);
        context.Put(conduit.creator);
        context.Put(user.userId);
        context.Put(0);  // first sync direction is unspecified
        context.Put(0);  // cable semantics for desktop-managed HotSync
        context.Text(localPath);
        context.Text(windowsPath);
        context.Text(localName);
        context.Text(user.name.substr(0, 63));
        context.Text(remote);
        context.Text(registry);
        if (!frondConduit::WriteFrame([&](const void* data, size_t size) { return child.Write(data, size); }, context.data)) {
            error = "Couldn't send settings to the Palm conduit host.";
            return false;
        }
        const auto name = title.empty() ? Hex(conduit.creator) : title;
        log("Running Palm conduit " + name);
        Runner runner{dlp, user, saveRecovery, log, {}, {}};
        if (!runner.Serve(child, error)) {
            error = name + ": " + error;
            return false;
        }
    }
    return true;
}

}  // namespace frondSync

#else

namespace frondSync {

bool RunPalmConduits(DlpClient&, const std::string&, const std::string&, const UserInfo&, const std::atomic<bool>*, const ConduitMark&,
                     const ConduitAsset&, const ConduitLog&, std::string&) {
    return true;
}

}  // namespace frondSync

#endif
