#ifndef FROND_CARD_TOOL_H
#define FROND_CARD_TOOL_H

#include "CardBacking.h"
#include "MemoryStickGeometry.h"
#include "MemoryStickImage.h"
#include "SdCandidate.h"
#include "SdImage.h"
#include "SessionIo.h"
#include "device_profile5.h"
#include "storage/Settle.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include "PosixCompat.h"
#ifndef EDQUOT
#define EDQUOT ENOSPC  // no quota error here
#endif
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
#include <cstdlib>
#include <unistd.h>
#include <vector>

// Makes, imports and exports card files outside a running machine, for both frontends: `make`
// (kind, size, output), `import` (image, kind, output) and `export` (card file, image). The output
// of make and import is a settled card file of its kind (.frondsd v2 for SD and Memory Stick Pro,
// v2 .frondms for a classic stick), written to a temp beside it and renamed, so
// a failed or cancelled run leaves nothing. Desktop runs it as `frond-core --card ...`, Android
// through JNI on a worker thread.
namespace frondCardTool {

using Progress = frondSdCandidate::Progress;
using Result = frondSdCandidate::Result;
using frondSdCandidate::fail;

enum class Kind { sd, ms, msPro };

inline bool parseKind(const std::string& text, Kind& kind) {
    if (text == "sd") kind = Kind::sd;
    else if (text == "ms") kind = Kind::ms;
    else if (text == "mspro") kind = Kind::msPro;
    else return false;
    return true;
}

inline const char* kindName(Kind kind) { return kind == Kind::sd ? "sd" : kind == Kind::ms ? "ms" : "mspro"; }

// Sizes some device takes: SD within the bounds of the profiles with an SD slot (and the largest
// card Frond makes), Pro up to the largest msProMaxMib, classic sticks from the union of msSizes.
inline bool sdBytesTaken(uint64_t bytes) {
    if (!frondSdFile::validPayloadSize(bytes)) return false;
    for (const auto& profile : deviceProfiles5)
        if (profile.frontend.sdSlot && profile.board != DeviceBoard5::none && bytes >= profile.sdMin &&
            bytes <= profile.sdMax)
            return true;
    return false;
}

inline bool msProMibTaken(unsigned mib) {
    if (!frondMsPro::validCardMib(mib)) return false;
    for (const auto& profile : deviceProfiles5)
        if (profile.board != DeviceBoard5::none && mib >= 256 && mib <= profile.msProMaxMib) return true;
    return false;
}

inline bool msMegabytesTaken(uint64_t megabytes) {
    if (!frondMsGeometry::byMegabytes(megabytes)) return false;
    for (const auto& profile : deviceProfiles5)
        if (profile.board != DeviceBoard5::none && (profile.msSizes & megabytes)) return true;
    return false;
}

inline std::string sizeList(Kind kind) {
    std::string out;
    auto add = [&](unsigned value) { out += (out.empty() ? "" : ", ") + std::to_string(value); };
    if (kind == Kind::sd) {
        for (unsigned mib : frondSd::cardMibs)
            if (sdBytesTaken(uint64_t(mib) << 20)) add(mib);
        return out + " MiB";
    }
    if (kind == Kind::msPro) {
        for (unsigned mib : frondMsPro::cardMibs)
            if (msProMibTaken(mib)) add(mib);
        return out + " MiB";
    }
    for (const auto& geometry : frondMsGeometry::sizes)
        if (msMegabytesTaken(geometry.megabytes)) add(geometry.megabytes);
    return out + " MB";
}

inline std::filesystem::path folderOf(const std::filesystem::path& file) {
    auto dir = file.parent_path();
    return dir.empty() ? std::filesystem::path(".") : dir;
}

// The output's file system needs `bytes` and the usual spare.
inline Result checkSpace(const std::filesystem::path& dest, uint64_t bytes) {
    std::error_code ec;
    if (!std::filesystem::is_directory(folderOf(dest), ec)) return fail("write", "The output folder doesn't exist.");
    if (!frondSdCandidate::hasSpace(folderOf(dest), bytes)) return fail("space", "There isn't enough free space for the card.");
    return {true, "", {}};
}

inline uint64_t sdFileBytes(uint64_t payload) {
    return frondSdFile::headerBytes + payload + 4 * frondSdFile::chunkCount(payload);
}

// Progress and cancellation as the writers see them: a false from `progress` cancels.
struct Run {
    const Progress& progress;
    bool cancelled{false};
    bool report(uint64_t done, uint64_t total) {
        if (progress && !progress(done, total)) cancelled = true;
        return !cancelled;
    }
};

// Writes `size` bytes as a .frondsd v2 file from a reader of payload ranges.
template <typename ReadFn>
inline Result writeSd(const std::filesystem::path& dest, uint64_t size, Run& run, ReadFn read) {
    std::array<uint8_t, frondMachine::identityBytes> identity{};
    identity[0] = 1;  // the frontend gives the card its identity; this skips the MD5 pass
    bool readFailed = false;
    const bool ok = frondSession::writeSdFromReader(
        dest, size,
        [&](uint64_t off, uint64_t n, void* out) {
            if (!read(off, n, static_cast<uint8_t*>(out))) {
                readFailed = true;
                return false;
            }
            return run.report(off + n, size);
        },
        identity);
    if (!ok) {
        if (run.cancelled) return fail("cancelled", "Cancelled.");
        return readFailed ? fail("read", "Could not read the card image.") : fail("write", "Could not write the card file.");
    }
    frondSession::syncDir(folderOf(dest));
    return {true, "created", {}};
}

// Writes a classic stick's physical image as a v2 .frondms at generation 1.
inline Result writeMs(const std::filesystem::path& dest, const std::vector<uint8_t>& physical, Run& run) {
    std::vector<uint8_t> stored;
    if (!frondMs::storedOf(physical, stored)) return fail("image", "Use a valid Frond classic Memory Stick image.");
    std::array<uint8_t, frondMachine::identityBytes> identity{};
    identity[0] = 1;  // the frontend gives the card its identity; this skips the MD5 pass
    const bool ok = frondSession::writeSdFromReader(
        dest, stored.size(),
        [&](uint64_t off, uint64_t n, void* out) {
            std::memcpy(out, stored.data() + off, size_t(n));
            return run.report(off + n, stored.size());
        },
        identity, 1, frondSession::msShape(stored.size()));
    if (!ok) return run.cancelled ? fail("cancelled", "Cancelled.") : fail("write", "Could not write the card file.");
    frondSession::syncDir(folderOf(dest));
    return {true, "created", {}};
}

inline uint64_t msFileBytes(const frondMsGeometry::Geometry& g) {
    const auto stored = frondMs::layoutOf(g)->StoredBytes();
    return frondSdFile::headerBytes + stored + 4 * frondSdFile::chunkCount(stored);
}

inline unsigned parseNumber(const std::string& text) {
    if (text.empty() || text.size() > 5 || text.find_first_not_of("0123456789") != std::string::npos) return 0;
    return unsigned(std::stoul(text));
}

// A new formatted card of `size`: MiB for SD and Pro, MB for a classic stick.
inline Result make(Kind kind, unsigned size, const std::filesystem::path& dest, const Progress& progress = {}) {
    Run run{progress};
    if (kind == Kind::ms) {
        if (!msMegabytesTaken(size)) return fail("size", "Choose a classic Memory Stick of " + sizeList(kind) + ".");
        const auto* geometry = frondMsGeometry::byMegabytes(size);
        if (auto space = checkSpace(dest, msFileBytes(*geometry)); !space.ok) return space;
        return writeMs(dest, frondMs::blank(*geometry), run);
    }
    const bool taken = kind == Kind::sd ? frondSd::validCardMib(size) && sdBytesTaken(uint64_t(size) << 20)
                                        : msProMibTaken(size);
    if (!taken)
        return fail("size", std::string("Choose ") + (kind == Kind::sd ? "an SD card" : "a Memory Stick Pro") + " of " +
                                sizeList(kind) + ".");
    const uint64_t bytes = uint64_t(size) << 20;
    if (auto space = checkSpace(dest, sdFileBytes(bytes)); !space.ok) return space;
    const auto layout = frondSd::layout(size);
    size_t next = 0;
    return writeSd(dest, bytes, run, [&](uint64_t off, uint64_t n, uint8_t* out) {
        std::memset(out, 0, size_t(n));
        for (; next < layout.size() && uint64_t(layout[next].index) * 512 < off + n; ++next)
            std::memcpy(out + (uint64_t(layout[next].index) * 512 - off), layout[next].bytes.data(), 512);
        return true;
    });
}

inline bool frondMagic(const uint8_t* header) {
    return !std::memcmp(header, "FRONDSD", 7) || !std::memcmp(header, "FRONDMS", 7);
}

// A copy of a card image as a card file of `kind`: raw SD or Pro sectors, or a Frond classic stick file.
inline Result importImage(const std::filesystem::path& source, Kind kind, const std::filesystem::path& dest,
                          const Progress& progress = {}) {
    Run run{progress};
    struct stat st{};
    if (::stat(source.c_str(), &st) < 0 || !S_ISREG(st.st_mode)) return fail("read", "Could not read the card image.");
    std::error_code ec;
    if (std::filesystem::equivalent(source, dest, ec)) return fail("same", "The image and the card file are the same file.");
    const uint64_t bytes = uint64_t(st.st_size);
    if (kind == Kind::ms) {
        uint8_t header[frondMs::headerBytes]{};
        const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
        const bool haveHeader = fd >= 0 && frondSession::readAllAt(fd, header, sizeof(header), 0);
        if (fd >= 0) ::close(fd);
        const bool frondFile = haveHeader && !std::memcmp(header, "FRONDMS\0", 8);
        const frondMsGeometry::Geometry* geometry = nullptr;
        frondSdFile::Info info;
        if (frondFile) {
            // A v1 or v2 .frondms.
            const bool v2 = frondMs::get32(header + 8) == 2;
            geometry = v2 ? frondMs::v2Geometry(header, &info) : frondMs::headerGeometry(header);
            if (!geometry || bytes != (v2 ? info.fileBytes() : frondMs::fileBytes(*geometry)))
                return fail("size", "Use a Frond classic Memory Stick image of " + sizeList(kind) + ".");
        } else {
            if (haveHeader && frondMagic(header)) return fail("image", "That is a Frond card file, not a raw card image.");
            // A raw logical image, as CloudpilotEmu and a PC see the stick.
            geometry = frondMs::geometryForLogical(bytes);
            if (!geometry) return fail("size", "Use a raw Memory Stick image of " + sizeList(kind) + ", aligned to 512 bytes.");
        }
        if (!msMegabytesTaken(geometry->megabytes))
            return fail("size", "No device takes a " + std::to_string(geometry->megabytes) + " MB Memory Stick.");
        std::vector<uint8_t> image;
        if (frondFile) {
            if (!frondMs::readPhysical(source, image)) return fail("image", "Use a valid Frond classic Memory Stick image.");
        } else {
            std::vector<uint8_t> raw;
            if (!frondMachine::readBytes(source, raw) || raw.size() != bytes) return fail("read", "Could not read the card image.");
            image = frondMs::fromLogical(raw.data(), raw.size(), *geometry);
        }
        if (auto space = checkSpace(dest, msFileBytes(*geometry)); !space.ok) return space;
        return writeMs(dest, image, run);
    }
    const bool taken = kind == Kind::sd ? sdBytesTaken(bytes) : msProMibTaken(frondMsPro::mibOfBytes(bytes));
    if (!taken)
        return fail("size", kind == Kind::sd ? "Use a raw SD image of " + sizeList(kind) + ", aligned to 512 bytes."
                                              : "Use a raw Memory Stick Pro image of " + sizeList(kind) + ".");
    const int in = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return fail("read", "Could not read the card image.");
    struct Closer { int fd; ~Closer() { ::close(fd); } } closer{in};
    uint8_t first[8]{};
    if (!frondSession::readAllAt(in, first, sizeof(first), 0) || frondMagic(first))
        return fail("image", "That is a Frond card file, not a raw card image.");
    if (auto space = checkSpace(dest, sdFileBytes(bytes)); !space.ok) return space;
    return writeSd(dest, bytes, run, [&](uint64_t off, uint64_t n, uint8_t* out) {
        return frondSession::readAllAt(in, out, size_t(n), off);
    });
}

// A card file's payload as a pinned view, for the existing raw export.
class FileSnapshot : public CardSnapshot {
   public:
    FileSnapshot(int fd, uint64_t size) : fd_(fd), size_(size) {}
    uint64_t Size() const override { return size_; }
    uint64_t GenerationId() const override { return 0; }

   protected:
    bool ReadBytes(size_t offset, size_t length, void* dest) const override {
        return frondSession::readAllAt(fd_, dest, length, frondSdFile::headerBytes + offset);
    }

   private:
    int fd_;
    uint64_t size_;
};

inline Result fromStorage(const frondStorage::Result& result) { return {result.ok, result.code, result.message}; }

// Checks a .frondsd (v1 or v2) against its header and CRCs (frondStorage::verify); this is the
// first half of an export's progress.
inline Result verifySdFile(int fd, const frondSdFile::Info& info, uint64_t fileSize, const uint8_t*, Run& run) {
    if (fileSize != info.fileBytes()) return fail("image", "The card file is damaged.");
    frondSdFile::Info read;
    return fromStorage(frondStorage::verify(fd, read, [&](uint64_t done, uint64_t total) { return run.report(done, 2 * total); }));
}

// A card file's image: raw sectors for an SD card or Memory Stick Pro; for a classic stick its raw
// logical image, or a v2 .frondms when the image's name ends in .frondms. The card file is settled
// first.
inline Result exportCard(const std::filesystem::path& card, const std::filesystem::path& image,
                         const Progress& progress = {}) {
    Run run{progress};
    struct stat st{};
    if (::stat(card.c_str(), &st) < 0 || !S_ISREG(st.st_mode)) return fail("read", "Could not read the card file.");
    std::error_code ec;
    if (std::filesystem::equivalent(card, image, ec)) return fail("same", "The image and the card file are the same file.");
    const int fd = ::open(card.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return fail("read", "Could not read the card file.");
    struct Closer { int fd; ~Closer() { ::close(fd); } } closer{fd};
    uint8_t header[frondSdFile::headerBytes]{};
    if (!frondSession::readAllAt(fd, header, sizeof(header), 0)) return fail("image", "That is not a Frond card file.");
    const bool classic = !std::memcmp(header, "FRONDMS\0", 8);
    if (!std::memcmp(header, "FRONDSD", 7) || (classic && frondMs::get32(header + 8) == 2)) {
        if (auto settled = frondStorage::settle(card); !settled.ok) return fromStorage(settled);
    } else {
        const std::string journal = card.filename().string() + ".journal-";
        for (std::filesystem::directory_iterator it(folderOf(card), ec), end; !ec && it != end; it.increment(ec))
            if (it->path().filename().string().rfind(journal, 0) == 0)
                return fail("unsettled", frondStorage::unsettledMessage);
    }
    if (classic) {
        // A .frondms image is a v2 card file, as Cards imports it; anything else is the raw logical
        // image.
        std::vector<uint8_t> physical;
        if (!frondMs::readPhysical(card, physical)) return fail("image", "The card file is damaged.");
        const auto& g = *frondMsGeometry::byPhysicalBytes(physical.size());
        if (image.extension() == ".frondms") {
            if (auto space = checkSpace(image, msFileBytes(g)); !space.ok) return space;
            return writeMs(image, physical, run);
        }
        std::vector<uint8_t> stored, raw;
        LogicalMsMedia media;
        if (!frondMs::storedOf(physical, stored)) return fail("image", "The card file is damaged.");
        std::vector<uint8_t>().swap(physical);
        if (!frondMs::openStored(media, g, [&](uint64_t off, uint64_t n, void* dest) {
                std::memcpy(dest, stored.data() + off, size_t(n));
                return true;
            }) ||
            !frondMs::logicalOf(media, raw))
            return fail("image", "The card file is damaged.");
        if (auto space = checkSpace(image, raw.size()); !space.ok) return space;
        if (!run.report(raw.size() / 2, raw.size())) return fail("cancelled", "Cancelled.");
        if (!frondSession::writeAtomicFrond(image, raw.data(), raw.size())) return fail("write", "Could not write the image.");
        run.report(raw.size(), raw.size());
        return {true, "exported", {}};
    }
    frondSdFile::Info info;
    if (!frondSession::readAllAt(fd, header, sizeof(header), 0) || !frondSdFile::parseHeader(header, info))
        return fail("image", "That is not a Frond card file.");
    if (auto space = checkSpace(image, info.payloadBytes); !space.ok) return space;
    // Checking is the first half of the progress.
    if (auto verified = verifySdFile(fd, info, uint64_t(frondStorage::sizeOf(fd)), header, run); !verified.ok)
        return verified;
    FileSnapshot snapshot(fd, info.payloadBytes);
    auto result = frondSdCandidate::exportRaw(
        snapshot, image, [&](uint64_t done, uint64_t total) { return run.report(info.payloadBytes + done, 2 * total); });
    if (!result.ok && run.cancelled) return fail("cancelled", "Cancelled.");
    return result;
}

inline uint64_t sizeOf(const std::filesystem::path& file) {
    struct stat st{};
    return ::stat(file.c_str(), &st) == 0 ? uint64_t(st.st_size) : 0;
}

// Finishes what a cut-off change or the apps left beside a card file. "settled GENERATION".
inline Result settleCard(const std::filesystem::path& card) {
    struct stat st{};
    if (::stat(card.c_str(), &st) < 0 || !S_ISREG(st.st_mode)) return fail("read", "Could not read the card file.");
    // A v1 .frondms has no journals of its own; its change counter stands for the generation.
    if (frondMachine::msFileVersion(card) == 1) {
        uint8_t header[frondMs::headerBytes];
        std::ifstream in(card, std::ios::binary);
        if (!in.read(reinterpret_cast<char*>(header), sizeof(header)) || !frondMs::headerGeometry(header))
            return fail("image", "The card file is damaged.");
        return {true, "", "settled " + std::to_string(frondMs::headerChanges(header))};
    }
    if (auto settled = frondStorage::settle(card); !settled.ok) return fromStorage(settled);
    frondSdFile::Info info;
    if (!frondStorage::readHeader(card, info)) return fail("image", "The card file is damaged.");
    return {true, "", "settled " + std::to_string(info.generation)};
}

// ---- frond-68k linked cards ----
// frond-68k keeps a linked card's writes as .gen-<file>-* generations beside its file, bound to the
// file's device, inode and change time. These two move and settle them as the library needs.

// Holds the card file's lock (the one frond-68k holds while the card is in a running machine).
struct CardLock {
    int fd{-1};
    explicit CardLock(const std::filesystem::path& card) {
        fd = ::open(card.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) != 0) { ::close(fd); fd = -2; }
    }
    ~CardLock() { if (fd >= 0) ::close(fd); }
};

// The generation entries of `card`: its own .gen- names, with or without the root.
inline std::vector<std::filesystem::path> linkedGenerations(const std::filesystem::path& card) {
    auto dir = card.parent_path();
    if (dir.empty()) dir = ".";
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (FileCardBacking::GeneratedName(card.filename().string(), name) && !FileCardBacking::NeighbourName(card, name))
            found.push_back(it->path());
    }
    return found;
}

// Whether something a cut-off write left is beside `card`: a held generation, a generation temp,
// or a Files change or journal.
inline bool linkedUnsettled(const std::filesystem::path& card) {
    const auto tag = card.filename().string();
    for (const auto& path : linkedGenerations(card)) {
        const auto rest = path.filename().string().substr(6 + tag.size());
        if (!rest.compare(0, 4, "tmp-")) return true;
    }
    auto dir = card.parent_path();
    if (dir.empty()) dir = ".";
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (!name.compare(0, 6, ".held-") && FileCardBacking::GeneratedName(tag, name.substr(6))) return true;
    }
    std::error_code exists;
    if (std::filesystem::exists(dir / (".settling-" + tag), exists)) return true;
    return frondStorage::hasChanges(card) || !frondStorage::journalFiles(card).empty();
}

// "DEV INO MTIME_NS" of a file, as a settle mark records the settled file.
inline std::string fileStamp(const std::filesystem::path& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return {};
    return std::to_string(uint64_t(st.st_dev)) + ' ' + std::to_string(uint64_t(st.st_ino)) + ' ' +
           std::to_string(frondPosix::MtimeNs(st));
}
// A file's change time in nanoseconds, 0 if it can't be read.
inline uint64_t changedAt(const std::filesystem::path& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return 0;
    return frondPosix::MtimeNs(st);
}

// A settle-linked cut off part way, finished or undone. The settled file is written to
// .settled-<file>.tmp, then .settling-<file> marks it complete (with the temp's device, inode and
// change time), then it replaces the card and the generations go. With the mark: the temp still
// there means the card wasn't replaced (the temp goes, the generations stay); no temp means it was,
// and the generations go if the card is still the settled file and none is newer than the mark
// (newer ones were written after a cut-off settle, by a machine that took the card). Card lock held.
inline bool finishSettle(const std::filesystem::path& card) {
    auto dir = card.parent_path();
    if (dir.empty()) dir = ".";
    const auto tag = card.filename().string();
    const auto temp = dir / (".settled-" + tag + ".tmp"), mark = dir / (".settling-" + tag);
    std::error_code ec;
    const bool marked = std::filesystem::exists(mark, ec);
    const bool written = std::filesystem::exists(temp, ec);
    if (!marked) {
        if (written) std::filesystem::remove(temp, ec);
        return true;
    }
    if (written) std::filesystem::remove(temp, ec);
    else {
        std::string stamp;
        std::getline(std::ifstream(mark), stamp);
        const uint64_t markedAt = changedAt(mark);
        bool stale = !stamp.empty() && stamp == fileStamp(card) && markedAt;
        for (const auto& path : linkedGenerations(card))
            if (changedAt(path) > markedAt) stale = false;
        if (stale) FileCardBacking::DiscardGenerations(card);
    }
    if (!frondStorage::syncDir(dir)) return false;
    std::filesystem::remove(mark, ec);
    return !ec && frondStorage::syncDir(dir);
}

// Writes a linked card's generations into its file and drops them, as frond-68k's take-out does, so
// any machine can take it. KEY is the 32-hex key it was linked with. A card with none is left as it is.
inline Result settleLinked(const std::filesystem::path& card, const std::string& key) {
    if (key.size() != 32 || key.find_first_not_of("0123456789abcdef") != std::string::npos)
        return fail("usage", "The key is 32 lowercase hex digits.");
    CardLock lock(card);
    if (lock.fd == -1) return fail("read", "Could not read the card file.");
    if (lock.fd == -2) return fail("busy", "The card is in a running machine.");
    if (!finishSettle(card)) return fail("write", "Couldn't write the card file.");
    if (linkedUnsettled(card)) return fail("unsettled", frondStorage::unsettledMessage);
    auto dir = card.parent_path();
    if (dir.empty()) dir = ".";
    std::error_code ec;
    if (!std::filesystem::exists(dir / (".gen-" + card.filename().string() + "-root"), ec)) {
        if (!linkedGenerations(card).empty()) return fail("unsettled", frondStorage::unsettledMessage);
        return {true, "", "settled 0"};
    }
    frondSdFile::Info info;
    std::vector<uint32_t> table;
    if (frondMachine::readSdHeader(card, info, &table) != frondMachine::SiblingStatus::ok)
        return fail("image", "The card file is damaged.");
    const uint64_t size = info.classic ? info.stored() : info.payloadBytes;
    FileCardOptions options;
    options.rejectInvalidRoot = true;
    options.requireRoot = true;
    options.identity = key;
    options.chunkCrcs = table;
    auto backing = FileCardBacking::Open(card, size, frondSdFile::headerBytes, options);
    if (!backing) return fail("unsettled", "The card's saved changes can't be read.");
    const uint64_t generation = backing->RootGenerationId();
    std::array<uint8_t, frondMachine::identityBytes> identity{};
    const auto tag = card.filename().string();
    const auto temp = dir / (".settled-" + tag + ".tmp"), mark = dir / (".settling-" + tag);
    errno = 0;
    const bool ok = frondSession::writeSdFromReader(
        temp, backing->Size(), [&](uint64_t off, uint64_t n, void* out) { return backing->Read(off, n, out); }, identity, 1,
        info.classic ? frondSession::msShape(size) : frondSession::StoredShape{});
    const int writeError = errno;
    backing.reset();
    if (!ok || !frondStorage::syncDir(dir)) {
        std::filesystem::remove(temp, ec);
        if (!ok && (writeError == ENOSPC || writeError == EDQUOT))
            return fail("space", "Not enough free space to write the card's changes.");
        return fail("write", "Couldn't write the card file.");
    }
    // The mark, then the swap: see finishSettle.
    {
        const auto stamp = fileStamp(temp) + '\n';
        const int m = ::open(mark.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        const bool made = m >= 0 && stamp.size() > 1 && ::write(m, stamp.data(), stamp.size()) == ssize_t(stamp.size()) && ::fsync(m) == 0;
        if (m >= 0) ::close(m);
        if (!made || !frondStorage::syncDir(dir)) {
            std::filesystem::remove(mark, ec);
            std::filesystem::remove(temp, ec);
            return fail("write", "Couldn't write the card file.");
        }
    }
    if (::rename(temp.c_str(), card.c_str()) != 0) {
        std::filesystem::remove(temp, ec);
        std::filesystem::remove(mark, ec);
        return fail("write", "Couldn't write the card file.");
    }
    if (!frondStorage::syncDir(dir) || !finishSettle(card)) return fail("write", "Couldn't write the card file.");
    return {true, "", "settled " + std::to_string(generation)};
}

// Same file: device and inode.
inline bool sameInode(const std::filesystem::path& a, const std::filesystem::path& b) {
    #ifdef _WIN32
    (void)a; (void)b;  // no inode numbers here
    return false;
#else
    struct stat x{}, y{};
    return ::lstat(a.c_str(), &x) == 0 && ::lstat(b.c_str(), &y) == 0 && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
#endif
}
// Whether renameat2 may be called. Android's app seccomp filter only allows it from Android 11 (API
// 30); before that the call can kill the app (SIGSYS), so the fallbacks are used.
inline bool renameNoReplaceAllowed() {
#if defined(__ANDROID__)
    static const bool allowed = [] {
        char value[PROP_VALUE_MAX]{};
        return __system_property_get("ro.build.version.sdk", value) > 0 && std::atoi(value) >= 30;
    }();
    return allowed;
#else
    return true;
#endif
}

// A rename that never replaces `to`: renameat2 with RENAME_NOREPLACE, or where the kernel or the
// disk lacks it, a hard link and an unlink, or where there are no hard links either (exFAT), a
// rename after checking `to` is free (the card lock and the library lock keep others out). A crash
// between the link and the unlink leaves both names; the next call finishes it.
inline bool moveNoReplace(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (sameInode(from, to)) return ::unlink(from.c_str()) == 0;
#if defined(__linux__) && defined(SYS_renameat2)
    if (renameNoReplaceAllowed()) {
        if (::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 1u /* RENAME_NOREPLACE */) == 0) return true;
        if (errno == EEXIST) return false;
    }
#endif
#ifndef _WIN32
    if (::link(from.c_str(), to.c_str()) == 0) return ::unlink(from.c_str()) == 0;
    if (errno == EEXIST) return false;
#endif
    struct stat st{};
    if (::lstat(to.c_str(), &st) == 0 || errno != ENOENT) return false;
    return ::rename(from.c_str(), to.c_str()) == 0;
}

// Moves a linked card file and its generations to TO, under TO's name, never replacing a file. The
// file keeps its device, inode and change time, so a save that links it finds it there. On another
// disk only a card with no generations moves: copied, checked and then deleted. Safe to run again
// after a crash part way.
inline Result moveLinked(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    auto fromDir = from.parent_path(), toDir = to.parent_path();
    if (fromDir.empty()) fromDir = ".";
    if (toDir.empty()) toDir = ".";
    const bool fromHere = std::filesystem::exists(from, ec);
    const bool toHere = std::filesystem::exists(to, ec);
    if (fromHere && toHere && !sameInode(from, to)) return fail("exists", "There's already a card file there.");
    if (!fromHere && !toHere) return fail("read", "Could not read the card file.");
    CardLock lock(fromHere ? from : to);
    if (lock.fd == -1) return fail("read", "Could not read the card file.");
    if (lock.fd == -2) return fail("busy", "The card is in a running machine.");
    if (fromHere && !finishSettle(from)) return fail("write", "Couldn't write the card file.");
    if (fromHere && linkedUnsettled(from)) return fail("unsettled", frondStorage::unsettledMessage);
    struct stat a{}, b{};
    if (::stat(fromDir.c_str(), &a) != 0 || ::stat(toDir.c_str(), &b) != 0) return fail("write", "The output folder doesn't exist.");
    const auto generations = linkedGenerations(from);
    const auto tagFrom = from.filename().string(), tagTo = to.filename().string();
    if (a.st_dev != b.st_dev) {
        if (!generations.empty())
            return fail("disk", "A card with saved changes only moves on the same disk.");
        if (!fromHere) return {true, "", "moved"};
        const auto temp = toDir / (".move-" + tagTo + ".tmp");
        std::filesystem::remove(temp, ec);
        if (!std::filesystem::copy_file(from, temp, ec) || ec) { std::filesystem::remove(temp, ec); return fail("write", "Couldn't copy the card file."); }
        {
            const int out = ::open(temp.c_str(), O_RDONLY | O_CLOEXEC);
            const bool synced = out >= 0 && ::fsync(out) == 0;
            if (out >= 0) ::close(out);
            if (!synced) { std::filesystem::remove(temp, ec); return fail("write", "Couldn't copy the card file."); }
        }
        // Size and every byte the same.
        bool same = sizeOf(temp) == sizeOf(from);
        if (same) {
            std::ifstream x(from, std::ios::binary), y(temp, std::ios::binary);
            std::vector<char> p(1 << 20), q(1 << 20);
            while (same && x && y) {
                x.read(p.data(), p.size());
                y.read(q.data(), q.size());
                same = x.gcount() == y.gcount() && !std::memcmp(p.data(), q.data(), size_t(x.gcount()));
            }
            same = same && x.eof() && y.eof();
        }
        if (!same || !moveNoReplace(temp, to)) { std::filesystem::remove(temp, ec); return fail("write", "Couldn't copy the card file."); }
        std::filesystem::remove(temp, ec);
        frondStorage::syncDir(toDir);
        if (::unlink(from.c_str()) != 0) return fail("write", "The card was copied but the old file couldn't be removed.");
        frondStorage::syncDir(fromDir);
        return {true, "", "moved"};
    }
    // The generations, the root last, then the file.
    for (int rootPass = 0; rootPass < 2; ++rootPass)
        for (const auto& path : generations) {
            const auto name = path.filename().string();
            const bool root = name == ".gen-" + tagFrom + "-root";
            if (root != bool(rootPass)) continue;
            const auto dest = toDir / (".gen-" + tagTo + name.substr(5 + tagFrom.size()));
            if (std::filesystem::exists(dest, ec) && !sameInode(path, dest)) return fail("exists", "There's already a card file there.");
            if (!moveNoReplace(path, dest)) return fail("write", "Couldn't move the card's saved changes.");
        }
    if (fromHere && !moveNoReplace(from, to)) return fail("write", "Couldn't move the card file.");
    if (!frondStorage::syncDir(toDir) || !frondStorage::syncDir(fromDir)) return fail("write", "Couldn't move the card file.");
    return {true, "", "moved"};
}

// Checks every byte of a settled card file against its CRCs. "verified GENERATION".
inline Result verifyCard(const std::filesystem::path& card, const Progress& progress = {}) {
    Run run{progress};
    if (frondStorage::hasChanges(card) || !frondStorage::journalFiles(card).empty())
        return fail("unsettled", frondStorage::unsettledMessage);
    if (frondMachine::msFileVersion(card) == 1) {
        std::vector<uint8_t> physical;
        uint8_t header[frondMs::headerBytes];
        std::ifstream in(card, std::ios::binary);
        if (!in.read(reinterpret_cast<char*>(header), sizeof(header)) || !frondMs::decodeFile(card, physical))
            return fail("image", "The card file is damaged.");
        run.report(1, 1);
        return {true, "", "verified " + std::to_string(frondMs::headerChanges(header))};
    }
    frondSdFile::Info info;
    auto verified = frondStorage::verify(card, info, [&](uint64_t done, uint64_t total) { return run.report(done, total); });
    if (!verified.ok) return fromStorage(verified);
    return {true, "", "verified " + std::to_string(info.generation)};
}

// One operation from its words: make KIND SIZE OUTPUT, import IMAGE KIND OUTPUT, export CARD IMAGE,
// settle CARD, discard CARD (drops an unfinished change that doesn't match, then settles) or
// verify CARD. On success the message is "KIND BYTES" of the card file (for export, "image BYTES";
// for settle, discard and verify, "settled GENERATION" or "verified GENERATION").
inline Result run(const std::vector<std::string>& args, const Progress& progress = {}) {
    Kind kind;
    Result result;
    if (args.size() == 4 && args[0] == "make") {
        if (!parseKind(args[1], kind)) return fail("usage", "The card kind is sd, ms or mspro.");
        result = make(kind, parseNumber(args[2]), args[3], progress);
        if (result.ok) result.message = std::string(kindName(kind)) + " " + std::to_string(sizeOf(args[3]));
    } else if (args.size() == 4 && args[0] == "import") {
        if (!parseKind(args[2], kind)) return fail("usage", "The card kind is sd, ms or mspro.");
        result = importImage(args[1], kind, args[3], progress);
        if (result.ok) result.message = std::string(kindName(kind)) + " " + std::to_string(sizeOf(args[3]));
    } else if (args.size() == 2 && args[0] == "settle") {
        result = settleCard(args[1]);
    } else if (args.size() == 2 && args[0] == "discard") {
        if (auto dropped = frondStorage::discardMismatched(args[1]); !dropped.ok) return fromStorage(dropped);
        result = settleCard(args[1]);
    } else if (args.size() == 3 && args[0] == "settle-linked") {
        result = settleLinked(args[1], args[2]);
    } else if (args.size() == 3 && args[0] == "move-linked") {
        result = moveLinked(args[1], args[2]);
    } else if (args.size() == 2 && args[0] == "verify") {
        result = verifyCard(args[1], progress);
    } else if (args.size() == 3 && args[0] == "export") {
        result = exportCard(args[1], args[2], progress);
        if (result.ok) result.message = "image " + std::to_string(sizeOf(args[2]));
    } else {
        return fail("usage", "Use: make KIND SIZE OUTPUT, import IMAGE KIND OUTPUT, export CARD IMAGE, settle CARD, discard CARD, verify CARD, settle-linked CARD KEY or move-linked FROM TO.");
    }
    return result;
}

inline volatile std::sig_atomic_t& signalled() {
    static volatile std::sig_atomic_t flag = 0;
    return flag;
}

// `frond-core --card ...`: prints "progress DONE/TOTAL" (bytes, one line per percent) and then
// "ok KIND BYTES" or "error CODE MESSAGE". Exit 0 on success, 1 on failure, 2 on bad use.
// SIGINT and SIGTERM cancel and remove the partial output.
inline int cliMain(const std::vector<std::string>& args) {
#ifdef _WIN32
    signal(SIGINT, [](int) { signalled() = 1; });
    signal(SIGTERM, [](int) { signalled() = 1; });
#else
    struct sigaction action{};
    action.sa_handler = [](int) { signalled() = 1; };
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
#endif
    int lastPercent = -1;
    const auto result = run(args, [&](uint64_t done, uint64_t total) {
        if (signalled()) return false;
        const int percent = total ? int(done * 100 / total) : 100;
        if (percent != lastPercent) {
            lastPercent = percent;
            std::printf("progress %llu/%llu\n", (unsigned long long)done, (unsigned long long)total);
            std::fflush(stdout);
        }
        return true;
    });
    if (result.ok) std::printf("ok %s\n", result.message.c_str());
    else std::printf("error %s %s\n", result.code.c_str(), result.message.c_str());
    std::fflush(stdout);
    return result.ok ? 0 : result.code == "usage" ? 2 : 1;
}

}  // namespace frondCardTool

#endif
