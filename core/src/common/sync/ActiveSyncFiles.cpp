#include "ActiveSyncFiles.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "PwiNote.h"
#include "Sha256.h"
#include "SyncStore.h"

namespace frondSync {

namespace {

namespace fs = std::filesystem;
constexpr char guestFolder[] = "\\My Documents\\Frond Sync";
constexpr uint64_t maxFileSize = 16 * 1024 * 1024;
constexpr uint64_t maxTransferSize = 128 * 1024 * 1024;

std::vector<uint16_t> WideAscii(const std::string& text) {
    std::vector<uint16_t> result(text.begin(), text.end());
    result.push_back(0);
    return result;
}

struct File {
    std::string name;
    uint64_t size;
};
using Files = std::map<std::string, File>;

std::string FileKey(std::string name) {
    if (name.empty() || name.size() > 128 || name.front() == '.' || name.back() == '.' || name.back() == ' ' ||
        name.find_first_of("<>:\"/\\|?*") != std::string::npos)
        return {};
    for (char& c : name) {
        if (c < ' ' || c > '~') return {};
        if ('A' <= c && c <= 'Z') c += 'a' - 'A';
    }
    const auto stem = name.substr(0, name.find('.'));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
        (stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) &&
         '0' <= stem[3] && stem[3] <= '9'))
        return {};
    return name;
}

struct Listing {
    Files files;
    std::set<std::string> blocked;
    unsigned skipped{};
};

bool GuestEntry(void* opaque, const uint16_t* name, size_t length, uint32_t attributes, uint64_t size) {
    auto& listing = *static_cast<Listing*>(opaque);
    // Ignore temporary files from incomplete copies.
    if (length && name[0] == '.') return true;
    if (std::any_of(name, name + length, [](uint16_t c) { return c < ' ' || c > '~'; })) {
        ++listing.skipped;
        return true;
    }
    std::string display(name, name + length);
    const auto key = FileKey(display);
    if (key.empty() || size > maxFileSize || (attributes & (0x10 | 0x400))) {
        if (!key.empty()) listing.blocked.insert(key);
        ++listing.skipped;
        return true;
    }
    return listing.files.emplace(key, File{std::move(display), size}).second;
}

Listing ReadHostDirectory(const fs::path& folder) {
    Listing listing;
    size_t entries = 0;
    for (const auto& entry : fs::directory_iterator(folder)) {
        if (++entries > 1024) throw std::runtime_error("Too many files in the profile Files folder.");
        const auto name = entry.path().filename().u8string();
        if (!name.empty() && name.front() == '.') continue;
        const auto key = FileKey(name);
        if (!fs::is_regular_file(entry.symlink_status())) {
            if (!key.empty()) listing.blocked.insert(key);
            continue;
        }
        const auto size = entry.file_size();
        if (key.empty() || size > maxFileSize) {
            if (!key.empty()) listing.blocked.insert(key);
            ++listing.skipped;
            continue;
        }
        if (!listing.files.emplace(key, File{name, size}).second)
            throw std::runtime_error("Two files in the profile Files folder have the same Windows Mobile name.");
    }
    return listing;
}

Bytes ReadHostFile(const fs::path& folder, const File& file) {
    const auto path = folder / fs::u8path(file.name);
    if (!fs::is_regular_file(fs::symlink_status(path))) throw std::runtime_error("A profile file changed during sync.");
    std::ifstream in(path, std::ios::binary);
    Bytes data(file.size);
    if (!in || !in.read(reinterpret_cast<char*>(data.data()), data.size()) || in.peek() != EOF)
        throw std::runtime_error("A profile file changed during sync.");
    return data;
}

std::vector<uint16_t> GuestPath(const std::string& name) {
    return WideAscii(std::string(guestFolder) + "\\" + name);
}

Bytes ReadGuestFile(FrondRapi* client, const File& file) {
    const uint32_t handle = FrondRapiOpenFile(client, GuestPath(file.name).data());
    if (handle == UINT32_MAX) throw std::runtime_error("Couldn't read " + file.name + " from Windows Mobile.");
    Bytes data;
    std::array<uint8_t, 8192> buffer;
    bool ok;
    do {
        uint32_t size;
        const auto remaining = file.size - data.size();
        const auto request = uint32_t(std::min<uint64_t>(buffer.size(), remaining ? remaining : 1));
        ok = FrondRapiRead(client, handle, buffer.data(), request, &size) && size <= remaining;
        if (!ok || !size) break;
        data.insert(data.end(), buffer.begin(), buffer.begin() + size);
    } while (ok);
    if (!FrondRapiCloseFile(client, handle) || !ok || data.size() != file.size)
        throw std::runtime_error("Couldn't read all of " + file.name + " from Windows Mobile.");
    return data;
}

std::string TemporaryName() {
    return ".frond-part-" + std::to_string(std::random_device{}());
}

void WriteGuestFile(FrondRapi* client, const std::string& name, const Bytes& data) {
    const auto temporary = GuestPath(TemporaryName());
    const auto destination = GuestPath(name);
    const auto handle = FrondRapiCreate(client, temporary.data());
    if (handle == UINT32_MAX) throw std::runtime_error("Couldn't create a Windows Mobile temporary file.");
    bool ok = true;
    for (size_t i = 0; i < data.size() && ok;) {
        const auto amount = std::min<size_t>(8192, data.size() - i);
        ok = FrondRapiWrite(client, handle, data.data() + i, amount);
        i += amount;
    }
    if (!FrondRapiCloseFile(client, handle) || !ok || !FrondRapiMove(client, temporary.data(), destination.data())) {
        FrondRapiRemove(client, temporary.data());
        throw std::runtime_error("Couldn't copy " + name + " to Windows Mobile.");
    }
}

void WriteHostFile(const fs::path& folder, const std::string& name, const Bytes& data) {
    const auto temporary = folder / TemporaryName();
    if (!fs::create_directory(temporary)) throw std::runtime_error("Couldn't create a temporary sync folder.");
    try {
        const auto file = temporary / "file";
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out.write(reinterpret_cast<const char*>(data.data()), data.size()))
            throw std::runtime_error("Couldn't write " + name + " to the profile.");
        out.close();
        if (!out) throw std::runtime_error("Couldn't finish writing " + name + " to the profile.");
        fs::create_hard_link(file, folder / fs::u8path(name));
        fs::remove(file);
        fs::remove(temporary);
    } catch (...) {
        std::error_code unused;
        fs::remove_all(temporary, unused);
        throw;
    }
}

std::string FileHash(const Bytes& data) {
    return Sha256Hex(data.data(), data.size());
}

using Baseline = std::map<std::string, std::string>;

Baseline ReadBaseline(const std::string& value) {
    Baseline result;
    std::istringstream input(value);
    for (std::string line; std::getline(input, line);) {
        if (line.size() <= 65 || line[64] != '\t') continue;
        const auto key = line.substr(65);
        if (FileKey(key) == key) result.emplace(key, line.substr(0, 64));
    }
    return result;
}

std::string EncodeBaseline(const Baseline& baseline) {
    std::string result;
    for (const auto& [key, hash] : baseline) result += hash + "\t" + key + "\n";
    return result;
}

std::string RecoveryName(const std::string& name) {
    return std::to_string(std::random_device{}()) + "-" + name;
}

fs::path HostRecoveryFolder(const fs::path& folder) {
    const auto recovery = folder / ".frond-recovery";
    fs::create_directory(recovery);
    if (!fs::is_directory(fs::symlink_status(recovery)))
        throw std::runtime_error("Couldn't open the profile file recovery folder.");
    return recovery;
}

void ChangeHostFile(const fs::path& folder, const File& old, const Bytes& expected, const Bytes* replacement) {
    const auto recovery = HostRecoveryFolder(folder);
    const auto parkedName = RecoveryName(old.name);
    const auto parked = recovery / fs::u8path(parkedName);
    const auto original = folder / fs::u8path(old.name);
    // The exclusive directory keeps a concurrent rename from replacing a recovery copy.
    if (!fs::create_directory(parked)) throw std::runtime_error("Couldn't reserve a file recovery folder.");
    const auto parkedFile = parked / fs::u8path(old.name);
    try {
        fs::rename(original, parkedFile);
    } catch (...) {
        std::error_code unused;
        fs::remove(parked, unused);
        throw;
    }
    try {
        if (ReadHostFile(parked, old) != expected)
            throw std::runtime_error("The profile file changed during sync.");
        if (replacement) WriteHostFile(folder, old.name, *replacement);
    } catch (...) {
        // Put the original back only if its name is still free.
        std::error_code error;
        fs::create_hard_link(parkedFile, original, error);
        if (!error) {
            fs::remove(parkedFile, error);
            fs::remove(parked, error);
        }
        throw;
    }
}

void ChangeGuestFile(FrondRapi* client, const File& old, const Bytes& expected, const Bytes* replacement) {
    const auto recovery = GuestPath(".frond-recovery");
    if (!FrondRapiMakeDirectory(client, recovery.data()))
        throw std::runtime_error("Couldn't create the Windows Mobile file recovery folder.");
    const std::string parkedName = ".frond-recovery\\" + RecoveryName(old.name);
    const auto parked = GuestPath(parkedName);
    const auto original = GuestPath(old.name);
    if (!FrondRapiMove(client, original.data(), parked.data()))
        throw std::runtime_error("Couldn't move " + old.name + " to the Windows Mobile file recovery folder.");
    try {
        if (ReadGuestFile(client, {parkedName, old.size}) != expected)
            throw std::runtime_error("The Windows Mobile file changed during sync.");
        if (replacement) WriteGuestFile(client, old.name, *replacement);
    } catch (...) {
        // An intervening guest save can occupy the name. The parked file stays recoverable.
        if (FrondRapiConnected(client)) FrondRapiMove(client, parked.data(), original.data());
        throw;
    }
}

constexpr char notesCategory[] = "WM5 Notes";

bool IsNoteFile(const std::string& key) {
    return key.size() > 4 && key.compare(key.size() - 4, 4, ".pwi") == 0;
}

std::string NoteField(const PimRecord& memo, const char* key) {
    const auto field = memo.extras.find(key);
    return field == memo.extras.end() ? std::string{} : std::string(field->second.begin(), field->second.end());
}

std::optional<std::string> MemoText(const PimRecord& memo) {
    const auto text = memo.fields.find("text");
    if (memo.deleted || text == memo.fields.end()) return std::nullopt;
    return text->second;
}

void SaveNoteLink(SyncProfile& profile, PimRecord& memo, const std::string& key, const std::string& text) {
    memo.extras[noteFileField] = Bytes(key.begin(), key.end());
    memo.extras[noteTextField] = Bytes(text.begin(), text.end());
    std::string error;
    if (!profile.SavePimRecord(memo, error)) throw std::runtime_error(error);
}

struct NotePass {
    std::set<std::string> blocked, conflicts;
};

NotePass ExportNotes(SyncProfile& profile, const fs::path& folder, bool syncNotes) {
    NotePass result;
    std::string error;
    if (!profile.BeginPimChange(error)) throw std::runtime_error(error);
    try {
        std::vector<PimRecord> memos;
        if (!profile.ReadPimRecords(PimKind::memos, true, memos, error)) throw std::runtime_error(error);
        const auto folderFiles = ReadHostDirectory(folder);
        for (auto& memo : memos) {
            const auto linked = NoteField(memo, noteFileField);
            if (!syncNotes || memo.secret || memo.archived) {
                if (!linked.empty()) result.blocked.insert(linked);
                continue;
            }
            const auto text = MemoText(memo);
            if (linked.empty()) {
                if (!text || memo.category != notesCategory) continue;
                const auto data = WritePwiNote(*text);
                if (!data) continue;
                const auto name = "Frond Memo " + std::to_string(memo.id) + ".pwi";
                const auto key = FileKey(name);
                if (folderFiles.blocked.count(key)) continue;
                if (const auto file = folderFiles.files.find(key); file != folderFiles.files.end()) {
                    if (ReadPwiNote(ReadHostFile(folder, file->second)) != text) continue;
                } else {
                    WriteHostFile(folder, name, *data);
                }
                SaveNoteLink(profile, memo, key, *text);
                continue;
            }

            const auto originalText = NoteField(memo, noteTextField);
            if (text && *text == originalText) continue;
            if (!IsNoteFile(linked) || FileKey(linked) != linked || folderFiles.blocked.count(linked)) continue;
            const auto file = folderFiles.files.find(linked);
            if (file == folderFiles.files.end()) {
                // A stop after parking the old copy must not turn a memo edit into a file deletion.
                if (text)
                    if (const auto data = WritePwiNote(*text)) WriteHostFile(folder, linked, *data);
                continue;
            }
            const auto old = ReadHostFile(folder, file->second);
            const auto fileText = ReadPwiNote(old);
            if (fileText != originalText) {
                if (!text || text != fileText) result.conflicts.insert(linked);
                continue;
            }
            if (!text) {
                ChangeHostFile(folder, file->second, old, nullptr);
                continue;
            }
            auto replacement = old;
            if (ReplacePwiNoteText(replacement, *text)) ChangeHostFile(folder, file->second, old, &replacement);
        }
        if (!profile.CommitPimChange(error)) throw std::runtime_error(error);
    } catch (...) {
        profile.RollBackPimChange();
        throw;
    }
    return result;
}

void ImportNotes(SyncProfile& profile, const fs::path& folder, const std::set<std::string>& unsettled,
                 bool complete, NotePass& notes) {
    const auto files = ReadHostDirectory(folder);
    const auto available = [&](const std::string& key) {
        return !unsettled.count(key) && !files.blocked.count(key);
    };
    std::string error;
    if (!profile.BeginPimChange(error)) throw std::runtime_error(error);
    try {
        std::vector<PimRecord> memos;
        if (!profile.ReadPimRecords(PimKind::memos, true, memos, error)) throw std::runtime_error(error);
        std::set<std::string> linkedFiles;
        for (auto& memo : memos) {
            const auto key = NoteField(memo, noteFileField);
            if (!IsNoteFile(key)) continue;
            if (!available(key) || memo.secret || memo.archived) {
                linkedFiles.insert(key);
                continue;
            }
            const auto text = MemoText(memo);
            const auto base = NoteField(memo, noteTextField);
            const auto file = files.files.find(key);
            if (file == files.files.end() && complete && (memo.deleted || text == base)) {
                memo.deleted = true;
                DetachPwiMemo(memo);
                if (!profile.SavePimRecord(memo, error)) throw std::runtime_error(error);
                continue;
            }
            linkedFiles.insert(key);
            if (file == files.files.end()) {
                if (complete && text != base) notes.conflicts.insert(key);
                continue;
            }
            const auto updated = ReadPwiNote(ReadHostFile(folder, file->second));
            if (updated == base) continue;
            if (text != base && text != updated) {
                notes.conflicts.insert(key);
                continue;
            }
            if (memo.deleted || !updated) continue;
            memo.fields["text"] = *updated;
            SaveNoteLink(profile, memo, key, *updated);
        }
        for (const auto& [key, file] : files.files) {
            if (!IsNoteFile(key) || !available(key) || linkedFiles.count(key)) continue;
            if (const auto text = ReadPwiNote(ReadHostFile(folder, file))) {
                PimRecord memo;
                memo.category = notesCategory;
                memo.fields["text"] = *text;
                SaveNoteLink(profile, memo, key, *text);
            }
        }
        if (!profile.CommitPimChange(error)) throw std::runtime_error(error);
    } catch (...) {
        profile.RollBackPimChange();
        throw;
    }
}

}  // namespace

std::vector<std::string> ListActiveSyncFiles(const std::string& profileFolder) {
    const auto folder = ActiveSyncFilesFolder(profileFolder);
    std::vector<std::string> result;
    if (folder.empty()) return result;
    const auto listing = ReadHostDirectory(fs::u8path(folder));
    for (const auto& [key, file] : listing.files) result.push_back(file.name);
    return result;
}

void AddActiveSyncFile(const std::string& profileFolder, const std::string& name, const std::string& source) {
    const auto folder = ActiveSyncFilesFolder(profileFolder);
    if (folder.empty()) throw std::runtime_error("Enable ActiveSync file exchange first.");
    const auto key = FileKey(name);
    if (key.empty()) throw std::runtime_error("Choose a file with a Windows Mobile-compatible name.");
    const auto target = fs::u8path(folder);
    if (ReadHostDirectory(target).files.count(key))
        throw std::runtime_error("The profile already has a file with this name.");
    const auto input = fs::u8path(source);
    if (!fs::is_regular_file(fs::symlink_status(input))) throw std::runtime_error("Couldn't open the selected file.");
    const auto size = fs::file_size(input);
    if (size > maxFileSize) throw std::runtime_error("Choose a file up to 16 MB.");
    WriteHostFile(target, name, ReadHostFile(input.parent_path(), {input.filename().u8string(), size}));
}

void ExportActiveSyncFile(const std::string& profileFolder, const std::string& name, const std::string& destination) {
    const auto folder = ActiveSyncFilesFolder(profileFolder);
    const auto key = FileKey(name);
    if (folder.empty() || key.empty()) throw std::runtime_error("Couldn't find the profile file.");
    const auto target = fs::u8path(folder);
    const auto listing = ReadHostDirectory(target);
    const auto file = listing.files.find(key);
    if (file == listing.files.end()) throw std::runtime_error("Couldn't find the profile file.");
    const auto data = ReadHostFile(target, file->second);
    std::ofstream output(fs::u8path(destination), std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(data.data()), data.size());
    output.close();
    if (!output) throw std::runtime_error("Couldn't save the profile file.");
}

std::string ExchangeActiveSyncFiles(FrondRapi* client, SyncProfile& profile, const std::string& device,
                                    const std::string& profileFolder, bool syncNotes) {
    const auto enabled = ActiveSyncFilesFolder(profileFolder);
    if (enabled.empty()) return {};
    const auto folder = fs::u8path(enabled);
    auto notes = ExportNotes(profile, folder, syncNotes);
    if (!FrondRapiMakeDirectory(client, WideAscii(guestFolder).data()))
        throw std::runtime_error("Couldn't create the Windows Mobile Frond Sync folder.");
    const auto host = ReadHostDirectory(folder);
    Listing guest;
    if (!FrondRapiList(client, GuestPath("*").data(), GuestEntry, &guest))
        throw std::runtime_error("Couldn't list the Windows Mobile Frond Sync folder.");

    constexpr char baselineKey[] = "activesync-files-v1";
    constexpr char cursorKey[] = "activesync-files-cursor";
    auto baseline = ReadBaseline(profile.DeviceValue(device, baselineKey));
    const auto save = [&] {
        std::string error;
        if (!profile.SetDeviceValue(device, baselineKey, EncodeBaseline(baseline), &error))
            throw std::runtime_error(error);
    };
    std::set<std::string> keys;
    for (const auto& [key, hash] : baseline) keys.insert(key);
    for (const auto& [key, file] : host.files) keys.insert(key);
    for (const auto& [key, file] : guest.files) keys.insert(key);

    uint64_t remaining = maxTransferSize;
    const auto first = keys.lower_bound(profile.DeviceValue(device, cursorKey));
    std::vector<std::string> order(first, keys.end());
    order.insert(order.end(), keys.begin(), first);
    std::string deferred;
    unsigned sent = 0, received = 0, conflicts = 0, removed = 0;
    std::set<std::string> unsettled = guest.blocked;
    for (const auto& key : order) {
        if (!deferred.empty() || host.blocked.count(key) || guest.blocked.count(key) || notes.blocked.count(key) ||
            notes.conflicts.count(key)) {
            unsettled.insert(key);
            continue;
        }
        const auto local = host.files.find(key);
        const auto remote = guest.files.find(key);
        const bool hasLocal = local != host.files.end(), hasRemote = remote != guest.files.end();
        const auto previous = baseline.find(key);
        const auto base = previous == baseline.end() ? std::string{} : previous->second;
        if (!hasLocal && !hasRemote) {
            baseline.erase(key);
            continue;
        }
        if (base.empty() && hasLocal && hasRemote && local->second.size != remote->second.size) {
            ++conflicts;
            unsettled.insert(key);
            continue;
        }
        if (hasRemote && remote->second.size > remaining) {
            deferred = key;
            unsettled.insert(key);
            continue;
        }
        if (hasRemote) remaining -= remote->second.size;
        const auto localData = hasLocal ? ReadHostFile(folder, local->second) : Bytes{};
        const auto remoteData = hasRemote ? ReadGuestFile(client, remote->second) : Bytes{};
        const auto localHash = hasLocal ? FileHash(localData) : std::string{};
        const auto remoteHash = hasRemote ? FileHash(remoteData) : std::string{};
        if (localHash == remoteHash) {
            baseline[key] = localHash;
            continue;
        }

        const bool send = remoteHash == base;
        const bool receive = localHash == base;
        if (!send && !receive) {
            ++conflicts;
            unsettled.insert(key);
            continue;
        }
        const auto cost = send ? localData.size() + remoteData.size() : 0;
        if (cost > remaining) {
            deferred = key;
            unsettled.insert(key);
            continue;
        }
        remaining -= cost;
        if (!base.empty()) {
            // After an interruption, a missing original may be parked, rather than deleted by a user.
            baseline.erase(key);
            save();
        }
        if (send) {
            if (hasRemote) ChangeGuestFile(client, remote->second, remoteData, hasLocal ? &localData : nullptr);
            else WriteGuestFile(client, local->second.name, localData);
            if (hasLocal) ++sent;
            else ++removed;
        } else {
            if (hasLocal) ChangeHostFile(folder, local->second, localData, hasRemote ? &remoteData : nullptr);
            else WriteHostFile(folder, remote->second.name, remoteData);
            if (hasRemote) ++received;
            else ++removed;
        }
        const auto& agreed = send ? localHash : remoteHash;
        if (!agreed.empty()) baseline[key] = agreed;
    }
    save();
    std::string error;
    if (!profile.SetDeviceValue(device, cursorKey, deferred, &error)) throw std::runtime_error(error);
    if (syncNotes) ImportNotes(profile, folder, unsettled, deferred.empty(), notes);
    return "Files: " + std::to_string(sent) + " sent, " + std::to_string(received) + " received, " +
           std::to_string(conflicts) + " differed, " + std::to_string(host.skipped + guest.skipped) + " skipped, " +
           std::to_string(removed) + " removed." +
           (removed || sent || received ? " Replaced and removed files are kept in .frond-recovery." : "") +
           (deferred.empty() ? "" : " File exchange will continue on the next sync.") +
           (notes.conflicts.empty() ? "" : " Notes: " + std::to_string(notes.conflicts.size()) + " differed.");
}

}  // namespace frondSync
