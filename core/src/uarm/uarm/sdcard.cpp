#include "sdcard.h"

#include "CardBacking.h"
#include "FileCardBacking.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {
    size_t sectorsTotal = 0;
    bool sdCardDirty = false;

    std::unique_ptr<CardBacking> backing;
    // A save moved the sidecar on but the card couldn't follow; it isn't backed by any sidecar
    // until it's replaced or adopted, so the next save copies it whole.
    bool backingStale = false;
    std::vector<uint8_t> dataView;
    bool dataViewValid = false;

    uint32_t* dirtyPages = NULL;
    size_t dirtyPagesSize = 0;

    char cardId[SD_CARD_ID_MAX_LEN + 1];
    uint64_t epoch = 0;

    void allocateDirtyPages(size_t sectors) {
        size_t dirtyPagesSize4 = sectors / (16 * 32);
        if ((dirtyPagesSize4 * 16 * 32) < sectors) dirtyPagesSize4++;
        dirtyPagesSize = dirtyPagesSize4 * 4;
        dirtyPages = reinterpret_cast<uint32_t*>(malloc(dirtyPagesSize));
        memset(dirtyPages, 0, dirtyPagesSize);
    }

    // The chunk table of a .frondsd v2 file, or none for v1 and other files.
    bool sidecarChunks(const char* path, uint64_t payloadBytes, uint64_t payloadOffset,
                       std::vector<uint32_t>& table) {
        table.clear();
        if (payloadOffset != frondSdFile::headerBytes) return true;
        FILE* f = fopen(path, "rb");
        if (!f) return false;
        uint8_t header[frondSdFile::headerBytes];
        const bool read = fread(header, 1, sizeof(header), f) == sizeof(header);
        frondSdFile::Info info;
        if (!read || !frondSdFile::parseContainer(header, info) || info.version != frondSdFile::versionV2) {
            fclose(f);
            return true;
        }
        if (info.stored() != payloadBytes) {
            fclose(f);
            return false;
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(4 * frondSdFile::chunkCount(payloadBytes)));
        const bool ok = fseeko(f, off_t(frondSdFile::headerBytes + payloadBytes), SEEK_SET) == 0 &&
                        fread(bytes.data(), 1, bytes.size(), f) == bytes.size() &&
                        frondSdFile::readTable(header, bytes.data(), bytes.size(), info, table);
        fclose(f);
        return ok;
    }

    void markDirty(uint32_t sector) {
        const uint32_t page = sector >> 4;
        dirtyPages[page / 32] |= (1u << (page % 32));
        sdCardDirty = true;
        dataViewValid = false;
    }
}  // namespace

void sdCardInitializeWithData(size_t sectors, void* buf, const char* id) {
    sdCardReset();

    size_t dirtyPagesSize4 = sectors / (16 * 32);
    if ((dirtyPagesSize4 * 16 * 32) < sectors) dirtyPagesSize4++;

    auto* p = static_cast<uint8_t*>(buf);
    std::vector<uint8_t> bytes(p, p + sectors * SD_SECTOR_SIZE);
    free(buf);
    backing = std::make_unique<RamCardBacking>(std::move(bytes), SD_SECTOR_SIZE);
    sectorsTotal = sectors;
    dirtyPagesSize = dirtyPagesSize4 * 4;
    sdCardDirty = false;
    dataViewValid = false;

    dirtyPages = reinterpret_cast<uint32_t*>(malloc(dirtyPagesSize));
    memset(dirtyPages, 0, dirtyPagesSize);

    sdCardRekey(id);
}

void sdCardInitialize(size_t sectors, const char* id) {
    uint8_t* buf = reinterpret_cast<uint8_t*>(malloc(sectors * SD_SECTOR_SIZE));
    memset(buf, 0, sectors * SD_SECTOR_SIZE);

    sdCardInitializeWithData(sectors, buf, id);
}

bool sdCardRead(uint32_t sector, void* buf) {
    if (!backing || sector >= sectorsTotal) return false;
    return backing->Read(uint64_t(sector) * SD_SECTOR_SIZE, SD_SECTOR_SIZE, buf);
}

void sdCardRekey(const char* id) {
    if (!sdCardInitialized()) return;

    strncpy(cardId, id, sizeof(cardId));
    cardId[sizeof(cardId) - 1] = '\0';
}

void sdCardReset() {
    ++epoch;
    if (dirtyPages) free(dirtyPages);
    backing.reset();
    backingStale = false;
    dataView.clear();
    dataViewValid = false;

    dirtyPages = NULL;

    sectorsTotal = 0;
    dirtyPagesSize = 0;
    sdCardDirty = false;
}

bool sdCardInitialized() { return backing != nullptr; }

uint64_t sdCardEpoch() { return epoch; }

bool sdCardReadBytes(uint64_t offset, uint64_t length, void* data) {
    return backing && backing->Read(offset, length, data);
}

bool sdCardWriteBytes(uint64_t offset, uint64_t length, const void* data) {
    if (!backing || !backing->Write(offset, length, data)) return false;
    if (length)
        for (uint64_t sector = offset / SD_SECTOR_SIZE; sector <= (offset + length - 1) / SD_SECTOR_SIZE; ++sector)
            markDirty(uint32_t(sector));
    return true;
}

void sdCardInitializeWithBytes(std::vector<uint8_t>&& bytes, const char* id) {
    sdCardReset();
    const size_t sectors = bytes.size() / SD_SECTOR_SIZE;
    backing = std::make_unique<RamCardBacking>(std::move(bytes), 4096);
    sectorsTotal = sectors;
    sdCardDirty = false;
    dataViewValid = false;
    allocateDirtyPages(sectors);
    sdCardRekey(id);
}

bool sdCardAdoptSavedFromRam(const char* path, uint64_t payloadBytes, uint64_t payloadOffset) {
    auto* ram = dynamic_cast<RamCardBacking*>(backing.get());
    if (!ram || ram->Pinned() || payloadBytes != ram->Size()) return false;
    FileCardOptions options;
    options.identity = cardId;
    if (!sidecarChunks(path, payloadBytes, payloadOffset, options.chunkCrcs)) return false;
    FileCardBacking::DiscardGenerations(path);
    auto fresh = FileCardBacking::Open(path, payloadBytes, payloadOffset, options);
    if (!fresh) return false;
    backing = std::move(fresh);
    backingStale = false;
    dataViewValid = false;
    return true;
}

bool sdCardWrite(uint32_t sector, const void* buf) {
    if (!backing || sector >= sectorsTotal) return false;
    if (!backing->Write(uint64_t(sector) * SD_SECTOR_SIZE, SD_SECTOR_SIZE, buf)) return false;
    markDirty(sector);
    return true;
}

size_t sdCardSectorCount() { return sectorsTotal; }

bool sdCardInitializeFromFile(const char* path, uint64_t payloadBytes, uint64_t payloadOffset,
                              const char* id) {
    if (!path || payloadBytes % SD_SECTOR_SIZE || !payloadBytes) return false;
    FileCardOptions options;
    options.recoverRootlessBase = true;
    options.identity = id ? id : "";
    if (!sidecarChunks(path, payloadBytes, payloadOffset, options.chunkCrcs)) return false;
    auto file = FileCardBacking::Open(path, payloadBytes, payloadOffset, options);
    if (!file) return false;
    sdCardReset();
    const size_t sectors = static_cast<size_t>(payloadBytes / SD_SECTOR_SIZE);
    size_t dirtyPagesSize4 = sectors / (16 * 32);
    if ((dirtyPagesSize4 * 16 * 32) < sectors) dirtyPagesSize4++;
    backing = std::move(file);
    sectorsTotal = sectors;
    dirtyPagesSize = dirtyPagesSize4 * 4;
    sdCardDirty = false;
    dataViewValid = false;
    dirtyPages = reinterpret_cast<uint32_t*>(malloc(dirtyPagesSize));
    memset(dirtyPages, 0, dirtyPagesSize);
    sdCardRekey(id);
    return true;
}

bool sdCardCommit() { return backing && backing->Commit(); }

bool sdCardFileBacked() { return dynamic_cast<FileCardBacking*>(backing.get()) != nullptr; }

bool sdCardBackedBy(const char* path) {
    auto* file = dynamic_cast<FileCardBacking*>(backing.get());
    return file && path && !backingStale && file->DurableIs(path);
}

bool sdCardRunsOn(const char* path) {
    auto* file = dynamic_cast<FileCardBacking*>(backing.get());
    if (!file || !path) return false;
    std::error_code ec;
    return std::filesystem::equivalent(file->DurablePath(), path, ec) && !ec;
}

void sdCardMarkStale() { backingStale = true; }

std::string sdCardBackingPath() {
    auto* file = dynamic_cast<FileCardBacking*>(backing.get());
    return file ? file->DurablePath().string() : std::string();
}

bool sdCardAdoptSaved(const char* path, uint64_t payloadBytes, uint64_t payloadOffset) {
    auto* file = dynamic_cast<FileCardBacking*>(backing.get());
    if (!file || file->Pinned() || payloadBytes != file->Size()) return false;
    FileCardOptions options;
    options.recoverRootlessBase = true;
    options.identity = cardId;
    if (!sidecarChunks(path, payloadBytes, payloadOffset, options.chunkCrcs)) return false;
    // The sidecar now holds the live bytes, so generations beside it are never needed again.
    FileCardBacking::DiscardGenerations(path);
    auto fresh = FileCardBacking::Open(path, payloadBytes, payloadOffset, options);
    if (!fresh) return false;
    backing = std::move(fresh);
    backingStale = false;
    dataViewValid = false;
    return true;
}

bool sdCardPinned() {
    auto* file = dynamic_cast<FileCardBacking*>(backing.get());
    return file && file->Pinned();
}

bool sdCardForEachChange(const CardSnapshot& pin,
                         const std::function<bool(uint64_t offset, uint64_t length, const uint8_t* data)>& fn) {
    auto* file = dynamic_cast<FileCardBacking*>(backing.get());
    return file && file->ForEachOverlayIn(pin, fn);
}

bool sdCardSetChunkCrcs(std::vector<uint32_t> table) {
    auto* file = dynamic_cast<FileCardBacking*>(backing.get());
    return file && file->SetChunkCrcs(std::move(table));
}

void sdCardTrustChunks(const std::vector<uint64_t>& chunks) {
    if (auto* file = dynamic_cast<FileCardBacking*>(backing.get()))
        for (auto c : chunks) file->TrustChunk(c);
}

struct Buffer sdCardData() {
    if (!backing) return (struct Buffer){.size = 0, .data = NULL};
    if (auto* ram = dynamic_cast<RamCardBacking*>(backing.get())) {
        if (const uint8_t* p = ram->ContiguousData()) {
            return (struct Buffer){.size = static_cast<size_t>(backing->Size()),
                                   .data = const_cast<uint8_t*>(p)};
        }
    }
    if (!dataViewValid) {
        dataView.resize(static_cast<size_t>(backing->Size()));
        if (!dataView.empty() && !backing->Read(0, backing->Size(), dataView.data()))
            return (struct Buffer){.size = 0, .data = NULL};
        dataViewValid = true;
    }
    return (struct Buffer){.size = dataView.size(), .data = dataView.data()};
}

struct Buffer sdCardDirtyPages() {
    return (struct Buffer){.size = dirtyPagesSize, .data = dirtyPages};
}

bool sdCardIsDirty() { return sdCardDirty; }

void sdCardSetDirty(bool isDirty) { sdCardDirty = isDirty; }

const char* sdCardGetId() { return sdCardInitialized() ? cardId : ""; }

std::pair<uint64_t, uint64_t> sdCardChangeMark() {
    if (!backing) return {0, 0};
    return {backing->Serial(), backing->Changes()};
}

std::shared_ptr<const CardSnapshot> sdCardSnapshot() {
    if (!backing) return nullptr;
    return backing->Snapshot();
}

bool sdCardRestore(const std::shared_ptr<const CardSnapshot>& snapshot) {
    if (!backing || !snapshot) return false;
    if (!backing->Restore(*snapshot)) return false;
    dataViewValid = false;
    return true;
}
