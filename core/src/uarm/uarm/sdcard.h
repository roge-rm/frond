#ifndef _SD_CARD_H_
#define _SD_CARD_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "buffer.h"

#include <string>

#define SD_SECTOR_SIZE 512
#define SD_CARD_ID_MAX_LEN 32

class CardBacking;
class CardSnapshot;

void sdCardInitialize(size_t sectors, const char* id);
void sdCardInitializeWithData(size_t sectors, void* buf, const char* id);
bool sdCardInitializeFromFile(const char* path, uint64_t payloadBytes, uint64_t payloadOffset,
                              const char* id);

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
bool sdCardRestore(const std::shared_ptr<const CardSnapshot>& snapshot);

#endif  // _SD_CARD_H_
