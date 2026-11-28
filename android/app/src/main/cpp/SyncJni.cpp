#include <jni.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

#include "ArduinoJson.h"
#include "sync/SyncStore.h"
#include "sync/ActiveSyncFiles.h"
#include "sync/InstallerFiles.h"
using namespace ArduinoJson;

namespace {
using namespace frondSync;

std::string fromJava(JNIEnv* env, jbyteArray bytes) {
    std::string result(env->GetArrayLength(bytes), '\0');
    env->GetByteArrayRegion(bytes, 0, result.size(), reinterpret_cast<jbyte*>(result.data()));
    return result;
}

jbyteArray toJava(JNIEnv* env, const std::string& text) {
    jbyteArray result = env->NewByteArray(text.size());
    if (result) env->SetByteArrayRegion(result, 0, text.size(), reinterpret_cast<const jbyte*>(text.data()));
    return result;
}

std::string hex(const Bytes& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t byte : bytes) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 15]);
    }
    return out;
}

Bytes unhex(const std::string& text) {
    const auto digit = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        throw std::runtime_error("Invalid record data.");
    };
    if (text.size() % 2) throw std::runtime_error("Invalid record data.");
    Bytes out;
    out.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) out.push_back(digit(text[i]) * 16 + digit(text[i + 1]));
    return out;
}

PimRecord readRecord(JsonObjectConst row, PimKind kind) {
    PimRecord record;
    record.kind = kind;
    record.id = row["id"] | int64_t{0};
    record.revision = row["revision"] | int64_t{0};
    record.category = row["category"].isNull() ? std::string() : row["category"].as<std::string>();
    record.secret = row["private"] | false;
    record.archived = row["archived"] | false;
    for (const auto item : row["fields"].as<JsonObjectConst>())
        record.fields.emplace(item.key().c_str(), item.value().as<std::string>());
    for (const auto item : row["extras"].as<JsonObjectConst>())
        record.extras.emplace(item.key().c_str(), unhex(item.value().as<std::string>()));
    return record;
}

void writeRecord(JsonObject row, const PimRecord& record) {
    row["id"] = record.id;
    row["revision"] = record.revision;
    row["category"] = record.category;
    row["private"] = record.secret;
    row["archived"] = record.archived;
    auto fields = row.createNestedObject("fields");
    auto extras = row.createNestedObject("extras");
    for (const auto& [key, value] : record.fields) fields[key] = value;
    for (const auto& [key, value] : record.extras) extras[key] = hex(value);
}

Bytes readFile(const std::string& path) {
    constexpr std::streamoff maxSize = 256 * 1024 * 1024;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = static_cast<std::streamoff>(file.tellg());
    if (!file || size <= 0 || size > maxSize) throw std::runtime_error("Choose a PRC or PDB file up to 256 MB.");
    Bytes result(size);
    if (!file.seekg(0).read(reinterpret_cast<char*>(result.data()), size))
        throw std::runtime_error("Couldn't read the database.");
    return result;
}

bool run(const std::string& root, JsonObjectConst in, JsonObject out, std::string& error) {
    const auto op = in["op"].as<std::string>();
    // ArduinoJson turns a missing key into "null"; a missing key reads as empty.
    const auto str = [&](const char* key) { return in[key].isNull() ? std::string() : in[key].as<std::string>(); };
    const int64_t id = in["profile"] | int64_t{0};
    const auto device = str("device");
    SyncLibrary library;
    if (!library.Open(root, error)) return false;
    if (op == "profiles") {
        auto rows = out.createNestedArray("rows");
        for (const auto& info : library.Profiles()) {
            auto row = rows.createNestedObject();
            row["id"] = info.id;
            row["name"] = info.name;
        }
        auto assignments = out.createNestedObject("assignments");
        for (const auto& assignment : library.Assignments()) assignments[assignment.deviceKey] = assignment.profileId;
        if (!device.empty()) {
            const auto assigned = library.FindAssignment(device);
            if (assigned) out["assigned"] = assigned->profileId;
        }
        return true;
    }
    if (op == "create") {
        ProfileInfo profile;
        if (!library.CreateProfile(str("name"), profile, error)) return false;
        out["id"] = profile.id;
        return true;
    }
    if (op == "rename") return library.RenameProfile(id, str("name"), error);
    if (op == "delete") return library.DeleteProfile(id, error);
    if (op == "assign") return id ? library.Assign(device, id, error, str("kind")) : library.Unassign(device, error);

    const auto info = library.FindProfile(id);
    if (!info) { error = "This profile is unavailable."; return false; }
    SyncProfile profile;
    if (!profile.Open(library.ProfilePath(*info), error)) return false;
    const auto assignedToProfile = [&](const std::string& key, const std::string& kind) {
        const auto assignment = library.FindAssignment(key);
        return assignment && assignment->profileId == id && assignment->kind == kind;
    };
    ProfileSyncLock lock;
    if ((op == "restore" || op == "install" || op == "install-member" || op == "unqueue") && !lock.Acquire(root, id, error)) return false;
    if (op == "actions") {
        for (const auto kind : {PimKind::contacts, PimKind::calendar, PimKind::tasks, PimKind::memos})
            out[PimKindName(kind)] = PimActionName(profile.GetPimAction(device, kind));
        return true;
    }
    if (op == "hotsync-transport" || op == "activesync-transport") {
        const bool palm = op == "hotsync-transport";
        if (!assignedToProfile(device, palm ? "palm" : "winmo")) {
            error = "Assign the device to this profile first.";
            return false;
        }
        const char* direct = palm ? "local" : "usb";
        const char* alternate = palm ? "network" : "serial";
        const auto transport = str("value");
        if (transport.empty()) {
            const bool useAlternate = palm ? profile.HotSyncUsesNetwork(device) : profile.ActiveSyncUsesSerial(device);
            out["connection"] = useAlternate ? alternate : direct;
            return true;
        }
        if (transport != direct && transport != alternate) {
            error = palm ? "Choose USB or serial, or Network." : "Choose USB or serial.";
            return false;
        }
        if (!lock.Acquire(root, id, error)) return false;
        return palm ? profile.SetHotSyncNetwork(device, transport == alternate, error)
                    : profile.SetActiveSyncSerial(device, transport == alternate, error);
    }
    if (op == "activesync-settings" || op == "activesync-pair" || op == "activesync-files-enable" ||
        op == "activesync-file-add" || op == "activesync-file-export") {
        if (op != "activesync-settings" && !lock.Acquire(root, id, error)) return false;
        if (!assignedToProfile(device, "winmo")) {
            error = "Assign the device to this profile first.";
            return false;
        }
        const auto confirmed = library.FindProfile(id);
        if (!confirmed || confirmed->folder != info->folder) {
            error = "This profile is unavailable.";
            return false;
        }
        const auto folder = library.ProfilePath(*confirmed);
        if (op == "activesync-pair")
            return profile.RequestActiveSyncPairing(device, in["request"] | false, error);
        if (op == "activesync-settings") {
            out["connection"] = profile.ActiveSyncUsesSerial(device) ? "serial" : "usb";
            out["pairing"] = profile.ActiveSyncPairing(device);
            out["filesEnabled"] = !ActiveSyncFilesFolder(folder).empty();
            auto files = out.createNestedArray("files");
            for (const auto& name : ListActiveSyncFiles(folder)) files.add(name);
        } else if (op == "activesync-files-enable") {
            ActiveSyncFilesFolder(folder, true);
        } else if (op == "activesync-file-add") {
            AddActiveSyncFile(folder, str("name"), str("path"));
        } else {
            ExportActiveSyncFile(folder, str("name"), str("path"));
        }
        return true;
    }
    if (op == "backups") {
        auto rows = out.createNestedArray("rows");
        for (const auto& source : profile.BackupDevices(true)) for (const auto& backup : profile.Backups(source)) {
            auto row = rows.createNestedObject();
            row["device"] = source;
            // A recovery copy belongs to its own device. "recovery" is when it was stored.
            std::string owner;
            if (IsRecoverySource(source, &owner)) {
                row["owner"] = owner;
                row["recovery"] = backup.storedAt;
            }
            // Palm database names are raw bytes. Keep them as hex for the key and show their Palm text.
            const Bytes name(backup.name.begin(), backup.name.end());
            row["key"] = hex(name);
            row["name"] = PalmPimText(name);
            row["extension"] = (backup.attributes & 1) ? "prc" : "pdb";
        }
        return true;
    }
    if (op == "backup") {
        const auto name = unhex(str("key"));
        Bytes bytes;
        if (!profile.LoadBackup(device, std::string(name.begin(), name.end()), bytes)) {
            error = "Couldn't read the backup.";
            return false;
        }
        std::ofstream file(str("path"), std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        file.close();
        if (!file) { error = "Couldn't write the backup."; return false; }
        return true;
    }
    if (op == "restore") {
        if (!assignedToProfile(device, "palm")) { error = "Assign the device to this profile first."; return false; }
        const auto count = profile.QueueRestore(str("source"), device, error);
        if (count < 0) return false;
        if (count == 0) { error = "No backups to restore."; return false; }
        out["count"] = count;
        return true;
    }
    if (op == "queue") {
        auto rows = out.createNestedArray("rows");
        const auto kind = str("kind");
        for (const auto& install : profile.InstallQueue()) {
            if (!InstallItemFitsKind(install, kind)) continue;
            auto row = rows.createNestedObject();
            row["id"] = install.id;
            row["name"] = install.fileName;
            row["state"] = install.state;
            row["message"] = install.message;
            row["device"] = install.deviceKey;
        }
        return true;
    }
    if (op == "installer-files" || op == "install-member" || op == "install") {
        InstallerFile file{str("name"), readFile(str("path"))};
        if (op != "install") {
            std::vector<InstallerFile> files;
            if (!UnpackInstaller(file.name, file.bytes, files, error)) return false;
            if (op == "installer-files") {
                auto rows = out.createNestedArray("rows");
                for (size_t i = 0; i < files.size(); ++i) {
                    const auto& candidate = files[i];
                    const auto cab = IsCabFileName(candidate.name);
                    if (!device.empty() && !assignedToProfile(device, cab ? "winmo" : "palm")) continue;
                    auto row = rows.createNestedObject();
                    row["index"] = i;
                    row["name"] = candidate.name;
                }
                if (rows.size() == 0) {
                    error = "No install files for the selected device.";
                    return false;
                }
                return true;
            }
            const auto index = in["index"] | files.size();
            if (index >= files.size() || files[index].name != str("member")) {
                error = "The installer changed. Open it again.";
                return false;
            }
            file = std::move(files[index]);
        }
        const bool cab = IsCabFileName(file.name);
        if (!device.empty() && !assignedToProfile(device, cab ? "winmo" : "palm")) {
            error = cab ? "Choose a Windows Mobile device assigned to this profile." : "Choose a Palm assigned to this profile.";
            return false;
        }
        InstallItem item;
        return profile.QueueInstall(device, file.name, file.bytes, item, error, cab ? "cab" : "ram");
    }
    if (op == "unqueue") return profile.RemoveInstall(in["id"].as<int64_t>(), error);
    if (op == "log") {
        auto rows = out.createNestedArray("rows");
        for (const auto& entry : profile.LogEntries("")) {
            auto row = rows.createNestedObject();
            row["time"] = entry.time;
            row["text"] = entry.text;
            row["device"] = entry.deviceKey;
        }
        return true;
    }

    PimKind kind;
    if (!ParsePimKind(str("kind"), kind)) { error = "Unknown record type."; return false; }
    if (op == "action") {
        PimAction action;
        if (!ParsePimAction(str("value"), action)) { error = "Unknown sync setting."; return false; }
        return profile.SetPimAction(device, kind, action, error);
    }
    if (op == "records") {
        std::vector<PimRecord> records;
        if (!profile.ReadPimRecords(kind, false, records, error)) return false;
        auto rows = out.createNestedArray("rows");
        for (const auto& record : records) writeRecord(rows.createNestedObject(), record);
        return true;
    }
    if (op == "save") {
        auto record = readRecord(in["record"], kind);
        return profile.SavePimRecord(record, error);
    }
    if (op == "remove") return profile.DeletePimRecord(in["id"].as<int64_t>(), in["revision"].as<int64_t>(), error);
    if (op == "import") {
        if (!profile.BeginPimChange(error)) return false;
        for (const auto row : in["rows"].as<JsonArrayConst>()) {
            auto record = readRecord(row.as<JsonObjectConst>(), kind);
            record.id = record.revision = 0;
            if (!profile.SavePimRecord(record, error)) { profile.RollBackPimChange(); return false; }
        }
        return profile.CommitPimChange(error);
    }
    error = "Unknown sync manager operation.";
    return false;
}
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_com_rm_frond_SyncNative_exchange(
    JNIEnv* env, jobject, jbyteArray root, jbyteArray request) {
    try {
        const auto text = fromJava(env, request);
        constexpr size_t limit = 64 * 1024 * 1024;
        if (text.size() > limit / 2) throw std::runtime_error("The sync manager request is too large.");
        const auto rootPath = fromJava(env, root);
        DynamicJsonDocument input(8192 + text.size());
        auto parse = deserializeJson(input, text);
        while (parse == DeserializationError::NoMemory && input.capacity() < limit) {
            input = DynamicJsonDocument(std::min(limit, input.capacity() * 2));
            parse = deserializeJson(input, text);
        }
        if (parse) throw std::runtime_error("Invalid or oversized sync manager request.");
        const auto args = input.as<JsonObjectConst>();
        const auto op = args["op"].as<std::string>();
        const bool listing = op == "profiles" || op == "backups" || op == "queue" || op == "records" || op == "log" || op == "installer-files" ||
                             op == "activesync-settings";
        std::string error, response;
        for (size_t capacity = 8192; capacity <= limit; capacity *= 2) {
            DynamicJsonDocument output(capacity);
            if (!run(rootPath, args, output.to<JsonObject>(), error))
                throw std::runtime_error(error.empty() ? "Couldn't update the sync manager." : error);
            if (!output.overflowed()) {
                serializeJson(output, response);
                return toJava(env, response);
            }
            if (!listing) break;
        }
        throw std::runtime_error("The sync manager result is too large.");
    } catch (const std::exception& e) {
        DynamicJsonDocument output(1024 + strlen(e.what()));
        output["error"] = e.what();
        std::string response;
        serializeJson(output, response);
        return toJava(env, response);
    }
}
