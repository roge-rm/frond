#include "CardImage.h"

#include <algorithm>
#include <cstring>

CardImage::CardImage(uint8_t* data, size_t blocksTotal, bool readOnly, Protocol protocol)
    : data(data), blocksTotal(blocksTotal), readOnly(readOnly), protocol(protocol) {
    const size_t pageCount = (blocksTotal >> 4) + ((blocksTotal % 16 != 0) > 0 ? 1 : 0);
    const size_t dirtyPageBufferSize = (pageCount >> 3) + ((pageCount % 8) > 0 ? 1 : 0);

    dirtyPages = std::make_unique<uint8_t[]>(dirtyPageBufferSize);
    memset(dirtyPages.get(), 0, dirtyPageBufferSize);
}

CardImage::CardImage(std::shared_ptr<CardBacking> backing, bool readOnly,
                     std::function<bool()> flush, uint32_t serial, Protocol protocol)
    : CardImage(nullptr, backing->Size() / BLOCK_SIZE, readOnly, protocol) {
    this->backing = std::move(backing);
    this->flush = std::move(flush);
    this->serial = serial;
}

size_t CardImage::Read(uint8_t* dest, size_t index, size_t count) {
    if (index >= blocksTotal) return 0;

    count = std::min(count, blocksTotal - index);

    if (backing) return backing->Read(index * BLOCK_SIZE, count * BLOCK_SIZE, dest) ? count : 0;

    for (size_t i = 0; i < count; i++)
        memcpy(dest + i * BLOCK_SIZE, data.get() + (i + index) * BLOCK_SIZE, BLOCK_SIZE);

    return count;
}

size_t CardImage::Write(const uint8_t* source, size_t index, size_t count) {
    if (readOnly || index >= blocksTotal) return 0;

    count = std::min(count, blocksTotal - index);

    if (backing) return WriteByteRange(source, index * BLOCK_SIZE, count * BLOCK_SIZE) ? count : 0;

    for (size_t block = index; block < index + count; block++) {
        memcpy(data.get() + block * BLOCK_SIZE, source + (block - index) * BLOCK_SIZE, BLOCK_SIZE);

        const size_t page = block >> 4;
        dirtyPages[page >> 3] |= 1 << (page & 0x07);
    }

    return count;
}

size_t CardImage::BlocksTotal() const { return blocksTotal; }

void CardImage::BeginBatch(bool rollback) {
    batching = true;
    batchFailed = false;
    pendingFlush = false;
    batchPrior = backing && rollback ? backing->Snapshot() : nullptr;
    if (backing && rollback && !batchPrior) batchFailed = true;
}

// One commit for the batch, or the card goes back to how it was at BeginBatch.
bool CardImage::EndBatch() {
    batching = false;
    const bool pending = pendingFlush;
    pendingFlush = false;
    auto prior = std::move(batchPrior);
    if (batchFailed || (pending && (!flush || !flush()))) {
        if (backing && prior) backing->Restore(*prior);
        batchFailed = false;
        return false;
    }
    return true;
}

bool CardImage::WriteByteRange(const uint8_t* source, size_t offset, size_t count) {
    if (readOnly) return false;
    if (offset > blocksTotal * BLOCK_SIZE || count > blocksTotal * BLOCK_SIZE - offset) return false;
    if (count == 0) return true;

    if (backing) {
        if (batching) {
            if (batchFailed) return false;
            if (!backing->Write(offset, count, source)) { batchFailed = true; return false; }
            pendingFlush = true;
            MarkRangeDirty(offset, count);
            return true;
        }
        auto prior = backing->Snapshot();
        if (!prior) return false;
        if (!backing->Write(offset, count, source) || !flush || !flush()) {
            backing->Restore(*prior);
            return false;
        }
        MarkRangeDirty(offset, count);
        return true;
    }
    memcpy(data.get() + offset, source, count);

    MarkRangeDirty(offset, count);

    return true;
}

bool CardImage::ReadByteRange(uint8_t* destination, size_t offset, size_t count) {
    if (offset > blocksTotal * BLOCK_SIZE || count > blocksTotal * BLOCK_SIZE - offset) return false;

    if (backing) return backing->Read(offset, count, destination);
    memcpy(destination, data.get() + offset, count);

    return true;
}

void CardImage::MarkRangeDirty(size_t offset, size_t count) {
    if (readOnly || !count || offset > blocksTotal * BLOCK_SIZE || count > blocksTotal * BLOCK_SIZE - offset) return;
    const size_t firstBlock = offset / BLOCK_SIZE;
    const size_t lastBlock = (offset + count - 1) / BLOCK_SIZE;

    for (size_t block = firstBlock; block <= lastBlock; block++) {
        const size_t page = block >> 4;
        dirtyPages[page >> 3] |= 1 << (page & 0x07);
    }
}

uint8_t* CardImage::RawData() { return data.get(); }

uint8_t* CardImage::DirtyPages() { return dirtyPages.get(); }
