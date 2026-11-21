#include "sdcard.h"

#include "CardBacking.h"
#include "FileCardBacking.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace {
    using Tracked = std::vector<std::pair<uint32_t, std::vector<uint8_t>>>;

    // Writes the tracked sectors over the saved file's bytes in the backing the card moves onto.
    bool replayTracked(CardBacking& fresh, const Tracked& sectors) {
        for (const auto& [sector, bytes] : sectors)
            if (!fresh.Write(uint64_t(sector) * SD_SECTOR_SIZE, SD_SECTOR_SIZE, bytes.data())) return false;
        return true;
    }
}  // namespace

SdCardStore::SdCardStore() = default;
SdCardStore::~SdCardStore() = default;

void SdCardStore::noteWritten(uint64_t sector) {
    if (tracking && sector < writtenSectors.size()) writtenSectors[size_t(sector)] = true;
}

// The noted sectors' bytes from the card as it runs now, read before a move drops its files.
bool SdCardStore::takeTracked(Tracked& out) {
    out.clear();
    for (size_t sector = 0; sector < writtenSectors.size(); ++sector) {
        if (!writtenSectors[sector]) continue;
        std::vector<uint8_t> bytes(SD_SECTOR_SIZE);
        if (!backing->Read(uint64_t(sector) * SD_SECTOR_SIZE, SD_SECTOR_SIZE, bytes.data())) return false;
        out.emplace_back(uint32_t(sector), std::move(bytes));
    }
    return true;
}

// One bit per 16-sector page, in whole 32-bit words.
void SdCardStore::allocateDirtyPages(size_t sectors) {
    size_t words = sectors / (16 * 32);
    if ((words * 16 * 32) < sectors) words++;
    dirtyPageBits.assign(words, 0);
}

// The chunk table of a .frondsd v2 file, or none for v1 and other files.
bool SdCardStore::sidecarChunks(const char* path, uint64_t payloadBytes, uint64_t payloadOffset,
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

void SdCardStore::markDirty(uint32_t sector) {
    const uint32_t page = sector >> 4;
    dirtyPageBits[page / 32] |= (1u << (page % 32));
    dirty = true;
    dataViewValid = false;
}

void SdCardStore::initializeWithData(size_t sectors, void* buf, const char* id) {
    reset();
    auto* p = static_cast<uint8_t*>(buf);
    std::vector<uint8_t> bytes(p, p + sectors * SD_SECTOR_SIZE);
    free(buf);
    backing = std::make_unique<RamCardBacking>(std::move(bytes), SD_SECTOR_SIZE);
    sectorsTotal = sectors;
    dirty = false;
    dataViewValid = false;
    allocateDirtyPages(sectors);
    rekey(id);
}

void SdCardStore::initialize(size_t sectors, const char* id) {
    uint8_t* buf = reinterpret_cast<uint8_t*>(malloc(sectors * SD_SECTOR_SIZE));
    memset(buf, 0, sectors * SD_SECTOR_SIZE);
    initializeWithData(sectors, buf, id);
}

bool SdCardStore::read(uint32_t sector, void* buf) {
    if (!backing || sector >= sectorsTotal) return false;
    return backing->Read(uint64_t(sector) * SD_SECTOR_SIZE, SD_SECTOR_SIZE, buf);
}

void SdCardStore::rekey(const char* id) {
    if (!initialized()) return;
    strncpy(cardId, id, sizeof(cardId));
    cardId[sizeof(cardId) - 1] = '\0';
}

void SdCardStore::reset() {
    ++epochCount;
    backing.reset();
    backingStale = false;
    dataView.clear();
    dataViewValid = false;
    dirtyPageBits.clear();
    sectorsTotal = 0;
    dirty = false;
}

bool SdCardStore::readBytes(uint64_t offset, uint64_t length, void* data) {
    return backing && backing->Read(offset, length, data);
}

bool SdCardStore::writeBytes(uint64_t offset, uint64_t length, const void* data) {
    if (!backing || !backing->Write(offset, length, data)) return false;
    if (length)
        for (uint64_t sector = offset / SD_SECTOR_SIZE; sector <= (offset + length - 1) / SD_SECTOR_SIZE; ++sector) {
            noteWritten(sector);
            markDirty(uint32_t(sector));
        }
    return true;
}

void SdCardStore::initializeWithBytes(std::vector<uint8_t>&& bytes, const char* id) {
    reset();
    const size_t sectors = bytes.size() / SD_SECTOR_SIZE;
    backing = std::make_unique<RamCardBacking>(std::move(bytes), 4096);
    sectorsTotal = sectors;
    dirty = false;
    dataViewValid = false;
    allocateDirtyPages(sectors);
    rekey(id);
}

bool SdCardStore::adoptSavedFromRam(const char* path, uint64_t payloadBytes, uint64_t payloadOffset) {
    auto* ram = dynamic_cast<RamCardBacking*>(backing.get());
    if (!ram || ram->Pinned() || payloadBytes != ram->Size()) return false;
    FileCardOptions options;
    options.identity = cardId;
    if (!sidecarChunks(path, payloadBytes, payloadOffset, options.chunkCrcs)) return false;
    Tracked tracked;
    if (!takeTracked(tracked)) return false;
    options.keepHeld = true;
    auto held = FileCardBacking::HoldGenerations(path);
    if (!held.ok) return false;
    auto fresh = FileCardBacking::Open(path, payloadBytes, payloadOffset, options, adoptHooks);
    if (!fresh || !replayTracked(*fresh, tracked)) {
        fresh.reset();
        if (!held.Return()) std::fprintf(stderr, "frond: could not return the held card generations\n");
        return false;
    }
    if (!held.Drop()) std::fprintf(stderr, "frond: could not remove the held card generations\n");
    backing = std::move(fresh);
    backingStale = false;
    dataViewValid = false;
    return true;
}

bool SdCardStore::write(uint32_t sector, const void* buf) {
    if (!backing || sector >= sectorsTotal) return false;
    if (!backing->Write(uint64_t(sector) * SD_SECTOR_SIZE, SD_SECTOR_SIZE, buf)) return false;
    noteWritten(sector);
    markDirty(sector);
    return true;
}

bool SdCardStore::initializeFromFile(const char* path, uint64_t payloadBytes, uint64_t payloadOffset,
                                     const char* id) {
    if (!path || payloadBytes % SD_SECTOR_SIZE || !payloadBytes) return false;
    FileCardOptions options;
    options.recoverRootlessBase = true;
    options.identity = id ? id : "";
    if (!sidecarChunks(path, payloadBytes, payloadOffset, options.chunkCrcs)) return false;
    auto file = FileCardBacking::Open(path, payloadBytes, payloadOffset, options);
    if (!file) return false;
    reset();
    const size_t sectors = static_cast<size_t>(payloadBytes / SD_SECTOR_SIZE);
    backing = std::move(file);
    sectorsTotal = sectors;
    dirty = false;
    dataViewValid = false;
    allocateDirtyPages(sectors);
    rekey(id);
    return true;
}

bool SdCardStore::commit() { return backing && backing->Commit(); }

bool SdCardStore::fileBacked() const { return dynamic_cast<FileCardBacking*>(backing.get()) != nullptr; }

bool SdCardStore::backedBy(const char* path) const {
    auto* file = dynamic_cast<FileCardBacking*>(backing.get());
    return file && path && !backingStale && file->DurableIs(path);
}

bool SdCardStore::runsOn(const char* path) const {
    auto* file = dynamic_cast<FileCardBacking*>(backing.get());
    if (!file || !path) return false;
    std::error_code ec;
    return std::filesystem::equivalent(file->DurablePath(), path, ec) && !ec;
}

std::string SdCardStore::backingPath() const {
    auto* file = dynamic_cast<FileCardBacking*>(backing.get());
    return file ? file->DurablePath().string() : std::string();
}

bool SdCardStore::adoptSaved(const char* path, uint64_t payloadBytes, uint64_t payloadOffset) {
    auto* file = dynamic_cast<FileCardBacking*>(backing.get());
    if (!file || file->Pinned() || payloadBytes != file->Size()) return false;
    FileCardOptions options;
    options.recoverRootlessBase = true;
    options.identity = cardId;
    if (!sidecarChunks(path, payloadBytes, payloadOffset, options.chunkCrcs)) return false;
    // Sectors written after a background save's snapshot, which the sidecar doesn't hold.
    Tracked tracked;
    if (!takeTracked(tracked)) return false;
    // The sidecar now holds the live bytes, so generations beside it are never needed again; they
    // go once the card is on its new backing, and come back if it can't move.
    options.keepHeld = true;
    auto held = FileCardBacking::HoldGenerations(path);
    if (!held.ok) return false;
    auto fresh = FileCardBacking::Open(path, payloadBytes, payloadOffset, options, adoptHooks);
    if (!fresh || !replayTracked(*fresh, tracked)) {
        fresh.reset();
        if (!held.Return()) std::fprintf(stderr, "frond: could not return the held card generations\n");
        return false;
    }
    if (!held.Drop()) std::fprintf(stderr, "frond: could not remove the held card generations\n");
    backing = std::move(fresh);
    backingStale = false;
    dataViewValid = false;
    return true;
}

bool SdCardStore::pinned() const {
    auto* file = dynamic_cast<FileCardBacking*>(backing.get());
    return file && file->Pinned();
}

bool SdCardStore::forEachChange(const CardSnapshot& pin,
                                const std::function<bool(uint64_t offset, uint64_t length, const uint8_t* data)>& fn) {
    auto* file = dynamic_cast<FileCardBacking*>(backing.get());
    return file && file->ForEachOverlayIn(pin, fn);
}

bool SdCardStore::setChunkCrcs(std::vector<uint32_t> table) {
    auto* file = dynamic_cast<FileCardBacking*>(backing.get());
    return file && file->SetChunkCrcs(std::move(table));
}

void SdCardStore::trustChunks(const std::vector<uint64_t>& chunks) {
    if (auto* file = dynamic_cast<FileCardBacking*>(backing.get()))
        for (auto c : chunks) file->TrustChunk(c);
}

struct Buffer SdCardStore::data() {
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

struct Buffer SdCardStore::dirtyPages() {
    return (struct Buffer){.size = dirtyPageBits.size() * 4, .data = dirtyPageBits.empty() ? nullptr : dirtyPageBits.data()};
}

void SdCardStore::trackWrites(bool on) {
    tracking = on;
    writtenSectors.assign(on ? sectorsTotal : 0, false);
}

std::shared_ptr<const CardSnapshot> SdCardStore::snapshot() {
    if (!backing) return nullptr;
    return backing->Snapshot();
}

bool SdCardStore::restore(const std::shared_ptr<const CardSnapshot>& snapshot) {
    if (!backing || !snapshot) return false;
    if (!backing->Restore(*snapshot)) return false;
    dataViewValid = false;
    return true;
}

SdCardStore& sdCardStore(unsigned slot) {
    static SdCardStore stores[sdCardStoreCount];
    return stores[slot < sdCardStoreCount ? slot : 0];
}

bool sdCardStore1Read(uint32_t sector, void* data) { return sdCardStore(1).read(sector, data); }
bool sdCardStore1Write(uint32_t sector, const void* data) { return sdCardStore(1).write(sector, data); }

// Slot 0's store.
namespace {
    SdCardStore& first() { return sdCardStore(0); }
}  // namespace

void sdCardInitialize(size_t sectors, const char* id) { first().initialize(sectors, id); }
void sdCardInitializeWithData(size_t sectors, void* buf, const char* id) { first().initializeWithData(sectors, buf, id); }
bool sdCardInitializeFromFile(const char* path, uint64_t payloadBytes, uint64_t payloadOffset, const char* id) {
    return first().initializeFromFile(path, payloadBytes, payloadOffset, id);
}
void sdCardRekey(const char* id) { first().rekey(id); }
bool sdCardCommit() { return first().commit(); }
bool sdCardFileBacked() { return first().fileBacked(); }
bool sdCardBackedBy(const char* path) { return first().backedBy(path); }
bool sdCardRunsOn(const char* path) { return first().runsOn(path); }
void sdCardMarkStale() { first().markStale(); }
std::string sdCardBackingPath() { return first().backingPath(); }
bool sdCardPinned() { return first().pinned(); }
bool sdCardForEachChange(const CardSnapshot& pin,
                         const std::function<bool(uint64_t offset, uint64_t length, const uint8_t* data)>& fn) {
    return first().forEachChange(pin, fn);
}
bool sdCardSetChunkCrcs(std::vector<uint32_t> table) { return first().setChunkCrcs(std::move(table)); }
void sdCardTrustChunks(const std::vector<uint64_t>& chunks) { first().trustChunks(chunks); }
bool sdCardAdoptSaved(const char* path, uint64_t payloadBytes, uint64_t payloadOffset) {
    return first().adoptSaved(path, payloadBytes, payloadOffset);
}
void sdCardReset() { first().reset(); }
bool sdCardReadBytes(uint64_t offset, uint64_t length, void* data) { return first().readBytes(offset, length, data); }
bool sdCardWriteBytes(uint64_t offset, uint64_t length, const void* data) { return first().writeBytes(offset, length, data); }
void sdCardInitializeWithBytes(std::vector<uint8_t>&& bytes, const char* id) {
    first().initializeWithBytes(std::move(bytes), id);
}
uint64_t sdCardEpoch() { return first().epoch(); }
bool sdCardAdoptSavedFromRam(const char* path, uint64_t payloadBytes, uint64_t payloadOffset) {
    return first().adoptSavedFromRam(path, payloadBytes, payloadOffset);
}
bool sdCardInitialized() { return first().initialized(); }
bool sdCardRead(uint32_t sector, void* data) { return first().read(sector, data); }
bool sdCardWrite(uint32_t sector, const void* data) { return first().write(sector, data); }
size_t sdCardSectorCount() { return first().sectorCount(); }
struct Buffer sdCardData() { return first().data(); }
struct Buffer sdCardDirtyPages() { return first().dirtyPages(); }
bool sdCardIsDirty() { return first().isDirty(); }
void sdCardSetDirty(bool isDirty) { first().setDirty(isDirty); }
const char* sdCardGetId() { return first().getId(); }
std::shared_ptr<const CardSnapshot> sdCardSnapshot() { return first().snapshot(); }
void sdCardTrackWrites(bool on) { first().trackWrites(on); }
void sdCardSetAdoptHooks(FileCardIoHooks* hooks) { first().setAdoptHooks(hooks); }
bool sdCardRestore(const std::shared_ptr<const CardSnapshot>& snapshot) { return first().restore(snapshot); }
