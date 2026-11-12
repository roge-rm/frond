#ifndef FROND_STORAGE_PACKED_H
#define FROND_STORAGE_PACKED_H

#include "storage/Container.h"
#include "storage/Io.h"
#include "storage/MsLayout.h"
#include "storage/Settle.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// A container file packed for sharing, as an exported session carries its nand.frondnand:
// nand.frondnand.gz, a gzip file (deflate, as the zip exports and the old .frond use) of the
// whole file. Inside a machine folder the file stays unpacked, so saves can journal into it.
// pack() and unpack() write a temp beside the output and rename it, so a cut-off run leaves no
// output; unpack() checks the result's header and every CRC before it takes the name.
namespace frondStorage {

constexpr const char* packedSuffix = ".gz";

inline std::filesystem::path packedPath(const std::filesystem::path& file) {
    return file.string() + packedSuffix;
}

namespace packed {

constexpr size_t bufferBytes = size_t(1) << 20;

// The temp a pack or unpack writes beside `dest`: the core's save temps for NAND, so a crash's
// leftover is cleaned as one.
inline const char* tempPrefix(const std::filesystem::path& dest) {
    const auto name = dest.filename().string();
    if (name.find(".frondnand") != std::string::npos) return ".frondnand.tmp-";
    if (name.find(".frondms") != std::string::npos) return ".frondms.tmp-";
    return ".frondsd.tmp-";
}

inline void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

// Whether an unpacked header is one Frond makes: a NAND of at most maxNandRawBytes of pages, a
// classic stick of a known size, or an SD or Pro card (the header's own limits).
inline bool knownSize(const frondSdFile::Info& info) {
    if (info.nand) return info.payloadBytes + info.sideBytes <= frondSdFile::maxNandRawBytes;
    if (info.classic) return frondMsLayout::of(info) != nullptr;
    return !info.sideBytes;
}

}  // namespace packed

// Packs `file` into the gzip file `dest`. `progress` gets the input bytes read.
inline Result pack(const std::filesystem::path& file, const std::filesystem::path& dest, int level = MZ_DEFAULT_LEVEL,
                   const Progress& progress = {}) {
    const int in = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return failed("read", "Could not read the file.");
    Fd inCloser(in);
    const int64_t total = sizeOf(in);
    if (total < 0) return failed("read", "Could not read the file.");
    std::filesystem::path temp;
    if (!createTemp(folderOf(dest), packed::tempPrefix(dest), temp)) return failed("write", "Could not write the file.");
    const int out = ::open(temp.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    Fd outCloser(out);
    auto fail = [&](const char* code, const char* message) {
        removePath(temp);
        return failed(code, message);
    };
    if (out < 0) return fail("write", "Could not write the file.");
    // gzip header: deflate, no name, no time, unknown system.
    const uint8_t header[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255};
    uint64_t at = 0;
    if (!writeAllAt(out, header, sizeof(header), at)) return fail("write", "Could not write the file.");
    at += sizeof(header);
    mz_stream z{};
    if (mz_deflateInit2(&z, level, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY) != MZ_OK)
        return fail("write", "Could not write the file.");
    struct Ender {
        mz_stream* z;
        ~Ender() { mz_deflateEnd(z); }
    } ender{&z};
    std::vector<uint8_t> input(packed::bufferBytes), output(packed::bufferBytes);
    uint32_t crc = 0;
    uint64_t done = 0;
    for (bool last = false; !last;) {
        const size_t n = size_t(std::min<uint64_t>(input.size(), uint64_t(total) - done));
        if (n && !readAllAt(in, input.data(), n, done)) return fail("read", "Could not read the file.");
        crc = uint32_t(mz_crc32(crc, input.data(), n));
        done += n;
        last = done == uint64_t(total);
        z.next_in = input.data();
        z.avail_in = unsigned(n);
        int status;
        do {
            z.next_out = output.data();
            z.avail_out = unsigned(output.size());
            status = mz_deflate(&z, last ? MZ_FINISH : MZ_NO_FLUSH);
            if (status != MZ_OK && status != MZ_STREAM_END && status != MZ_BUF_ERROR)
                return fail("write", "Could not write the file.");
            const size_t made = output.size() - z.avail_out;
            if (made && !writeAllAt(out, output.data(), made, at)) return fail("write", "Could not write the file.");
            at += made;
        } while (z.avail_in || (last && status != MZ_STREAM_END));
        if (progress && !progress(done, uint64_t(total))) return fail("cancelled", "Cancelled.");
    }
    uint8_t trailer[8];
    packed::put32(trailer, crc);
    packed::put32(trailer + 4, uint32_t(total));
    if (!writeAllAt(out, trailer, sizeof(trailer), at) || fsync(out) != 0 || rename(temp.c_str(), dest.c_str()) < 0)
        return fail("write", "Could not write the file.");
    syncDir(folderOf(dest));
    return {true, "", {}};
}

// Unpacks the gzip file `packed` into the container `dest`, checked against its header and CRCs.
// `info` gets its header. `progress` gets the packed bytes read.
inline Result unpack(const std::filesystem::path& packedFile, const std::filesystem::path& dest,
                     frondSdFile::Info* info = nullptr, const Progress& progress = {}) {
    const int in = ::open(packedFile.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return failed("read", "Could not read the file.");
    Fd inCloser(in);
    const int64_t total = sizeOf(in);
    uint8_t header[10];
    if (total < 18 || !readAllAt(in, header, sizeof(header), 0) || header[0] != 0x1f || header[1] != 0x8b ||
        header[2] != 8 || (header[3] & 0xe0))
        return failed("image", "The file is damaged.");
    // Optional fields: extra, name, comment, header CRC.
    uint64_t at = sizeof(header);
    const auto skipText = [&] {
        for (uint8_t c = 1; c;)
            if (at >= uint64_t(total) || !readAllAt(in, &c, 1, at++)) return false;
        return true;
    };
    if (header[3] & 4) {
        uint8_t n[2];
        if (!readAllAt(in, n, 2, at)) return failed("image", "The file is damaged.");
        at += 2 + (uint64_t(n[0]) | uint64_t(n[1]) << 8);
    }
    if (((header[3] & 8) && !skipText()) || ((header[3] & 16) && !skipText())) return failed("image", "The file is damaged.");
    if (header[3] & 2) at += 2;
    if (at + 8 > uint64_t(total)) return failed("image", "The file is damaged.");
    std::filesystem::path temp;
    if (!createTemp(folderOf(dest), packed::tempPrefix(dest), temp)) return failed("write", "Could not write the file.");
    const int out = ::open(temp.c_str(), O_RDWR | O_TRUNC | O_CLOEXEC);
    Fd outCloser(out);
    auto fail = [&](const char* code, const char* message) {
        removePath(temp);
        return failed(code, message);
    };
    if (out < 0) return fail("write", "Could not write the file.");
    mz_stream z{};
    if (mz_inflateInit2(&z, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) return fail("write", "Could not write the file.");
    struct Ender {
        mz_stream* z;
        ~Ender() { mz_inflateEnd(z); }
    } ender{&z};
    std::vector<uint8_t> input(packed::bufferBytes), output(packed::bufferBytes);
    const uint64_t end = uint64_t(total) - 8;
    uint32_t crc = 0;
    uint64_t written = 0;
    // Until the header is out, nothing more than it; then the file its header describes, so a
    // damaged or hostile file can't fill the disk.
    uint64_t limit = frondSdFile::headerBytes;
    bool sized = false;
    int status = MZ_OK;
    while (status != MZ_STREAM_END) {
        if (!z.avail_in) {
            const size_t n = size_t(std::min<uint64_t>(input.size(), end - at));
            if (!n || !readAllAt(in, input.data(), n, at)) return fail("image", "The file is damaged.");
            at += n;
            z.next_in = input.data();
            z.avail_in = unsigned(n);
        }
        z.next_out = output.data();
        z.avail_out = unsigned(output.size());
        status = mz_inflate(&z, MZ_NO_FLUSH);
        if (status != MZ_OK && status != MZ_STREAM_END) return fail("image", "The file is damaged.");
        const size_t made = output.size() - z.avail_out;
        crc = uint32_t(mz_crc32(crc, output.data(), made));
        if (!sized && written + made >= frondSdFile::headerBytes) {
            uint8_t head[frondSdFile::headerBytes];
            const size_t had = size_t(written);
            if (had) {
                if (!readAllAt(out, head, had, 0)) return fail("read", "Could not read the file.");
            }
            std::memcpy(head + had, output.data(), frondSdFile::headerBytes - had);
            frondSdFile::Info found;
            if (!frondSdFile::parseContainer(head, found) || !packed::knownSize(found))
                return fail("image", "The file is damaged.");
            limit = found.fileBytes();
            sized = true;
        }
        if (written + made > limit) return fail("image", "The file is damaged.");
        if (made && !writeAllAt(out, output.data(), made, written)) return fail("write", "Could not write the file.");
        written += made;
        if (progress && !progress(at, uint64_t(total))) return fail("cancelled", "Cancelled.");
    }
    uint8_t trailer[8];
    if (at - z.avail_in != end || !readAllAt(in, trailer, sizeof(trailer), end) ||
        frondSdFile::get32(trailer) != crc || frondSdFile::get32(trailer + 4) != uint32_t(written))
        return fail("image", "The file is damaged.");
    frondSdFile::Info checked;
    if (fsync(out) != 0) return fail("write", "Could not write the file.");
    if (auto verified = verify(out, checked); !verified.ok) return fail(verified.code == "read" ? "read" : "image", "The file is damaged.");
    if (rename(temp.c_str(), dest.c_str()) < 0) return fail("write", "Could not write the file.");
    syncDir(folderOf(dest));
    if (info) *info = checked;
    return {true, "", {}};
}

}  // namespace frondStorage

#endif
