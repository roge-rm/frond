#ifndef _CARD_IMAGE_H_
#define _CARD_IMAGE_H_

#include <cstdint>
#include <memory>
#include <functional>
#include "../uarm/uarm/CardBacking.h"

class CardImage {
   public:
    enum class Protocol : uint32_t { sd = 0, mmc = 1 };
    Protocol GetProtocol() const { return protocol; }
    // Those two values cannot be changed --- they're here for documentation only!
    constexpr static size_t BLOCK_SIZE = 512;
    constexpr static size_t DIRTY_PAGE_SIZE = 8192;

   public:
    CardImage(uint8_t* data, size_t blocksTotal, bool readOnly = false, Protocol protocol = Protocol::sd);
    CardImage(std::shared_ptr<CardBacking> backing, bool readOnly, std::function<bool()> flush,
              uint32_t serial, Protocol protocol = Protocol::sd);
    uint32_t Serial() const { return serial; }
    bool IsReadOnly() const { return readOnly; }
    // Byte-range writes between Begin and EndBatch commit once, at EndBatch.
    // `rollback` takes a snapshot to restore if the batch fails; without it a failed write stays.
    void BeginBatch(bool rollback = true);
    bool EndBatch();

    size_t Read(uint8_t* dest, size_t index, size_t count = 1);
    size_t Write(const uint8_t* source, size_t index, size_t count = 1);
    size_t BlocksTotal() const;

    bool WriteByteRange(const uint8_t* source, size_t offset, size_t count);
    bool ReadByteRange(uint8_t* destination, size_t offset, size_t count);

    void MarkRangeDirty(size_t offset, size_t count);

    uint8_t* RawData();
    uint8_t* DirtyPages();

   private:
    std::unique_ptr<uint8_t[]> data;
    std::unique_ptr<uint8_t[]> dirtyPages;
    size_t blocksTotal;
    const bool readOnly;
    const Protocol protocol;
    std::shared_ptr<CardBacking> backing;
    std::function<bool()> flush;
    bool batching = false, pendingFlush = false, batchFailed = false;
    std::shared_ptr<const CardSnapshot> batchPrior;
    uint32_t serial{0x00004aec};
};

#endif  // _CARD_IMAGE_H_
