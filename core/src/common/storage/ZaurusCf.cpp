#include "storage/ZaurusCf.h"

#include "storage/Errors.h"
#include "storage/Ext3Writer.h"
#include "storage/Fat32Writer.h"
#include "storage/FatNames.h"
#include "storage/Io.h"
#include "storage/TarStream.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>

using frondStorage::Cancelled;
using frondStorage::Error;

namespace frondZaurusCf {

namespace {

constexpr size_t maxEntries = 32'768;
constexpr size_t maxPathBytes = 4096;
constexpr size_t chunk = 256 * 1024;
constexpr size_t fontRomBytes = 8 * 1024 * 1024;
constexpr const char* hdd1Files = ".sys/hdlist1.dat";
constexpr const char* factorySeed = ".sys/hdimage2.tgz";
constexpr int fontHeights[] = {10, 12, 16, 18, 20, 24, 36};

std::string fontPath(int height) {
    return "usr/QtPalmtop.rom/lib/fonts/lcfont_" + std::to_string(height * 10) + "_50_t10.qpf";
}

std::string quoted(const std::string& name) { return "\"" + name + "\""; }

// Python's PurePosixPath parts, refusing what archive_path refuses.
std::vector<std::string> archivePath(const std::string& name) {
    std::vector<std::string> parts;
    bool bad = name.find('\0') != std::string::npos || name.size() > maxPathBytes || (!name.empty() && name[0] == '/');
    size_t at = 0;
    while (!bad && at <= name.size()) {
        const auto slash = std::min(name.find('/', at), name.size());
        const auto part = name.substr(at, slash - at);
        at = slash + 1;
        if (part.empty() || part == ".") continue;
        if (part == ".." || part.size() > 255) bad = true;
        parts.push_back(part);
    }
    if (bad || parts.empty()) throw Error("The archive has an unexpected path " + quoted(name) + ".");
    return parts;
}

std::string join(const std::vector<std::string>& parts, size_t from) {
    std::string out;
    for (size_t i = from; i < parts.size(); ++i) out += (out.empty() ? "" : "/") + parts[i];
    return out;
}

std::string parentOf(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

struct Entry {
    frondTar::Member member;
    std::string path;  // inside the partition, "" for its root
    uint32_t content{};
    bool hasContent{};
};

// One partition's entries as partition_members collects them.
struct Partition {
    std::string name;
    uint64_t capacity{};
    uint64_t payload{};
    std::vector<Entry> entries;
    std::map<std::string, size_t> byPath;

    const Entry* find(const std::string& path) const {
        const auto it = byPath.find(path);
        return it == byPath.end() ? nullptr : &entries[it->second];
    }
};

// Checks a member and files it under its partition; nullptr when the archive has none of `parts`.
Entry& addMember(std::vector<Partition*>& parts, const frondTar::Member& m) {
    const auto path = archivePath(m.name);
    Partition* part = nullptr;
    for (auto* p : parts)
        if (p->name == path[0]) part = p;
    if (!part) throw Error("The archive has an unexpected partition in " + quoted(m.name) + ".");
    const auto rel = join(path, 1);
    if (part->byPath.count(rel)) throw Error("The archive has " + quoted(m.name) + " twice.");
    if (part->entries.size() >= maxEntries) throw Error("The archive's " + part->name + " has too many entries.");
    if (m.sparse) throw Error("The archive has a sparse entry " + quoted(m.name) + ", which isn't supported.");
    if (!(m.isDir() || m.isReg() || m.isSym() || m.isLink()))
        throw Error("The archive has an unsupported entry " + quoted(m.name) + ".");
    if (m.size < 0 || (!m.isReg() && m.size)) throw Error("The archive has an invalid size for " + quoted(m.name) + ".");
    if (part->name == "hdd3" && m.size > 0xffffffffll)
        throw Error(quoted(m.name) + " is larger than FAT32 allows.");
    if (m.linkname.size() > maxPathBytes || m.linkname.find('\0') != std::string::npos)
        throw Error("The archive has an invalid link for " + quoted(m.name) + ".");
    part->payload += uint64_t(m.size);
    if (part->payload > part->capacity)
        throw Error("The archive's " + part->name + " is larger than its partition.");
    if (rel.empty() && !m.isDir()) throw Error("The archive's partition root must be a folder: " + quoted(m.name) + ".");
    part->byPath[rel] = part->entries.size();
    part->entries.push_back({m, rel, 0, false});
    return part->entries.back();
}

std::string linkTarget(const Partition& part, const Entry& e) {
    std::string name = e.member.linkname;
    name.erase(0, name.find_first_not_of('/') == std::string::npos ? name.size() : name.find_first_not_of('/'));
    const auto target = archivePath(name);
    if (target[0] != part.name) throw Error("The archive's hard link " + quoted(e.member.name) + " crosses a partition.");
    return join(target, 1);
}

// The checks partition_members makes once the whole archive is read.
void checkPartition(const Partition& part) {
    if (part.entries.empty()) throw Error("The archive doesn't contain " + part.name + ".");
    for (const auto& e : part.entries) {
        for (auto up = e.path; !up.empty();) {
            up = parentOf(up);
            const auto* parent = part.find(up);
            if (parent && !parent->member.isDir())
                throw Error("The archive has " + quoted(e.member.name) + " inside something that isn't a folder.");
        }
        if (e.member.isLink()) {
            const auto* linked = part.find(linkTarget(part, e));
            if (!linked || !linked->member.isReg())
                throw Error("The archive's hard link " + quoted(e.member.name) + " needs a regular file.");
        }
    }
}

// validate_fat_entries: FAT32 takes folders and files with names it can hold, no two the same
// but for case.
void checkFat(const Partition& part) {
    std::map<std::u16string, std::string> seen;
    for (const auto& e : part.entries) {
        if (!(e.member.isDir() || e.member.isReg()))
            throw Error("FAT32 doesn't support the archive link " + quoted(e.member.name) + ".");
        if (e.path.empty()) continue;
        size_t at = 0;
        while (at <= e.path.size()) {
            const auto slash = std::min(e.path.find('/', at), e.path.size());
            const auto name = e.path.substr(at, slash - at);
            at = slash + 1;
            bool bad = name.back() == '.' || name.back() == ' ';
            for (unsigned char c : name)
                if (c < 32 || strchr("\"*/:<>?\\|", c)) bad = true;
            if (frondStorage::fat::utf16(name).size() > 255) bad = true;
            if (bad) throw Error("The archive has an invalid FAT32 name " + quoted(e.member.name) + ".");
        }
        for (auto up = e.path; !up.empty(); up = parentOf(up)) {
            const auto key = frondStorage::fat::folded(frondStorage::fat::utf16(up));
            const auto [it, added] = seen.emplace(key, up);
            if (!added && it->second != up)
                throw Error("The archive has names in hdd3 that differ only by case: " + quoted(e.member.name) + ".");
        }
    }
}

uint32_t le32(const std::vector<uint8_t>& rom, size_t at) {
    return uint32_t(rom[at]) | uint32_t(rom[at + 1]) << 8 | uint32_t(rom[at + 2]) << 16 | uint32_t(rom[at + 3]) << 24;
}

// lcfont_files: the seven LC Fonts from the CS0 dump's resource directory.
std::map<std::string, std::vector<uint8_t>> lcFonts(int fd) {
    std::vector<uint8_t> rom(fontRomBytes + 1);
    frondTar::FdReader reader(fd);
    rom.resize(reader.read(rom.data(), rom.size()));
    const size_t directory = 0x80000;
    if (rom.size() != fontRomBytes || memcmp(rom.data() + directory, "ROM_DATA_BLOCK\0\0", 16) != 0)
        throw Error("Expected the 8 MiB SL-C3000 CS0 dump with its ROM resource directory.");

    // The directory and the font table use offsets from the end of their offset lists.
    auto records = [&](size_t start, size_t end, size_t count) {
        if (start + 20 + count * 4 > end || le32(rom, start + 16) != count)
            throw Error("Unexpected SL-C3000 ROM resource count.");
        const size_t table = start + 20, data = table + count * 4;
        std::vector<std::pair<size_t, size_t>> out;
        std::vector<uint64_t> starts;
        for (size_t i = 0; i < count; ++i) starts.push_back(uint64_t(data) + le32(rom, table + 4 * i));
        if (le32(rom, table) != 0) throw Error("Invalid offsets in the SL-C3000 ROM resources.");
        for (size_t i = 0; i < count; ++i) {
            const uint64_t next = i + 1 < count ? starts[i + 1] : end;
            if (!(starts[i] < next)) throw Error("Invalid offsets in the SL-C3000 ROM resources.");
            out.emplace_back(size_t(starts[i]), size_t(next));
        }
        return out;
    };
    size_t start = 0, end = 0;
    bool found = false;
    for (const auto& [s, e] : records(directory, fontRomBytes, 4)) {
        if (s + 20 <= rom.size() && memcmp(rom.data() + s + 4, "C700_QPF_DATA\0\0\0", 16) == 0) {
            const uint64_t length = le32(rom, s);
            if (!(length >= 20 && length + 4 <= e - s)) throw Error("Invalid LC Font resource block size.");
            start = s + 4;
            end = start + size_t(length);
            found = true;
            break;
        }
    }
    if (!found) throw Error("The CS0 dump doesn't contain the stock LC Font resources.");
    std::map<std::string, std::vector<uint8_t>> fonts;
    const auto fontRecords = records(start, end, std::size(fontHeights));
    for (size_t i = 0; i < fontRecords.size(); ++i) {
        const auto [s, e] = fontRecords[i];
        if (e - s < 14) throw Error("Truncated LC Font resource.");
        const uint64_t length = le32(rom, s);
        if (!(length >= 10 && length + 4 <= e - s) || le32(rom, s + 4) != uint32_t(fontHeights[i]))
            throw Error("Invalid LC Font resource size or metrics.");
        fonts[fontPath(fontHeights[i])].assign(rom.begin() + ptrdiff_t(s + 4), rom.begin() + ptrdiff_t(s + 4 + length));
    }
    return fonts;
}

void writeMbr(const WriteFn& write, uint64_t total) {
    auto chs = [](uint64_t lba, uint8_t* out) {
        uint64_t cylinder = lba / (16 * 63);
        uint64_t head = (lba % (16 * 63)) / 63, sector = lba % 63;
        if (cylinder > 1023) cylinder = 1023, head = 15, sector = 62;
        out[0] = uint8_t(head);
        out[1] = uint8_t((sector + 1) | ((cylinder >> 2) & 0xc0));
        out[2] = uint8_t(cylinder);
    };
    uint8_t mbr[sectorBytes]{};
    const uint64_t parts[3][3] = {{hdd1Start, hdd2Start, 0x83}, {hdd2Start, hdd3Start, 0x83}, {hdd3Start, total, 0x0c}};
    for (int i = 0; i < 3; ++i) {
        uint8_t* p = mbr + 446 + 16 * i;
        chs(parts[i][0], p + 1);
        p[4] = uint8_t(parts[i][2]);
        chs(parts[i][1] - 1, p + 5);
        frondStorage::fat::put32(p + 8, uint32_t(parts[i][0]));
        frondStorage::fat::put32(p + 12, uint32_t(parts[i][1] - parts[i][0]));
    }
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    write(0, mbr, sizeof mbr);
}

// Passes on only the 512-byte sectors that aren't all zeros.
WriteFn sparse(const WriteFn& write) {
    return [write](uint64_t offset, const uint8_t* data, size_t size) {
        auto zero = [&](size_t from, size_t to) {
            return std::all_of(data + from, data + to, [](uint8_t c) { return !c; });
        };
        auto edge = [&](size_t at) { return std::min(size, at + size_t(sectorBytes - (offset + at) % sectorBytes)); };
        size_t at = 0;
        while (at < size) {
            size_t to = edge(at);
            if (zero(at, to)) {
                at = to;
                continue;
            }
            size_t run = to;
            while (run < size) {
                const size_t next = edge(run);
                if (zero(run, next)) break;
                run = next;
            }
            write(offset + at, data + at, run - at);
            at = run;
        }
    };
}

uint32_t stamp32(int64_t v) { return uint32_t(uint64_t(v)); }

std::vector<frondExt3::Node> ext3Nodes(const Partition& part) {
    std::vector<frondExt3::Node> nodes;
    for (const auto& e : part.entries) {
        frondExt3::Node n;
        n.path = e.path;
        n.type = e.member.type;
        n.mode = e.member.mode & 07777;
        n.uid = stamp32(e.member.uid);
        n.gid = stamp32(e.member.gid);
        n.mtime = stamp32(e.member.mtime);
        n.content = e.content;
        if (e.member.isSym()) n.target = e.member.linkname;
        if (e.member.isLink()) n.target = linkTarget(part, e);
        nodes.push_back(std::move(n));
    }
    return nodes;
}

}  // namespace

uint64_t imageSectors(int sizeGb) {
    if (sizeGb < 4 || sizeGb > 32) return 0;
    return sizeGb == 4 ? stockSectors : uint64_t(sizeGb) * 1'000'000'000ull / sectorBytes;
}

void build(const Request& request, const WriteFn& rawWrite) {
    const uint64_t sectors = imageSectors(request.sizeGb);
    if (!sectors) throw Error("Zaurus CF size must be a whole number from 4 to 32 GB.");
    if (request.hdd1Fd < 0) throw Error("Choose a Sharp HDD1 archive.");
    auto poll = [&] {
        if (request.cancelled && request.cancelled()) throw Cancelled();
    };
    uint64_t readBytes = 0;
    const uint64_t total = request.hdd1Size > 0 ? uint64_t(request.hdd1Size) : 0;
    auto report = [&](size_t n) {
        readBytes += n;
        if (request.progress) request.progress(total ? std::min(readBytes, total) : readBytes, total);
        poll();
    };
    poll();
    const auto fonts = request.fontRomFd >= 0 ? lcFonts(request.fontRomFd) : std::map<std::string, std::vector<uint8_t>>{};

    const WriteFn write = sparse(rawWrite);
    const auto now = uint32_t(std::time(nullptr));
    writeMbr(write, sectors);
    frondExt3::Writer hdd1(write, hdd1Start * sectorBytes, (hdd2Start - hdd1Start) / 2, now, "hdd1", poll);
    frondExt3::Writer hdd2(write, hdd2Start * sectorBytes, (hdd3Start - hdd2Start) / 2, now, "hdd2", poll);
    frondFat32::Writer hdd3(write, hdd3Start * sectorBytes, uint32_t(sectors - hdd3Start), uint32_t(hdd3Start), now, poll);

    Partition p1, p2, p3;
    p1.name = "hdd1";
    p1.capacity = (hdd2Start - hdd1Start) * sectorBytes;
    p2.name = "hdd2";
    p2.capacity = (hdd3Start - hdd2Start) * sectorBytes;
    p3.name = "hdd3";
    p3.capacity = (sectors - hdd3Start) * sectorBytes;
    std::vector<Partition*> outerParts{&p1}, seedParts{&p2, &p3};
    auto limit = [](const std::vector<Partition*>& parts) {
        uint64_t sum = 0;
        for (auto* p : parts) sum += p->capacity;
        return sum + parts.size() * maxEntries * (maxPathBytes + 1024);
    };

    std::vector<uint8_t> buffer(chunk);
    uint64_t replaced = 0;
    bool seedDone = false;

    frondTar::FdReader file(request.hdd1Fd, report);
    frondTar::PeekReader peek(file);
    std::unique_ptr<frondTar::GzipReader> gunzip;
    if (peek.gzipped()) gunzip = std::make_unique<frondTar::GzipReader>(peek);
    frondTar::TarReader outer(gunzip ? static_cast<frondTar::Reader&>(*gunzip) : peek, limit(outerParts));

    // The factory seed: hdd2 and hdd3, read while its bytes go into hdd1.
    auto readSeed = [&](frondTar::Reader& source) {
        frondTar::PeekReader seedPeek(source);
        if (!seedPeek.gzipped()) throw Error("HDD1 .sys/hdimage2.tgz must be a gzip-compressed factory seed.");
        frondTar::GzipReader seedGunzip(seedPeek);
        frondTar::TarReader seed(seedGunzip, limit(seedParts));
        frondTar::Member m;
        while (seed.next(m)) {
            auto& e = addMember(seedParts, m);
            if (!m.isReg()) continue;
            const bool fat = archivePath(m.name)[0] == "hdd3";
            e.content = fat ? hdd3.beginFile(uint64_t(m.size)) : hdd2.beginFile(uint64_t(m.size));
            e.hasContent = true;
            while (const size_t n = seed.readData(buffer.data(), buffer.size())) {
                if (fat)
                    hdd3.fileData(buffer.data(), n);
                else
                    hdd2.fileData(buffer.data(), n);
            }
            if (fat)
                hdd3.endFile();
            else
                hdd2.endFile();
        }
        // The rest of the gzip stream, so its CRC is checked.
        while (seedGunzip.read(buffer.data(), buffer.size())) poll();
        while (seedPeek.read(buffer.data(), buffer.size())) {
        }
    };

    class Tee : public frondTar::Reader {
       public:
        Tee(frondTar::TarReader& from, frondExt3::Writer& to) : from_(from), to_(to) {}
        size_t read(uint8_t* data, size_t n) override {
            size_t done = 0;
            while (done < n) {
                const size_t got = from_.readData(data + done, n - done);
                if (!got) break;
                to_.fileData(data + done, got);
                done += got;
            }
            return done;
        }

       private:
        frondTar::TarReader& from_;
        frondExt3::Writer& to_;
    };

    frondTar::Member m;
    while (outer.next(m)) {
        auto& e = addMember(outerParts, m);
        if (!m.isReg()) continue;
        const auto font = fonts.find(e.path);
        if (font != fonts.end() && m.size == 0) {
            replaced += font->second.size();
            e.content = hdd1.beginFile(font->second.size());
            hdd1.fileData(font->second.data(), font->second.size());
        } else if (e.path == factorySeed) {
            e.content = hdd1.beginFile(uint64_t(m.size));
            Tee tee(outer, hdd1);
            readSeed(tee);
            seedDone = true;
        } else {
            e.content = hdd1.beginFile(uint64_t(m.size));
            while (const size_t n = outer.readData(buffer.data(), buffer.size())) hdd1.fileData(buffer.data(), n);
        }
        hdd1.endFile();
        e.hasContent = true;
    }
    // The rest of a gzip archive, so its CRC is checked.
    if (gunzip)
        while (gunzip->read(buffer.data(), buffer.size())) {
        }

    checkPartition(p1);
    const auto* list = p1.find(hdd1Files);
    const auto* seedEntry = p1.find(factorySeed);
    if (!list || !list->member.isReg() || !seedEntry || !seedEntry->member.isReg() || !seedDone)
        throw Error("A complete HDD1 archive needs its .sys/hdlist1.dat and .sys/hdimage2.tgz.");
    if (!list->member.size) throw Error("HDD1 .sys/hdlist1.dat is empty.");
    checkPartition(p2);
    checkPartition(p3);
    checkFat(p3);
    if (request.requireFonts)
        for (int height : fontHeights) {
            const auto* f = p1.find(fontPath(height));
            if (!f || !f->member.isReg() || (!f->member.size && !fonts.count(fontPath(height))))
                throw Error("Direct boot needs the stock LC Fonts. Choose a CS0 dump or a complete HDD1 with LC Fonts.");
        }
    if (p1.payload + replaced > p1.capacity)
        throw Error("The HDD1 archive and LC Fonts are larger than the first partition.");

    hdd2.finish(ext3Nodes(p2));
    std::vector<frondFat32::Node> fatNodes;
    for (const auto& e : p3.entries) fatNodes.push_back({e.path, e.member.isDir(), e.member.mtime, e.content});
    hdd3.finish(fatNodes);
    hdd1.finish(ext3Nodes(p1));
    if (request.progress) request.progress(total ? total : readBytes, total);
}

void buildToFd(const Request& request, int outFd) {
    if (frondStorage::sizeOf(outFd) != 0) throw Error("The CF image must be written to a new, empty file.");
    const uint64_t bytes = imageSectors(request.sizeGb) * sectorBytes;
    if (!bytes) throw Error("Zaurus CF size must be a whole number from 4 to 32 GB.");
    if (ftruncate(outFd, off_t(bytes)) != 0) throw Error("Could not make the CF image. Check the storage space.");
    build(request, [outFd](uint64_t offset, const uint8_t* data, size_t size) {
        if (!frondStorage::writeAllAt(outFd, data, size, offset))
            throw Error("Could not write the CF image. Check the storage space.");
    });
    if (frondStorage::sizeOf(outFd) != int64_t(bytes)) throw Error("The CF image came out the wrong size.");
    if (fsync(outFd) != 0) throw Error("Could not write the CF image. Check the storage space.");
}

int cliMain(const std::vector<std::string>& args) {
    std::string hdd1, fontRom, output;
    int sizeGb = 4;
    bool requireFonts = false, progress = false;
    auto usage = [] {
        std::cerr << "Usage: frond --zaurus-cf --hdd1 FILE [--size-gb 4-32] [--font-rom FILE] [--require-fonts] "
                     "[--progress] --output FILE"
                  << std::endl;
        return 2;
    };
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        const bool hasValue = i + 1 < args.size();
        if (a == "--hdd1" && hasValue) {
            hdd1 = args[++i];
        } else if (a == "--font-rom" && hasValue) {
            fontRom = args[++i];
        } else if (a == "--output" && hasValue) {
            output = args[++i];
        } else if (a == "--size-gb" && hasValue) {
            const auto& v = args[++i];
            if (v.empty() || v.size() > 2 || v.find_first_not_of("0123456789") != std::string::npos) return usage();
            sizeGb = std::stoi(v);
        } else if (a == "--require-fonts") {
            requireFonts = true;
        } else if (a == "--progress") {
            progress = true;
        } else {
            return usage();
        }
    }
    if (hdd1.empty() || output.empty()) return usage();

    std::filesystem::path temp;
    try {
        if (!imageSectors(sizeGb)) throw Error("Zaurus CF size must be a whole number from 4 to 32 GB.");
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::symlink_status(output, ec)))
            throw Error("The output already exists: " + output);
        const frondStorage::Fd in(::open(hdd1.c_str(), O_RDONLY | O_CLOEXEC));
        if (in.fd < 0) throw Error("Could not open the HDD1 archive.");
        const frondStorage::Fd font(fontRom.empty() ? -1 : ::open(fontRom.c_str(), O_RDONLY | O_CLOEXEC));
        if (!fontRom.empty() && font.fd < 0) throw Error("Could not open the CS0 dump.");
        if (!frondStorage::createTemp(frondStorage::folderOf(output), ".frond-zaurus-cf-", temp))
            throw Error("Could not create the CF image next to the output.");
        const frondStorage::Fd out(::open(temp.c_str(), O_RDWR | O_CLOEXEC));
        if (out.fd < 0) throw Error("Could not create the CF image next to the output.");
        Request request;
        request.hdd1Fd = in.fd;
        request.hdd1Size = frondStorage::sizeOf(in.fd);
        request.fontRomFd = font.fd;
        request.requireFonts = requireFonts;
        request.sizeGb = sizeGb;
        int last = -1;
        if (progress)
            request.progress = [&](uint64_t done, uint64_t total) {
                const int percent = total ? int(done * 100 / total) : 0;
                if (percent == last) return;
                last = percent;
                std::cout << "progress " << percent << std::endl;
            };
        buildToFd(request, out.fd);
        // Publish the finished image without replacing anything.
#ifdef _WIN32
        if (std::filesystem::exists(output, ec)) throw Error("The output already exists: " + output);
        std::filesystem::rename(temp, output, ec);
        if (ec) throw Error("Could not create the CF image: " + ec.message());
#else
        if (::link(temp.c_str(), output.c_str()) != 0)
            throw Error(errno == EEXIST ? "The output already exists: " + output : "Could not create the CF image.");
        frondStorage::removePath(temp);
        frondStorage::syncDir(frondStorage::folderOf(output));
#endif
    } catch (const Error& error) {
        frondStorage::removePath(temp);
        std::cerr << "Could not create Zaurus CF image: " << error.message << std::endl;
        return 1;
    } catch (const std::exception& error) {
        frondStorage::removePath(temp);
        std::cerr << "Could not create Zaurus CF image: " << error.what() << std::endl;
        return 1;
    }
    return 0;
}

}  // namespace frondZaurusCf
