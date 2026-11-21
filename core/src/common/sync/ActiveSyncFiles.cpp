#include "ActiveSyncFiles.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <stdexcept>
#include <vector>

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
    unsigned skipped{};
};

bool GuestEntry(void* opaque, const uint16_t* name, size_t length, uint32_t attributes, uint64_t size) {
    auto& listing = *static_cast<Listing*>(opaque);
    // Ignore directories, reparse points, and temporary files from incomplete copies.
    if (attributes & (0x10 | 0x400)) return true;
    if (length && name[0] == '.') return true;
    if (std::any_of(name, name + length, [](uint16_t c) { return c < ' ' || c > '~'; })) {
        ++listing.skipped;
        return true;
    }
    std::string display(name, name + length);
    const auto key = FileKey(display);
    if (key.empty() || size > maxFileSize) {
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
        if (!fs::is_regular_file(entry.symlink_status())) continue;
        const auto key = FileKey(name);
        const auto size = entry.file_size();
        if (key.empty() || size > maxFileSize) {
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

std::string ExchangeActiveSyncFiles(FrondRapi* client, const std::string& profileFolder) {
    const auto enabled = ActiveSyncFilesFolder(profileFolder);
    if (enabled.empty()) return {};
    const auto folder = fs::u8path(enabled);
    if (!FrondRapiMakeDirectory(client, WideAscii(guestFolder).data()))
        throw std::runtime_error("Couldn't create the Windows Mobile Frond Sync folder.");
    const auto host = ReadHostDirectory(folder);
    Listing guest;
    if (!FrondRapiList(client, GuestPath("*").data(), GuestEntry, &guest))
        throw std::runtime_error("Couldn't list the Windows Mobile Frond Sync folder.");

    uint64_t remaining = maxTransferSize;
    const auto reserve = [&](uint64_t size) {
        if (size > remaining) throw std::runtime_error("The file exchange exceeds 128 MB.");
        remaining -= size;
    };
    unsigned sent = 0, received = 0, conflicts = 0;
    for (const auto& [key, file] : host.files) {
        if (const auto existing = guest.files.find(key); existing != guest.files.end()) {
            if (file.size != existing->second.size) {
                ++conflicts;
            } else {
                reserve(file.size);
                if (ReadHostFile(folder, file) != ReadGuestFile(client, existing->second)) ++conflicts;
            }
        } else {
            reserve(file.size);
            WriteGuestFile(client, file.name, ReadHostFile(folder, file));
            ++sent;
        }
    }
    for (const auto& [key, file] : guest.files) {
        if (host.files.count(key)) continue;
        reserve(file.size);
        WriteHostFile(folder, file.name, ReadGuestFile(client, file));
        ++received;
    }
    return "Files: " + std::to_string(sent) + " sent, " + std::to_string(received) + " received, " +
           std::to_string(conflicts) + " differed, " + std::to_string(host.skipped + guest.skipped) + " skipped.";
}

}  // namespace frondSync
