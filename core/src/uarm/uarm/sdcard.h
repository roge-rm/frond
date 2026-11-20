#ifndef _SD_CARD_H_
#define _SD_CARD_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "buffer.h"

#include <string>
#include <utility>
#ifdef _WIN32
#include "PosixCompat.h"
#endif

#define SD_SECTOR_SIZE 512
#define SD_CARD_ID_MAX_LEN 32

class CardBacking;
class CardSnapshot;
struct FileCardIoHooks;

// A card store: one card's bytes on a backing (RAM or a .frondsd file), its dirty pages and id.
// Slot 0 is the store every device has; the sdCard* functions below act on it. The Zodiac's
// second slot (the W4200's) has slot 1. Each store is used from the emulation thread only.
class SdCardStore {
   public:
    SdCardStore();
    ~SdCardStore();
    SdCardStore(const SdCardStore&) = delete;
    SdCardStore& operator=(const SdCardStore&) = delete;

    void initialize(size_t sectors, const char* id);
    void initializeWithData(size_t sectors, void* buf, const char* id);
    bool initializeFromFile(const char* path, uint64_t payloadBytes, uint64_t payloadOffset, const char* id);
    void initializeWithBytes(std::vector<uint8_t>&& bytes, const char* id);
    void rekey(const char* id);
    bool commit();
    bool fileBacked() const;
    bool backedBy(const char* path) const;
    bool runsOn(const char* path) const;
    void markStale() { backingStale = true; }
    std::string backingPath() const;
    bool pinned() const;
    bool forEachChange(const CardSnapshot& pin,
                       const std::function<bool(uint64_t offset, uint64_t length, const uint8_t* data)>& fn);
    bool setChunkCrcs(std::vector<uint32_t> table);
    void trustChunks(const std::vector<uint64_t>& chunks);
    bool adoptSaved(const char* path, uint64_t payloadBytes, uint64_t payloadOffset);
    bool adoptSavedFromRam(const char* path, uint64_t payloadBytes, uint64_t payloadOffset);
#ifdef _WIN32
    // The same with a std::filesystem path's wchar_t c_str().
    bool initializeFromFile(const wchar_t* path, uint64_t payloadBytes, uint64_t payloadOffset, const char* id) {
        return initializeFromFile(frondPosix::Narrow(path).c_str(), payloadBytes, payloadOffset, id);
    }
    bool backedBy(const wchar_t* path) const { return backedBy(frondPosix::Narrow(path).c_str()); }
    bool runsOn(const wchar_t* path) const { return runsOn(frondPosix::Narrow(path).c_str()); }
    bool adoptSaved(const wchar_t* path, uint64_t payloadBytes, uint64_t payloadOffset) {
        return adoptSaved(frondPosix::Narrow(path).c_str(), payloadBytes, payloadOffset);
    }
    bool adoptSavedFromRam(const wchar_t* path, uint64_t payloadBytes, uint64_t payloadOffset) {
        return adoptSavedFromRam(frondPosix::Narrow(path).c_str(), payloadBytes, payloadOffset);
    }
#endif
    void reset();
    bool readBytes(uint64_t offset, uint64_t length, void* data);
    bool writeBytes(uint64_t offset, uint64_t length, const void* data);
    uint64_t epoch() const { return epochCount; }
    bool initialized() const { return backing != nullptr; }
    bool read(uint32_t sector, void* data);
    bool write(uint32_t sector, const void* data);
    size_t sectorCount() const { return sectorsTotal; }
    struct Buffer data();
    struct Buffer dirtyPages();
    bool isDirty() const { return dirty; }
    void setDirty(bool isDirty) { dirty = isDirty; }
    const char* getId() const { return initialized() ? cardId : ""; }
    std::shared_ptr<const CardSnapshot> snapshot();
    void trackWrites(bool on);
    void setAdoptHooks(FileCardIoHooks* hooks) { adoptHooks = hooks; }
    bool restore(const std::shared_ptr<const CardSnapshot>& snapshot);

   private:
    bool sidecarChunks(const char* path, uint64_t payloadBytes, uint64_t payloadOffset, std::vector<uint32_t>& table);
    bool takeTracked(std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& out);
    void noteWritten(uint64_t sector);
    void markDirty(uint32_t sector);
    void allocateDirtyPages(size_t sectors);

    size_t sectorsTotal = 0;
    bool dirty = false;
    std::unique_ptr<CardBacking> backing;
    // A save moved the sidecar on but the card couldn't follow; it isn't backed by any sidecar
    // until it's replaced or adopted, so the next save copies it whole.
    bool backingStale = false;
    std::vector<uint8_t> dataView;
    bool dataViewValid = false;
    std::vector<uint32_t> dirtyPageBits;
    char cardId[SD_CARD_ID_MAX_LEN + 1]{};
    uint64_t epochCount = 0;
    // Test hooks for the Open a move onto a saved file makes.
    FileCardIoHooks* adoptHooks = nullptr;
    // Sectors written while a background save writes (trackWrites), one bit each, so the note
    // never outgrows the card.
    bool tracking = false;
    std::vector<bool> writtenSectors;
};

constexpr unsigned sdCardStoreCount = 2;
// The store for `slot`: 0 for the first (and usually only) card slot, 1 for the Zodiac's second.
SdCardStore& sdCardStore(unsigned slot);
// Slot 1's sectors as vSD callbacks.
bool sdCardStore1Read(uint32_t sector, void* data);
bool sdCardStore1Write(uint32_t sector, const void* data);

// Slot 0's store, as before stores were per slot.
void sdCardInitialize(size_t sectors, const char* id);
void sdCardInitializeWithData(size_t sectors, void* buf, const char* id);
bool sdCardInitializeFromFile(const char* path, uint64_t payloadBytes, uint64_t payloadOffset,
                              const char* id);
#ifdef _WIN32
inline bool sdCardInitializeFromFile(const wchar_t* path, uint64_t payloadBytes, uint64_t payloadOffset,
                                     const char* id) {
    return sdCardInitializeFromFile(frondPosix::Narrow(path).c_str(), payloadBytes, payloadOffset, id);
}
#endif

void sdCardRekey(const char* id);
bool sdCardCommit();
bool sdCardFileBacked();
// True when the live card is file-backed on the file now at `path` (same inode) and hasn't been
// marked stale. Saves journal against the sidecar only then.
bool sdCardBackedBy(const char* path);
// True when the live card was opened from `path`, even if a rename has since put a new file there.
bool sdCardRunsOn(const char* path);
// After a save replaced the card's file and the card couldn't move onto the new one.
void sdCardMarkStale();
// The file a file-backed card runs on, or empty for a card in RAM.
std::string sdCardBackingPath();
// A .frondsd v2 sidecar's chunks are checked as they are first read; a bad chunk fails the read.
// True while a snapshot of a file-backed card is held.
bool sdCardPinned();
// Pages of a pinned file-backed card that differ from its sidecar, in order.
bool sdCardForEachChange(const CardSnapshot& pin,
                         const std::function<bool(uint64_t offset, uint64_t length, const uint8_t* data)>& fn);
// New chunk table after the sidecar was rewritten in place.
bool sdCardSetChunkCrcs(std::vector<uint32_t> table);
// Stops checking these chunks of a file-backed card.
void sdCardTrustChunks(const std::vector<uint64_t>& chunks);
// Moves a file-backed card onto the sidecar a save has just published, with the same bytes.
// Its old generations are dropped. Refused while a snapshot is pinned.
bool sdCardAdoptSaved(const char* path, uint64_t payloadBytes, uint64_t payloadOffset);

void sdCardReset();

// A classic Memory Stick keeps its v2 .frondms stored bytes in the store: reads and writes at any
// offset, a stored image moved in without a copy, and a count of every initialize and reset, so a
// holder can tell the card it attached is still the one there.
bool sdCardReadBytes(uint64_t offset, uint64_t length, void* data);
bool sdCardWriteBytes(uint64_t offset, uint64_t length, const void* data);
void sdCardInitializeWithBytes(std::vector<uint8_t>&& bytes, const char* id);
uint64_t sdCardEpoch();
// As sdCardAdoptSaved, for a card in RAM whose bytes a save has just written to `path`.
bool sdCardAdoptSavedFromRam(const char* path, uint64_t payloadBytes, uint64_t payloadOffset);

bool sdCardInitialized();

bool sdCardRead(uint32_t sector, void* data);
bool sdCardWrite(uint32_t sector, const void* data);

size_t sdCardSectorCount();

struct Buffer sdCardData();
struct Buffer sdCardDirtyPages();

bool sdCardIsDirty();
void sdCardSetDirty(bool isDirty);

const char* sdCardGetId();

std::shared_ptr<const CardSnapshot> sdCardSnapshot();
// From a background save's snapshot until it finishes, sectors the guest writes are noted, and
// sdCardAdoptSaved and sdCardAdoptSavedFromRam copy them from the running card into the saved
// file's backing, which holds the snapshot. Turning it on or off forgets the list.
void sdCardTrackWrites(bool on);
// Tests only: I/O hooks for the backing sdCardAdoptSaved and sdCardAdoptSavedFromRam open.
void sdCardSetAdoptHooks(FileCardIoHooks* hooks);
bool sdCardRestore(const std::shared_ptr<const CardSnapshot>& snapshot);

#endif  // _SD_CARD_H_
