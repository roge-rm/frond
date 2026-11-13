#ifndef FROND_SESSION_IO_H
#define FROND_SESSION_IO_H

#include "BackgroundJob.h"
#include "FileCardBacking.h"
#include "MachineFolder.h"
#include "md5.h"
#include "session/session_file5.h"
#include "storage/Io.h"
#include "storage/Journal.h"
#include "storage/Packed.h"
#include "storage/Settle.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "zip/miniz.h"

// v7 to v9 machine-folder I/O and streamed v6 extraction. Card payloads stay in
// sd.frondsd / ms.frondms / ms.frondmsp, internal NAND in nand.frondnand from v9; the .frond holds
// bounded state and descriptors. New saves are v9 with .frondsd v2; v7, v8 and .frondsd v1 still load.
namespace frondSession {
constexpr uint32_t msCardChunk = 0x0220;
constexpr size_t msV0PresentWord = 411;  // version + 410 protocol words
constexpr size_t msV0SizeWord = 413;     // version + protocol + present + protect
constexpr size_t inflateChunk = 65536;
constexpr uint64_t msPhysicalBytes = frondMachine::msPhysicalBytes;

struct Contents {
    std::string deviceName;
    int32_t displayMode{};
    uint32_t ramSize{};
    std::vector<uint8_t> metadata, nor, nand, memory, savestate;
    std::vector<frondMachine::Media> media;
    // Under --card-swap: the slot holds another card than the one saved, or none.
    bool sdSwapped{}, msSwapped{};
};

// A load with --card-swap: the frontend may have changed the card files while the machine was off.
// `identity` and `writeProtected` describe the card put in, given with --card-identity.
struct CardSwap {
    bool identityGiven{};
    std::array<uint8_t, frondMachine::identityBytes> identity{};
    bool writeProtected{};
};

inline uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32); }

inline bool identityEmpty(const std::array<uint8_t, frondMachine::identityBytes>& id) {
    return std::all_of(id.begin(), id.end(), [](uint8_t b) { return b == 0; });
}
inline void md5HexIdentity(const uint8_t digest[16], std::array<uint8_t, frondMachine::identityBytes>& identity) {
    static constexpr char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        identity[2 * i] = uint8_t(hex[digest[i] >> 4]);
        identity[2 * i + 1] = uint8_t(hex[digest[i] & 0xf]);
    }
}

using frondStorage::createTemp;
using frondStorage::removePath;

inline bool peekHeader(const uint8_t* data, size_t size, uint32_t& version, std::string& device) {
    if (!data || size < SessionFile5::kHeaderBytes) return false;
    if (le32(data) != SessionFile5::kMagic) return false;
    version = le32(data + 4);
    if (version < SessionFile5::kVersionV6 || version > SessionFile5::kCurrentVersion) return false;
    const char* field = reinterpret_cast<const char*>(data + 8);
    device.assign(field, strnlen(field, SessionFile5::kDeviceNameBytes));
    for (size_t i = device.size(); i < SessionFile5::kDeviceNameBytes; ++i)
        if (field[i]) return false;
    return !device.empty();
}

inline bool writeAtomicFrond(const std::filesystem::path& dest, const void* data, size_t size) {
    std::error_code ec;
    auto dir = dest.parent_path();
    if (dir.empty()) dir = ".";
    if (!std::filesystem::is_directory(dir, ec)) return false;
    std::string tmpl = (dir / ".frond.tmp-XXXXXX").string();
    std::vector<char> name(tmpl.begin(), tmpl.end());
    name.push_back(0);
    const int fd = mkstemp(name.data());
    if (fd < 0) return false;
    bool open = true;
    auto fail = [&] {
        if (open) close(fd);
        unlink(name.data());
        return false;
    };
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t left = size;
    while (left) {
        const auto n = write(fd, bytes, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return fail();
        bytes += n;
        left -= static_cast<size_t>(n);
    }
    if (fsync(fd) < 0) return fail();
    if (close(fd) < 0) {
        open = false;
        unlink(name.data());
        return false;
    }
    open = false;
    if (rename(name.data(), dest.c_str()) < 0) {
        unlink(name.data());
        return false;
    }
    const int folder = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (folder >= 0) {
        fsync(folder);
        ::close(folder);
    }
    return true;
}

inline bool isFolderVersion(uint32_t version) {
    return version == SessionFile5::kVersionV7 || version == SessionFile5::kVersionV8 ||
           version == SessionFile5::kVersionV9;
}

inline bool encodeMedia(const std::vector<frondMachine::Media>& cards, std::vector<uint8_t>& out,
                        size_t descriptorBytes = frondMachine::descriptorBytes) {
    out.clear();
    for (const auto& card : cards) {
        std::vector<uint8_t> one;
        if (!frondMachine::encodeDescriptor(card, one, descriptorBytes)) return false;
        out.insert(out.end(), one.begin(), one.end());
    }
    return true;
}

inline bool decodeMedia(const uint8_t* bytes, size_t size, std::vector<frondMachine::Media>& cards,
                        size_t descriptorBytes = frondMachine::descriptorBytes) {
    cards.clear();
    if (size % descriptorBytes) return false;
    for (size_t off = 0; off < size; off += descriptorBytes) {
        frondMachine::Media media;
        if (!frondMachine::decodeDescriptor(bytes + off, descriptorBytes, media))
            return false;
        cards.push_back(std::move(media));
    }
    return true;
}

inline bool fromSession(const SessionFile5& session, Contents& out) {
    auto copy = [](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        return n && b ? std::vector<uint8_t>(b, b + n) : std::vector<uint8_t>{};
    };
    Contents parsed;
    parsed.deviceName = session.GetDeviceName();
    parsed.displayMode = session.GetDisplayMode();
    parsed.ramSize = session.GetRamSize();
    parsed.metadata = copy(session.GetMetadata(), session.GetMetadataSize());
    parsed.nor = copy(session.GetNor(), session.GetNorSize());
    parsed.nand = copy(session.GetNand(), session.GetNandSize());
    parsed.memory = copy(session.GetMemory(), session.GetMemorySize());
    parsed.savestate = copy(session.GetSavestate(), session.GetSavestateSize());
    if (isFolderVersion(session.GetVersion())) {
        if (!decodeMedia(static_cast<const uint8_t*>(session.GetMedia()), session.GetMediaSize(),
                         parsed.media, SessionFile5::MediaDescriptorBytes(session.GetVersion())))
            return false;
    }
    out = std::move(parsed);
    return true;
}

// Writes a v9 .frond, or a v8 one when the NAND is still embedded (a v6 session just extracted).
inline bool saveV7(const std::filesystem::path& dest, const Contents& contents) {
    std::vector<uint8_t> media;
    if (!encodeMedia(contents.media, media)) return false;
    SessionFile5 session;
    session.SetDeviceName(contents.deviceName)
        .SetDisplayMode(contents.displayMode)
        .SetRamSize(contents.ramSize)
        .SetMetadata(contents.metadata.size(), contents.metadata.data())
        .SetNor(contents.nor.size(), contents.nor.data())
        .SetNand(contents.nand.size(), contents.nand.data())
        .SetMemory(contents.memory.size(), contents.memory.data())
        .SetSavestate(contents.savestate.size(), contents.savestate.data())
        .SetMedia(media.size(), media.data());
    const auto started = std::chrono::steady_clock::now();
    if (!session.SerializeAs(contents.nand.empty() ? SessionFile5::kVersionV9 : SessionFile5::kVersionV8)) return false;
    const auto compressed = std::chrono::steady_clock::now();
    const bool written = writeAtomicFrond(dest, session.GetSerializedSession(), session.GetSerializedSessionSize());
    if (saveTimingWanted())
        std::cerr << "save timing: compress " << std::chrono::duration<double, std::milli>(compressed - started).count()
                  << " ms, write " << saveTimingMs(compressed) << " ms, " << session.GetSerializedSessionSize()
                  << " bytes" << std::endl;
    return written;
}

using frondStorage::readAllAt;
using frondStorage::syncDir;
using frondStorage::writeAllAt;

// What a container file holds: its payload, and for a classic stick its side region as well.
struct StoredShape {
    uint64_t payloadBytes{}, sideBytes{};
    bool classic{};
    bool nand{};
    uint64_t stored() const { return frondSdFile::storedBytes(payloadBytes, sideBytes); }
    unsigned magic() const { return nand ? frondSdFile::magicNand : classic ? frondSdFile::magicMs : frondSdFile::magicSd; }
};
inline StoredShape sdShape(uint64_t payloadBytes) { return {payloadBytes, 0, false}; }
// nand.frondnand for a raw NAND of `rawBytes`.
inline StoredShape nandShape(uint64_t rawBytes) {
    return frondMachine::validNandBytes(rawBytes)
               ? StoredShape{frondMachine::nandMainBytes(rawBytes), frondMachine::nandSpareBytes(rawBytes), false, true}
               : StoredShape{};
}
// The v2 .frondms shape of stored bytes this long, or an empty one.
inline StoredShape msShape(uint64_t storedBytes) {
    const auto* l = frondMsLayout::byStoredBytes(storedBytes);
    return l ? StoredShape{l->PayloadBytes(), l->SideBytes(), true} : StoredShape{};
}

// Table and header of a v2 container whose stored bytes are already written after the header, then fsync.
inline bool finishSdV2(int fd, const StoredShape& shape, uint64_t generation, const std::vector<uint32_t>& table) {
    std::vector<uint8_t> tableBytes;
    frondSdFile::tableBytes(table, tableBytes);
    uint8_t header[frondSdFile::headerBytes];
    frondSdFile::makeHeaderV2(header, shape.payloadBytes, generation, table, shape.sideBytes, shape.magic());
    return table.size() == frondSdFile::chunkCount(shape.stored()) &&
           writeAllAt(fd, tableBytes.data(), tableBytes.size(), frondSdFile::headerBytes + shape.stored()) &&
           writeAllAt(fd, header, sizeof(header), 0) && fsync(fd) == 0;
}
inline bool finishSdV2(int fd, uint64_t size, uint64_t generation, const std::vector<uint32_t>& table) {
    return finishSdV2(fd, sdShape(size), generation, table);
}

inline bool writeSdFile(const std::filesystem::path& path, const uint8_t* payload, uint64_t size) {
    std::vector<uint8_t> file;
    if (!frondSdFile::encode(payload, size, file)) return false;
    return writeAtomicFrond(path, file.data(), file.size());
}

// Stream sectors to a .frondsd v2 at `generation` without a whole-card vector. With a classic or
// NAND `shape`, `size` is its stored bytes and the file is a v2 .frondms or nand.frondnand.
template <typename ReadFn>
inline bool writeSdFromReader(const std::filesystem::path& path, uint64_t size, ReadFn read,
                              std::array<uint8_t, frondMachine::identityBytes>& identity,
                              uint64_t generation = 1, StoredShape shape = {}) {
    if (!shape.classic && !shape.nand) shape = sdShape(size);
    if (!frondSdFile::validPayloadSize(shape.payloadBytes) || shape.stored() != size) return false;
    auto dir = path.parent_path();
    if (dir.empty()) dir = ".";
    std::string tmpl = (dir / ".frondsd.tmp-XXXXXX").string();
    std::vector<char> name(tmpl.begin(), tmpl.end());
    name.push_back(0);
    const int fd = mkstemp(name.data());
    if (fd < 0) return false;
    uint8_t header[frondSdFile::headerBytes]{};
    if (write(fd, header, sizeof(header)) != ssize_t(sizeof(header))) {
        close(fd);
        unlink(name.data());
        return false;
    }
    // 1 MiB at a time; the MD5 is only for a card with no identity yet (a small card).
    std::vector<uint8_t> chunk(size_t(std::min<uint64_t>(size, 1u << 20)));
    frondSdFile::ChunkCrcs crcs;
    MD5Context digest{};
    md5Init(&digest);
    const bool needDigest = identityEmpty(identity);
    for (uint64_t off = 0; off < size; off += chunk.size()) {
        const auto want = size_t(std::min<uint64_t>(chunk.size(), size - off));
        if (!read(off, want, chunk.data())) {
            close(fd);
            unlink(name.data());
            return false;
        }
        crcs.add(chunk.data(), want);
        if (needDigest) md5Update(&digest, chunk.data(), want);
        size_t wrote = 0;
        while (wrote < want) {
            const auto n = write(fd, chunk.data() + wrote, want - wrote);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                close(fd);
                unlink(name.data());
                return false;
            }
            wrote += size_t(n);
        }
    }
    md5Finalize(&digest);
    const bool finished = finishSdV2(fd, shape, generation, crcs.table());
    const bool closed = close(fd) == 0;
    if (!finished || !closed || rename(name.data(), path.c_str()) < 0) {
        unlink(name.data());
        return false;
    }
    if (identityEmpty(identity)) md5HexIdentity(digest.digest, identity);
    return true;
}

// The header geometry for a payload of size bytes; the 128 MB one for sizes outside the table,
// which only the v6 extraction test stubs use.
inline const frondMsGeometry::Geometry& msGeometryFor(uint64_t size) {
    const auto* g = frondMsGeometry::byPhysicalBytes(size);
    return g ? *g : frondMsGeometry::largest;
}

// Streams a classic Memory Stick image to a .frondms in 64 KiB chunks, with change counter `changes`.
template <typename ReadFn>
inline bool writeMsFromReader(const std::filesystem::path& path, uint64_t size, ReadFn read, uint64_t changes = 0) {
    auto dir = path.parent_path();
    if (dir.empty()) dir = ".";
    std::string tmpl = (dir / ".frondms.tmp-XXXXXX").string();
    std::vector<char> name(tmpl.begin(), tmpl.end());
    name.push_back(0);
    const int fd = mkstemp(name.data());
    if (fd < 0) return false;
    auto fail = [&] {
        close(fd);
        unlink(name.data());
        return false;
    };
    auto put = [&](const uint8_t* p, size_t n) {
        while (n) {
            const auto w = write(fd, p, n);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return false;
            p += w;
            n -= size_t(w);
        }
        return true;
    };
    uint8_t header[frondSdFile::headerBytes]{};
    if (!put(header, sizeof(header))) return fail();
    std::vector<uint8_t> chunk(inflateChunk);
    uint32_t crc = 0;
    for (uint64_t off = 0; off < size;) {
        const size_t n = size_t(std::min<uint64_t>(chunk.size(), size - off));
        if (!read(off, n, chunk.data())) return fail();
        crc = mz_crc32(crc, chunk.data(), n);
        if (!put(chunk.data(), n)) return fail();
        off += n;
    }
    std::memcpy(header, "FRONDMS", 8);
    frondSdFile::put32(header + 8, 1);
    frondSdFile::put32(header + 12, msGeometryFor(size).blocks);
    frondSdFile::put32(header + 16, msGeometryFor(size).pagesPerBlock);
    frondSdFile::put32(header + 20, 512);
    frondSdFile::put32(header + 24, 9);
    frondSdFile::put32(header + 28, crc);
    frondSdFile::put32(header + 32, uint32_t(changes));
    frondSdFile::put32(header + 36, uint32_t(changes >> 32));
    if (lseek(fd, 0, SEEK_SET) < 0 || !put(header, sizeof(header)) || fsync(fd) < 0) return fail();
    if (close(fd) < 0 || rename(name.data(), path.c_str()) < 0) {
        unlink(name.data());
        return false;
    }
    return true;
}

inline bool writeMsFile(const std::filesystem::path& path, const uint8_t* payload, uint64_t size) {
    if (!payload || size > std::numeric_limits<size_t>::max()) return false;
    const auto n = static_cast<size_t>(size);
    std::vector<uint8_t> file(frondSdFile::headerBytes + n);
    std::memcpy(file.data(), "FRONDMS", 8);
    frondSdFile::put32(file.data() + 8, 1);
    frondSdFile::put32(file.data() + 12, msGeometryFor(size).blocks);
    frondSdFile::put32(file.data() + 16, msGeometryFor(size).pagesPerBlock);
    frondSdFile::put32(file.data() + 20, 512);
    frondSdFile::put32(file.data() + 24, 9);
    frondSdFile::put32(file.data() + 28, frondSdFile::crc32(payload, n));
    std::memcpy(file.data() + frondSdFile::headerBytes, payload, n);
    return writeAtomicFrond(path, file.data(), file.size());
}

inline uint32_t savestateWord(const uint8_t* data, size_t words, size_t index) {
    if (index >= words) return 0;
    return le32(data + index * 4);
}

// Test seam: imageBytes defaults to classic physical size. Tests inject a stub.
inline bool extractMsFromSavestate(const uint8_t* data, size_t size, const std::filesystem::path& dest,
                                   uint64_t imageBytes = msPhysicalBytes, bool* found = nullptr) {
    if (found) *found = false;
    if (!data || size < 4 || size % 4) return true;
    const size_t words = size / 4;
    const uint32_t count = savestateWord(data, words, 0);
    if (uint64_t(1) + uint64_t(count) * 2 > words) return false;
    size_t offset = 1 + size_t(count) * 2;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t type = savestateWord(data, words, 1 + i * 2);
        const uint32_t n = savestateWord(data, words, 2 + i * 2);
        if (offset + n > words) return false;
        if (type == msCardChunk) {
            if (n < msV0SizeWord + 2) return true;
            const uint32_t version = savestateWord(data, words, offset);
            if (version != 0) return true;
            const uint32_t present = savestateWord(data, words, offset + msV0PresentWord);
            if (!present) return true;
            const uint32_t packed = savestateWord(data, words, offset + msV0SizeWord);
            const uint32_t crc = savestateWord(data, words, offset + msV0SizeWord + 1);
            if (!packed || offset + msV0SizeWord + 2 > words) return false;
            const uint8_t* compressed = data + (offset + msV0SizeWord + 2) * 4;
            const size_t packedBytes = packed;
            if (compressed + packedBytes > data + size) return false;
            if (imageBytes > std::numeric_limits<size_t>::max()) return false;
            std::vector<uint8_t> physical(static_cast<size_t>(imageBytes));
            mz_stream stream{};
            stream.next_in = const_cast<uint8_t*>(compressed);
            stream.avail_in = packedBytes;
            stream.next_out = physical.data();
            stream.avail_out = physical.size();
            if (mz_inflateInit(&stream) != MZ_OK) return false;
            const auto result = mz_inflate(&stream, MZ_FINISH);
            const bool ok = result == MZ_STREAM_END && stream.total_in == packedBytes &&
                            stream.total_out == physical.size();
            mz_inflateEnd(&stream);
            if (!ok || frondSdFile::crc32(physical.data(), physical.size()) != crc) return false;
            if (found) *found = true;
            return writeMsFile(dest, physical.data(), physical.size());
        }
        offset += n;
    }
    return true;
}

inline bool fillSection(mz_stream& stream, std::vector<uint8_t>& dest) {
    if (dest.empty()) return true;
    stream.next_out = dest.data();
    stream.avail_out = dest.size();
    while (stream.avail_out) {
        const int r = mz_inflate(&stream, MZ_NO_FLUSH);
        if (r == MZ_STREAM_END) break;
        if (r != MZ_OK && r != MZ_BUF_ERROR) return false;
        if (r == MZ_BUF_ERROR && stream.avail_in == 0) return false;
    }
    return stream.avail_out == 0;
}

inline bool parseV6Toc(const uint8_t* file, size_t size, Contents& parsed, uint32_t& sdSize) {
    if (size < SessionFile5::kHeaderBytes + 32) return false;
    uint32_t version = 0;
    std::string device;
    if (!peekHeader(file, size, version, device) || version != SessionFile5::kVersionV6) return false;
    const uint8_t* p = file + SessionFile5::kHeaderBytes;
    parsed.deviceName = std::move(device);
    parsed.displayMode = int32_t(le32(p));
    parsed.ramSize = le32(p + 4);
    const uint32_t metadataSize = le32(p + 8);
    const uint32_t norSize = le32(p + 12);
    const uint32_t nandSize = le32(p + 16);
    const uint32_t memorySize = le32(p + 20);
    const uint32_t savestateSize = le32(p + 24);
    sdSize = le32(p + 28);
    const uint64_t bounded =
        uint64_t(metadataSize) + norSize + nandSize + memorySize + savestateSize;
    if (bounded > SessionFile5::kMaxInflatedBytes) return false;
    if (sdSize > SessionFile5::kMaxSdBytes || sdSize % 512) return false;
    parsed.metadata.resize(metadataSize);
    parsed.nor.resize(norSize);
    parsed.nand.resize(nandSize);
    parsed.memory.resize(memorySize);
    parsed.savestate.resize(savestateSize);
    return true;
}

inline bool streamInflateV6(const uint8_t* file, size_t size, Contents& out, const std::filesystem::path& sdPath,
                            std::string& sdIdentity) {
    Contents parsed;
    uint32_t sdSize = 0;
    if (!parseV6Toc(file, size, parsed, sdSize)) return false;
    if (sdSize && !frondSdFile::validPayloadSize(sdSize)) return false;

    mz_stream stream{};
    if (mz_inflateInit(&stream) != MZ_OK) return false;
    stream.next_in = const_cast<uint8_t*>(file + SessionFile5::kHeaderBytes + 32);
    stream.avail_in = size - (SessionFile5::kHeaderBytes + 32);
    const bool sections = fillSection(stream, parsed.metadata) && fillSection(stream, parsed.nor) &&
                          fillSection(stream, parsed.nand) && fillSection(stream, parsed.memory) &&
                          fillSection(stream, parsed.savestate);
    if (!sections) {
        mz_inflateEnd(&stream);
        return false;
    }

    if (sdSize) {
        auto dir = sdPath.parent_path();
        if (dir.empty()) dir = ".";
        std::string tmpl = (dir / ".frondsd.tmp-XXXXXX").string();
        std::vector<char> name(tmpl.begin(), tmpl.end());
        name.push_back(0);
        const int fd = mkstemp(name.data());
        if (fd < 0) {
            mz_inflateEnd(&stream);
            return false;
        }
        uint8_t header[frondSdFile::headerBytes]{};
        if (write(fd, header, sizeof(header)) != ssize_t(sizeof(header))) {
            close(fd);
            unlink(name.data());
            mz_inflateEnd(&stream);
            return false;
        }
        uint8_t chunk[inflateChunk];
        frondSdFile::ChunkCrcs crcs;
        MD5Context digest{};
        md5Init(&digest);
        size_t left = sdSize;
        while (left) {
            stream.next_out = chunk;
            stream.avail_out = std::min(sizeof(chunk), left);
            const int r = mz_inflate(&stream, stream.avail_in ? MZ_NO_FLUSH : MZ_FINISH);
            const size_t produced = std::min(sizeof(chunk), left) - stream.avail_out;
            if (!produced) {
                close(fd);
                unlink(name.data());
                mz_inflateEnd(&stream);
                return false;
            }
            crcs.add(chunk, produced);
            md5Update(&digest, chunk, produced);
            size_t off = 0;
            while (off < produced) {
                const auto n = write(fd, chunk + off, produced - off);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) {
                    close(fd);
                    unlink(name.data());
                    mz_inflateEnd(&stream);
                    return false;
                }
                off += size_t(n);
            }
            left -= produced;
            if (r != MZ_OK && r != MZ_STREAM_END && r != MZ_BUF_ERROR) {
                close(fd);
                unlink(name.data());
                mz_inflateEnd(&stream);
                return false;
            }
        }
        md5Finalize(&digest);
        if (!finishSdV2(fd, sdSize, 1, crcs.table()) || close(fd) < 0 ||
            rename(name.data(), sdPath.c_str()) < 0) {
            unlink(name.data());
            mz_inflateEnd(&stream);
            return false;
        }
        frondMachine::Media media;
        media.kind = frondMachine::kindSd;
        media.name = frondMachine::sdSibling;
        media.inserted = true;
        media.logicalSize = sdSize;
        media.physicalSize = sdSize;
        media.contentGeneration = 1;
        md5HexIdentity(digest.digest, media.identity);
        sdIdentity.assign(reinterpret_cast<const char*>(media.identity.data()), 32);
        parsed.media.push_back(std::move(media));
    }

    uint8_t tail;
    stream.next_out = &tail;
    stream.avail_out = 1;
    const int end = mz_inflate(&stream, MZ_FINISH);
    mz_inflateEnd(&stream);
    if (end != MZ_STREAM_END || stream.avail_out == 0) return false;
    out = std::move(parsed);
    return true;
}

// SD and Memory Stick Pro save journals, <card>.journal-<to generation> beside the folder's card
// file, in the storage library's format (storage/Journal.h). Saves write session journals.
constexpr size_t journalHeaderBytes = frondStorage::journalHeaderBytes;
constexpr size_t journalTrailerBytes = frondStorage::journalTrailerBytes;
constexpr size_t journalRecordBytes = frondStorage::journalRecordBytes;

// `kind` picks the card file: SD (sd.frondsd) or Memory Stick Pro (ms.frondmsp), which share all of this.
inline std::filesystem::path fileCardPath(const std::filesystem::path& folder, uint32_t kind) {
    return folder / frondMachine::fileCardSibling(kind);
}

inline std::filesystem::path sdJournalPath(const std::filesystem::path& folder, uint64_t toGeneration,
                                           uint32_t kind = frondMachine::kindSd) {
    return frondStorage::journalPath(fileCardPath(folder, kind), toGeneration);
}

// A full copy at `generation`, fsynced under this name before the .frond that describes it is
// published, then renamed onto sd.frondsd. Shares the journal prefix so frontends leave it alone.
inline std::filesystem::path sdFullPath(const std::filesystem::path& folder, uint64_t generation,
                                        uint32_t kind = frondMachine::kindSd) {
    return frondStorage::fullPath(fileCardPath(folder, kind), generation);
}

inline std::vector<std::filesystem::path> sdJournals(const std::filesystem::path& folder,
                                                     uint32_t kind = frondMachine::kindSd) {
    return frondStorage::journalFiles(fileCardPath(folder, kind));
}

inline void removeSdJournals(const std::filesystem::path& folder, const std::filesystem::path& keep = {},
                             uint32_t kind = frondMachine::kindSd) {
    frondStorage::removeJournalFiles(fileCardPath(folder, kind), keep);
}

// Builds a session journal in a temp: records in any order, then finish() with the new table.
class SdJournalWriter {
   public:
    bool open(const std::filesystem::path& folder, uint64_t payloadBytes, uint64_t from, uint64_t to,
              uint32_t kind = frondMachine::kindSd) {
        return writer_.open(fileCardPath(folder, kind), payloadBytes, from, to);
    }
    bool add(uint64_t offset, uint32_t length, const uint8_t* data) { return writer_.add(offset, length, data); }
    bool finish(const std::vector<uint32_t>& table, std::filesystem::path& journal) {
        return writer_.finish(table, journal);
    }
    void abandon() { writer_.abandon(); }

   private:
    frondStorage::JournalWriter writer_;
};

using SdJournal = frondStorage::Journal;
using frondStorage::JournalStep;

inline bool readSdJournal(int fd, uint64_t size, SdJournal& out) { return frondStorage::readJournal(fd, size, out); }
inline bool readSdJournal(const std::filesystem::path& path, SdJournal& out) {
    return frondStorage::readJournal(path, out);
}

inline int64_t& sdJournalFailAfter() { return frondStorage::journalFailAfter(); }

// Applies a complete journal to the card file in place (storage/Journal.h applyJournal).
inline bool applySdJournal(const std::filesystem::path& folder, const std::filesystem::path& journal,
                           std::vector<uint32_t>* tableOut = nullptr, JournalStep stop = JournalStep::all,
                           uint32_t kind = frondMachine::kindSd) {
    return frondStorage::applyJournal(fileCardPath(folder, kind), journal, tableOut, stop);
}

// Brings the folder's card file to the generation the .frond describes (storage/Settle.h): what
// Files… or a standalone change left is finished first, then the session save.
inline bool settleSdJournal(const std::filesystem::path& folder, const frondMachine::Media& card) {
    const auto result =
        frondStorage::settle(fileCardPath(folder, card.kind), card.contentGeneration, frondMachine::containerPayload(card));
    if (!result.ok && result.code != "unsettled") std::cerr << result.message << std::endl;
    return result.ok;
}

inline bool sessionPathIn(const std::filesystem::path& folder, std::filesystem::path& dest) {
    if (frondMachine::sessionFile(folder, dest)) return true;
    dest = folder / (folder.filename().string() + ".frond");
    return true;
}

inline bool saveFolder(const std::filesystem::path& folder, const Contents& contents) {
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) return false;
    if (frondMachine::checkSiblings(folder, contents.media) != frondMachine::SiblingStatus::ok)
        return false;
    std::filesystem::path dest;
    sessionPathIn(folder, dest);
    return saveV7(dest, contents);
}

// Write the .frond temp, rename it, then rename sidecar temps. A failed .frond
// rename leaves previous card files; sidecar temps are ignored names. `published` is set once the
// .frond is renamed, so a caller can tell a later failure from one that changed nothing. Card
// files named in `keep` stay even when the .frond no longer describes them.
inline bool saveFolderFromTemps(const std::filesystem::path& folder, const Contents& contents,
                                const std::vector<std::filesystem::path>& sidecarTemps,
                                bool* published = nullptr, const std::vector<std::string>* keep = nullptr) {
    if (published) *published = false;
    if (sidecarTemps.size() != contents.media.size()) return false;
    std::filesystem::path dest;
    sessionPathIn(folder, dest);
    std::filesystem::path frondTmp;
    auto fail = [&] {
        removePath(frondTmp);
        for (const auto& t : sidecarTemps) removePath(t);
        return false;
    };
    if (!createTemp(folder, ".frond.tmp-", frondTmp)) return fail();
    if (!saveV7(frondTmp, contents)) return fail();
    if (rename(frondTmp.c_str(), dest.c_str()) < 0) return fail();
    frondTmp.clear();
    if (published) *published = true;
    // The .frond rename must reach the disk before a card file is changed in place.
    syncDir(folder);
    for (size_t i = 0; i < sidecarTemps.size(); ++i) {
        if (sidecarTemps[i].empty()) continue;
        const auto live = folder / contents.media[i].name;
        if (rename(sidecarTemps[i].c_str(), live.c_str()) < 0) {
            removePath(sidecarTemps[i]);
            return false;
        }
    }
    // A card the new .frond no longer describes, after an unmount or detach, goes with its
    // generations, or the folder would refuse to load.
    for (const auto* sibling :
         {frondMachine::sdSibling, frondMachine::msSibling, frondMachine::msProSibling, frondMachine::nandSibling}) {
        const bool described = std::any_of(contents.media.begin(), contents.media.end(),
                                           [&](const frondMachine::Media& m) { return m.name == sibling; });
        if (described || (keep && std::find(keep->begin(), keep->end(), sibling) != keep->end())) continue;
        std::error_code ec;
        if (std::filesystem::exists(folder / sibling, ec) && !std::filesystem::remove(folder / sibling, ec))
            return false;
        FileCardBacking::DiscardGenerations(folder / sibling);
    }
    return true;
}

inline bool parseFolderSession(const uint8_t* data, size_t size, Contents& out) {
    SessionFile5 session;
    if (!session.Deserialize(size, data) || !isFolderVersion(session.GetVersion())) return false;
    return fromSession(session, out);
}

// A card file the frontend put in under --card-swap, checked as a new card of its kind: what Files…
// or a standalone change left beside it is settled, then header, size and payload as a load does,
// and no session journals or full copies beside it.
inline bool swappedInCard(const std::filesystem::path& folder, const char* name, const CardSwap& swap,
                          frondMachine::Media& out) {
    if (!swap.identityGiven) {
        std::cerr << "a card put in needs --card-identity" << std::endl;
        return false;
    }
    frondMachine::Media media;
    media.name = name;
    media.inserted = true;
    media.writeProtected = swap.writeProtected;
    media.identity = swap.identity;
    const auto path = folder / name;
    std::error_code ec;
    if (media.name == frondMachine::msSibling && frondMachine::msFileVersion(path) == 2) {
        media.kind = frondMachine::kindMs;
        if (const auto settled = frondStorage::settleChanges(path); !settled.ok) {
            std::cerr << settled.message << std::endl;
            return false;
        }
        if (!sdJournals(folder, media.kind).empty()) {
            std::cerr << "the card put in has unfinished save files beside it" << std::endl;
            return false;
        }
        frondSdFile::Info info;
        const auto* layout = frondMachine::readSdHeader(path, info) == frondMachine::SiblingStatus::ok
                                 ? frondMsLayout::of(info) : nullptr;
        const auto* g = layout ? frondMsGeometry::byShape(layout->blocks, layout->pagesPerBlock) : nullptr;
        if (!g) return false;
        media.physicalSize = g->PhysicalBytes();
        media.logicalSize = g->LogicalBytes();
        media.contentGeneration = info.generation;
        if (!frondMachine::validMedia(media) || frondMachine::checkSdSibling(path, media) != frondMachine::SiblingStatus::ok)
            return false;
    } else if (media.name == frondMachine::msSibling) {
        media.kind = frondMachine::kindMs;
        const auto bytes = std::filesystem::file_size(path, ec);
        if (ec || bytes < frondSdFile::headerBytes) return false;
        const auto* g = frondMsGeometry::byPhysicalBytes(bytes - frondSdFile::headerBytes);
        if (!g) return false;
        media.physicalSize = g->PhysicalBytes();
        media.logicalSize = g->LogicalBytes();
        if (!frondMachine::msHeaderLooksRight(path, media.physicalSize, &media.contentGeneration)) return false;
    } else {
        media.kind = media.name == frondMachine::msProSibling ? frondMachine::kindMsPro : frondMachine::kindSd;
        if (const auto settled = frondStorage::settleChanges(path); !settled.ok) {
            std::cerr << settled.message << std::endl;
            return false;
        }
        if (!sdJournals(folder, media.kind).empty()) {
            std::cerr << "the card put in has unfinished save files beside it" << std::endl;
            return false;
        }
        frondSdFile::Info info;
        if (frondMachine::readSdHeader(path, info) != frondMachine::SiblingStatus::ok) return false;
        media.physicalSize = media.logicalSize = info.payloadBytes;
        media.contentGeneration = info.generation;
        if (!frondMachine::validMedia(media) || frondMachine::checkSdSibling(path, media) != frondMachine::SiblingStatus::ok)
            return false;
    }
    if (!frondMachine::validMedia(media)) return false;
    out = std::move(media);
    return true;
}

// Fits the saved cards to the card files now in the folder, slot by slot (SD; Memory Stick or Pro).
// A saved card whose file is there unchanged, with the same identity when one is given, stays as
// saved: same SD or Pro generation, same classic stick change counter. A missing file empties the
// slot; another card, or the same one changed, is checked as a new card. The device profile check
// comes later, as for any load.
inline bool resolveCardSwap(const std::filesystem::path& folder, Contents& parsed, const CardSwap& swap) {
    std::vector<frondMachine::Media> cards;
    const std::vector<std::vector<const char*>> slots = {{frondMachine::sdSibling},
                                                         {frondMachine::msSibling, frondMachine::msProSibling}};
    for (size_t slot = 0; slot < slots.size(); ++slot) {
        const frondMachine::Media* saved = nullptr;
        for (const auto& media : parsed.media) {
            if (media.kind == frondMachine::kindNand) continue;
            const bool sd = media.kind == frondMachine::kindSd;
            if (sd == (slot == 0)) {
                if (saved) return false;
                saved = &media;
            }
        }
        const char* present = nullptr;
        for (const auto* name : slots[slot]) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(folder / name, ec)) continue;
            if (present) {
                std::cerr << "the folder has a Memory Stick and a Memory Stick Pro" << std::endl;
                return false;
            }
            present = name;
        }
        if (!saved && !present) continue;
        bool& swapped = slot == 0 ? parsed.sdSwapped : parsed.msSwapped;
        if (saved && present && saved->name == present && (!swap.identityGiven || swap.identity == saved->identity)) {
            std::error_code ec;
            const auto path = folder / present;
            uint64_t changes = 0;
            const bool v2 = saved->kind == frondMachine::kindMs && frondMachine::msFileVersion(path) == 2;
            const bool same = frondMachine::fileCardKind(saved->kind) || v2
                                  ? settleSdJournal(folder, *saved) &&
                                        frondMachine::checkSdSibling(path, *saved) == frondMachine::SiblingStatus::ok
                                  : std::filesystem::file_size(path, ec) == frondSdFile::headerBytes + saved->physicalSize &&
                                        !ec && frondMachine::msHeaderLooksRight(path, saved->physicalSize, &changes) &&
                                        changes == saved->contentGeneration;
            if (same) {
                cards.push_back(*saved);
                continue;
            }
        }
        swapped = true;
        if (!present) continue;
        frondMachine::Media card;
        if (!swappedInCard(folder, present, swap, card)) return false;
        cards.push_back(std::move(card));
    }
    // Internal NAND isn't in a slot; it loads as saved, settled first.
    for (const auto& media : parsed.media)
        if (media.kind == frondMachine::kindNand) {
            if (!settleSdJournal(folder, media)) return false;
            cards.push_back(media);
        }
    parsed.media = std::move(cards);
    return true;
}

// An exported session carries its NAND packed (nand.frondnand.gz). Unpacks `from` into the
// folder's nand.frondnand when that's missing, checked against its CRCs; the descriptor check
// follows in checkSiblings. True when there was nothing to unpack.
inline bool unpackNand(const std::filesystem::path& from, const std::filesystem::path& folder,
                       const std::vector<frondMachine::Media>& media) {
    std::error_code ec;
    const auto dest = folder / frondMachine::nandSibling;
    const auto packedFile = frondStorage::packedPath(from / frondMachine::nandSibling);
    const bool described = std::any_of(media.begin(), media.end(), [](const frondMachine::Media& m) {
        return m.kind == frondMachine::kindNand;
    });
    if (!described || std::filesystem::exists(dest, ec) || std::filesystem::exists(from / frondMachine::nandSibling, ec) ||
        !std::filesystem::is_regular_file(packedFile, ec))
        return true;
    if (!frondStorage::unpack(packedFile, dest).ok) return false;
    // In its own folder the packed copy has done its job.
    if (std::filesystem::equivalent(from, folder, ec)) removePath(packedFile);
    return true;
}

// Settles any SD journal, then checks the siblings. `full` reads every card byte, for imports.
// With `swap` the card files may differ from the saved cards (resolveCardSwap).
inline bool loadV7Bytes(const uint8_t* data, size_t size, const std::filesystem::path& folder,
                        Contents& out, bool full = false, const CardSwap* swap = nullptr) {
    Contents parsed;
    if (!parseFolderSession(data, size, parsed) || !unpackNand(folder, folder, parsed.media)) return false;
    if (swap) {
        if (!resolveCardSwap(folder, parsed, *swap) ||
            frondMachine::checkSiblings(folder, parsed.media, full) != frondMachine::SiblingStatus::ok)
            return false;
        out = std::move(parsed);
        return true;
    }
    for (const auto& media : parsed.media)
        if (frondMachine::journalledKind(media.kind) && !settleSdJournal(folder, media)) return false;
    if (frondMachine::checkSiblings(folder, parsed.media, full) != frondMachine::SiblingStatus::ok)
        return false;
    out = std::move(parsed);
    return true;
}

inline bool extractV6ToFolder(const uint8_t* data, size_t size, const std::filesystem::path& folder,
                              uint64_t msImageBytes = msPhysicalBytes) {
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec) && !std::filesystem::create_directories(folder, ec))
        return false;
    Contents contents;
    std::string sdIdentity;
    if (!streamInflateV6(data, size, contents, folder / frondMachine::sdSibling, sdIdentity))
        return false;
    bool foundMs = false;
    if (!contents.savestate.empty() &&
        !extractMsFromSavestate(contents.savestate.data(), contents.savestate.size(),
                                folder / frondMachine::msSibling, msImageBytes, &foundMs))
        return false;
    if (foundMs) {
        frondMachine::Media media;
        media.kind = frondMachine::kindMs;
        media.name = frondMachine::msSibling;
        media.inserted = true;
        media.logicalSize = frondMachine::msLogicalBytes;
        media.physicalSize = frondMachine::msPhysicalBytes;
        contents.media.push_back(std::move(media));
    }
    std::filesystem::path dest;
    sessionPathIn(folder, dest);
    if (!saveV7(dest, contents)) return false;
    bool expectSd = false, expectMs = false;
    for (const auto& media : contents.media) {
        if (media.kind == frondMachine::kindSd) expectSd = true;
        if (media.kind == frondMachine::kindMs) expectMs = true;
    }
    std::error_code removeEc;
    if (!expectSd) std::filesystem::remove(folder / frondMachine::sdSibling, removeEc);
    if (!expectMs) std::filesystem::remove(folder / frondMachine::msSibling, removeEc);
    // A v6 stick is never a Pro: an old ms.frondmsp, its journals and its full copies would be refused as extra.
    std::vector<std::filesystem::path> staleMsPro;
    for (std::filesystem::directory_iterator it(folder, removeEc), end; !removeEc && it != end; it.increment(removeEc)) {
        const auto name = it->path().filename().string();
        if (name.compare(0, std::strlen(frondMachine::msProSibling), frondMachine::msProSibling) == 0)
            staleMsPro.push_back(it->path());
    }
    for (const auto& path : staleMsPro) std::filesystem::remove(path, removeEc);
    FileCardBacking::DiscardGenerations(folder / frondMachine::msProSibling);
    return true;
}

inline std::filesystem::path loneExtractFolder(const std::filesystem::path& file) {
    auto dest = file.parent_path() / file.stem();
    if (dest == file) dest += "-session";
    return dest;
}

// A new folder for a lone v6 file: <stem>/, or <stem> 2/ and so on when that exists. An existing
// folder is never reused, so opening never overwrites anything beside the file.
inline bool createLoneExtractFolder(const std::filesystem::path& file, std::filesystem::path& folder) {
    const auto base = loneExtractFolder(file);
    for (int n = 1; n < 1000; ++n) {
        auto candidate = base;
        if (n > 1) candidate += " " + std::to_string(n);
        std::error_code ec;
        if (std::filesystem::create_directory(candidate, ec)) {
            folder = candidate;
            return true;
        }
        if (ec) return false;
    }
    return false;
}

// A lone v6 file extracts into <stem>/ next to it. destFolder overrides that.
inline bool extractLoneV6(const std::filesystem::path& frondFile, std::vector<uint8_t>& bytes,
                          std::filesystem::path destFolder = {},
                          uint64_t msImageBytes = msPhysicalBytes) {
    if (destFolder.empty()) destFolder = loneExtractFolder(frondFile);
    std::error_code ec;
    if (std::filesystem::exists(destFolder, ec) && !std::filesystem::is_directory(destFolder, ec))
        return false;
    if (!std::filesystem::exists(destFolder, ec) && !std::filesystem::create_directories(destFolder, ec))
        return false;
    return extractV6ToFolder(bytes.data(), bytes.size(), destFolder, msImageBytes);
}

inline bool isMachineFolder(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) return false;
    std::filesystem::path session;
    if (frondMachine::sessionFile(path, session)) return true;
    return std::filesystem::is_regular_file(path / frondMachine::sdSibling, ec) ||
           std::filesystem::is_regular_file(path / frondMachine::msSibling, ec) ||
           std::filesystem::is_regular_file(path / frondMachine::msProSibling, ec) ||
           std::filesystem::is_regular_file(path / frondMachine::nandSibling, ec);
}

inline bool frondInFolder(const std::filesystem::path& file) {
    const auto parent = file.parent_path();
    const auto name = file.filename().string();
    if (name == frondMachine::androidSession) return true;
    if (name == parent.filename().string() + ".frond") return true;
    std::error_code ec;
    return std::filesystem::is_regular_file(parent / frondMachine::sdSibling, ec) ||
           std::filesystem::is_regular_file(parent / frondMachine::msSibling, ec) ||
           std::filesystem::is_regular_file(parent / frondMachine::msProSibling, ec) ||
           std::filesystem::is_regular_file(parent / frondMachine::nandSibling, ec);
}

// Copies a sidecar into `dest` through a temp, so a partial copy never takes the real name.
inline bool copySidecar(const std::filesystem::path& from, const std::filesystem::path& dest, const char* prefix) {
    std::filesystem::path temp;
    if (!createTemp(dest.parent_path(), prefix, temp)) return false;
    std::error_code ec;
    std::filesystem::copy_file(from, temp, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec || rename(temp.c_str(), dest.c_str()) < 0) {
        removePath(temp);
        return false;
    }
    return true;
}

// Loads a session that lives somewhere else into the empty machine folder `work`: a v6 file is
// extracted there and a v7 session's cards are copied there. Nothing is written beside the source.
inline bool loadIntoWorkFolder(const std::filesystem::path& path, const std::filesystem::path& work, Contents& out,
                               std::filesystem::path& folder, uint64_t msImageBytes) {
    std::error_code ec;
    if (!std::filesystem::is_directory(work, ec)) return false;
    std::filesystem::path session = path, source = path.parent_path();
    if (isMachineFolder(path)) {
        source = path;
        if (!frondMachine::sessionFile(path, session)) return false;
    } else if (!std::filesystem::is_regular_file(path, ec)) {
        return false;
    }
    if (source.empty()) source = ".";
    std::vector<uint8_t> bytes;
    if (!frondMachine::readBytes(session, bytes)) return false;
    uint32_t version = 0;
    std::string device;
    if (!peekHeader(bytes.data(), bytes.size(), version, device)) return false;
    if (version == SessionFile5::kVersionV6) {
        std::filesystem::path extracted;
        if (!extractV6ToFolder(bytes.data(), bytes.size(), work, msImageBytes) ||
            !frondMachine::sessionFile(work, extracted) || !frondMachine::readBytes(extracted, bytes))
            return false;
        folder = work;
        return loadV7Bytes(bytes.data(), bytes.size(), work, out);
    }
    if (!isFolderVersion(version)) return false;
    Contents parsed;
    if (!parseFolderSession(bytes.data(), bytes.size(), parsed)) return false;
    for (const auto& media : parsed.media) {
        // NAND comes with its journals and full copies like an SD card.
        const bool sd = frondMachine::fileCardKind(media.kind) || media.kind == frondMachine::kindNand;
        const char* prefix = media.kind == frondMachine::kindNand ? ".frondnand.tmp-"
                             : sd                                ? ".frondsd.tmp-"
                                                                 : ".frondms.tmp-";
        // A journal or full copy the source never finished comes along and is settled in the copy.
        std::error_code jec;
        const auto journal = sdJournalPath(source, media.contentGeneration, media.kind);
        const auto full = sdFullPath(source, media.contentGeneration, media.kind);
        const bool hasFull = sd && media.contentGeneration > 0 && std::filesystem::is_regular_file(full, jec);
        if (media.kind == frondMachine::kindNand && !std::filesystem::exists(source / media.name, jec) &&
            std::filesystem::is_regular_file(frondStorage::packedPath(source / media.name), jec)) {
            if (!unpackNand(source, work, parsed.media)) return false;
            continue;
        }
        if (!copySidecar(source / media.name, work / media.name, prefix) &&
            (!hasFull || std::filesystem::exists(source / media.name, jec)))
            return false;
        if (sd && std::filesystem::is_regular_file(journal, jec) &&
            !copySidecar(journal, sdJournalPath(work, media.contentGeneration, media.kind), ".frondsd.tmp-"))
            return false;
        if (hasFull && !copySidecar(full, sdFullPath(work, media.contentGeneration, media.kind), ".frondsd.tmp-"))
            return false;
    }
    if (!loadV7Bytes(bytes.data(), bytes.size(), work, parsed, true)) return false;
    folder = work;
    out = std::move(parsed);
    return true;
}

// `work`, when given and not the session's own folder, is where an outside session runs from.
// `swap` is --card-swap, for a session loaded in its own folder; a copy into `work` checks as saved.
inline bool loadPath(const std::filesystem::path& path, Contents& out, std::filesystem::path& folder,
                     uint64_t msImageBytes = msPhysicalBytes, const std::filesystem::path& work = {},
                     const CardSwap* swap = nullptr) {
    std::error_code ec;
    if (!work.empty()) {
        const auto own = isMachineFolder(path) ? path : path.parent_path();
        if (!std::filesystem::equivalent(own.empty() ? std::filesystem::path(".") : own, work, ec))
            return loadIntoWorkFolder(path, work, out, folder, msImageBytes);
    }
    if (isMachineFolder(path)) {
        folder = path;
        std::filesystem::path session;
        if (!frondMachine::sessionFile(path, session)) return false;
        std::vector<uint8_t> bytes;
        if (!frondMachine::readBytes(session, bytes)) return false;
        uint32_t version = 0;
        std::string device;
        if (!peekHeader(bytes.data(), bytes.size(), version, device)) return false;
        if (version == SessionFile5::kVersionV6) {
            if (!extractV6ToFolder(bytes.data(), bytes.size(), path, msImageBytes)) return false;
            if (!frondMachine::readBytes(session, bytes) &&
                !(frondMachine::sessionFile(path, session) && frondMachine::readBytes(session, bytes)))
                return false;
        }
        return loadV7Bytes(bytes.data(), bytes.size(), path, out, false, swap);
    }
    if (std::filesystem::is_regular_file(path, ec)) {
        std::vector<uint8_t> bytes;
        if (!frondMachine::readBytes(path, bytes)) return false;
        uint32_t version = 0;
        std::string device;
        if (!peekHeader(bytes.data(), bytes.size(), version, device)) return false;
        if (frondInFolder(path)) {
            folder = path.parent_path();
            if (version == SessionFile5::kVersionV6) {
                if (!extractV6ToFolder(bytes.data(), bytes.size(), folder, msImageBytes)) return false;
                std::filesystem::path session;
                if (!frondMachine::sessionFile(folder, session) ||
                    !frondMachine::readBytes(session, bytes))
                    return false;
            } else if (!isFolderVersion(version)) {
                return false;
            }
            return loadV7Bytes(bytes.data(), bytes.size(), folder, out, false, swap);
        }
        if (version == SessionFile5::kVersionV6) {
            if (!createLoneExtractFolder(path, folder)) return false;
            std::filesystem::path session;
            if (!extractV6ToFolder(bytes.data(), bytes.size(), folder, msImageBytes) ||
                !frondMachine::sessionFile(folder, session) || !frondMachine::readBytes(session, bytes)) {
                std::filesystem::remove_all(folder, ec);
                return false;
            }
            return loadV7Bytes(bytes.data(), bytes.size(), folder, out, false, swap);
        }
        if (isFolderVersion(version)) {
            folder = path.parent_path();
            return loadV7Bytes(bytes.data(), bytes.size(), folder, out, false, swap);
        }
    }
    return false;
}
}  // namespace frondSession

#endif
