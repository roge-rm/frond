#ifndef FROND_ZAURUS_MICRODRIVE_H
#define FROND_ZAURUS_MICRODRIVE_H

#include "MachineFolder.h"
#include "SdCandidate.h"
#include "storage/Io.h"
#ifdef __ANDROID__
#include "CardTool.h"
#endif

namespace frondNative {

inline constexpr char microdrivePendingId[] = ".microdrive.id.pending";

inline std::filesystem::path& managedMicrodriveFolder() {
    static std::filesystem::path folder;
    return folder;
}

inline constexpr char microdriveRecord[] = "zaurus-microdrive-v1 ";
inline std::string& microdriveGeneration() {
    static std::string generation;
    return generation;
}

inline bool validMicrodriveToken(std::string_view token) {
    return token.size() == 32 && std::all_of(token.begin(), token.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

inline bool readMicrodriveId(const std::filesystem::path& file, std::string& id) {
    const frondStorage::Fd fd(::open(file.c_str(), O_RDONLY | O_CLOEXEC));
    std::array<char, 32> bytes{};
    if (fd.fd < 0 || frondStorage::sizeOf(fd.fd) != int64_t(bytes.size()) ||
        !frondStorage::readAllAt(fd.fd, bytes.data(), bytes.size(), 0))
        return false;
    if (!validMicrodriveToken({bytes.data(), bytes.size()})) return false;
    id.assign(bytes.data(), bytes.size());
    return true;
}

// A migrated save can use the pending ID if the process stopped just after publishing the state.
inline bool microdriveIdFor(const std::filesystem::path& folder, std::string& id) {
    const auto live = folder / frondMachine::microdriveId;
    std::error_code ec;
    if (std::filesystem::exists(live, ec)) return readMicrodriveId(live, id);
    if (ec) return false;
    const auto pending = folder / microdrivePendingId;
    if (std::filesystem::exists(pending, ec)) return readMicrodriveId(pending, id);
    if (ec) return false;

    std::filesystem::path temp;
    if (!frondStorage::createTemp(folder, ".frond.tmp-microdrive-id-", temp)) return false;
    const frondStorage::Fd fd(::open(temp.c_str(), O_WRONLY | O_CLOEXEC));
    const auto generated = frondSdCandidate::randomIdentity();
    if (fd.fd < 0 || !frondStorage::writeAllAt(fd.fd, generated.data(), generated.size(), 0) || fsync(fd.fd) < 0) {
        frondStorage::removePath(temp);
        return false;
    }
#ifndef _WIN32
#ifdef __ANDROID__
    // The app holds the machine's run lock and its sandbox disallows hard links.
    const bool created = frondCardTool::moveNoReplace(temp, pending);
#else
    const bool created = ::link(temp.c_str(), pending.c_str()) == 0;
#endif
    frondStorage::removePath(temp);
    if (!created) {
        if (std::filesystem::exists(live, ec)) return readMicrodriveId(live, id);
        return readMicrodriveId(pending, id);
    }
#else
    std::filesystem::rename(temp, pending, ec);
    if (ec) {
        frondStorage::removePath(temp);
        return false;
    }
#endif
    id = generated;
    return frondStorage::syncDir(folder);
}

inline bool publishMicrodriveId(const std::filesystem::path& folder) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(folder / frondMachine::microdriveId, ec)) return true;
    std::filesystem::rename(folder / microdrivePendingId, folder / frondMachine::microdriveId, ec);
    return !ec && frondStorage::syncDir(folder);
}

}  // namespace frondNative

#endif
