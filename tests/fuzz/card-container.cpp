// Frond's card files: the SD, Memory Stick and NAND container headers and chunk CRC table, change
// journals, the old commit files, pending lists and a classic stick's block map, then a whole card
// file opened as the card manager does (journal beside it settled first) and its FAT walked.
// The input can have the CRCs put right so the parsers get past them.
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <fstream>

#include "FatWalk.h"
#include "Scratch.h"
#include "storage/CardFile.h"
#include "storage/ClassicStick.h"
#include "storage/Legacy.h"

namespace {
using Bytes = std::vector<uint8_t>;
namespace sd = frondSdFile;

void FixContainer(Bytes& file) {
    if (file.size() < sd::headerBytes) return;
    const uint64_t payload = std::min<uint64_t>(sd::get64(file.data() + 16), file.size() - sd::headerBytes);
    if (sd::get32(file.data() + 8) == sd::versionV1) {
        sd::put32(file.data() + 24, sd::crc32(file.data() + sd::headerBytes, size_t(payload)));
        return;
    }
    sd::ChunkCrcs crcs;
    crcs.add(file.data() + sd::headerBytes, size_t(payload));
    const auto& table = crcs.table();
    Bytes tail;
    sd::tableBytes(table, tail);
    file.resize(sd::headerBytes + size_t(payload));
    file.insert(file.end(), tail.begin(), tail.end());
    sd::put32(file.data() + 24, sd::headerCrc(file.data(), table));
}

void FixJournal(Bytes& journal) {
    if (journal.size() < frondStorage::journalHeaderBytes + frondStorage::journalTrailerBytes) return;
    const size_t body = journal.size() - frondStorage::journalTrailerBytes;
    sd::put32(journal.data() + body + 8, mz_crc32(0, journal.data(), body));
}

void FixCommit(Bytes& commit) {
    if (commit.size() < 32) return;
    const size_t end = commit.size() - 12;
    const uint32_t crc = mz_crc32(0, commit.data(), end + 8);
    for (int i = 0; i < 4; ++i) commit[end + 8 + i] = uint8_t(crc >> (24 - 8 * i));
}

int MemoryFile(const Bytes& bytes) {
    const int fd = memfd_create("fuzz", MFD_CLOEXEC);
    if (fd < 0 || (!bytes.empty() && write(fd, bytes.data(), bytes.size()) != ssize_t(bytes.size()))) std::abort();
    return fd;
}

void Save(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream(path, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    using namespace frondStorage;
    static unsigned runs = 0;
    FuzzedDataProvider data(bytes, size);
    const unsigned mode = data.ConsumeIntegralInRange(0, 5);
    const bool fix = data.ConsumeBool();
    try {
        switch (mode) {
            case 0: {
                auto file = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(0, 3 << 20));
                if (fix) FixContainer(file);
                sd::Info info;
                if (file.size() >= sd::headerBytes) {
                    sd::parseContainer(file.data(), info);
                    if (sd::parseContainer(file.data(), info) && info.version == sd::versionV2) {
                        std::vector<uint32_t> table;
                        const uint64_t at = sd::headerBytes + std::min<uint64_t>(info.stored(), file.size() - sd::headerBytes);
                        sd::readTable(file.data(), file.data() + at, file.size() - size_t(at), info, table);
                    }
                }
                Bytes payload;
                sd::decode(file.data(), file.size(), payload, &info);
                break;
            }
            case 1: {
                auto journal = data.ConsumeRemainingBytes<uint8_t>();
                if (fix) FixJournal(journal);
                Fd fd(MemoryFile(journal));
                Journal parsed;
                readJournal(fd.fd, journal.size(), parsed);
                break;
            }
            case 2: {
                auto commit = data.ConsumeRemainingBytes<uint8_t>();
                if (fix) FixCommit(commit);
                uint64_t length = 0;
                std::vector<legacy::Record> records;
                legacy::decodeCommit(commit, length, records);
                break;
            }
            case 3: {
                const auto pending = data.ConsumeRemainingBytes<uint8_t>();
                Fd fd(MemoryFile(pending));
                Pending parsed;
                readPending("/proc/self/fd/" + std::to_string(fd.fd), parsed);
                break;
            }
            case 4: {
                const auto& layout = frondMsLayout::layouts[data.ConsumeIntegralInRange<size_t>(0, 2)];
                const auto stored = data.ConsumeRemainingBytes<uint8_t>();
                // The stored bytes: the input's from the side region's start, zeros elsewhere.
                classicStick::ReadFn read = [&](uint64_t at, uint64_t n, void* dest) {
                    auto* out = static_cast<uint8_t*>(dest);
                    std::memset(out, 0, size_t(n));
                    const uint64_t base = layout.SideAt();
                    for (uint64_t i = 0; i < n; ++i)
                        if (at + i >= base && at + i - base < stored.size()) out[i] = stored[size_t(at + i - base)];
                    return true;
                };
                classicStick::State state;
                if (classicStick::load(layout, read, state))
                    for (uint32_t logical = 0; logical < 8 && logical < layout.UserBlocks(); ++logical) {
                        const auto claim = classicStick::plan(state, logical * 37 % layout.UserBlocks());
                        classicStick::apply(state, claim);
                    }
                break;
            }
            default: {
                const std::filesystem::path folder = FuzzScratch("card-container", runs);
                const auto card = folder / "card.frondsd";
                std::error_code ignored;
                for (const auto& extra : journalFiles(card)) std::filesystem::remove(extra, ignored);
                auto file = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(0, 3 << 20));
                if (fix) FixContainer(file);
                Save(card, file);
                if (data.ConsumeBool()) {
                    auto journal = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(0, 1 << 16));
                    if (data.ConsumeBool()) FixJournal(journal);
                    sd::Info info;
                    const uint64_t generation = file.size() >= sd::headerBytes && sd::parseContainer(file.data(), info) ? info.generation : 0;
                    Save(journalPath(card, generation + 1), journal);
                }
                auto opened = CardFile::open(card, data.ConsumeBool());
                FuzzFat(*opened, data);
                break;
            }
        }
    } catch (const Error&) {
    }
    return 0;
}
