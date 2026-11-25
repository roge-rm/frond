#include "memory_buffer.h"

#include "guest_memory.h"

bool memoryBufferAllocate(struct MemoryBuffer* memoryBuffer, size_t size) {
    if (size % MEMORY_BUFFER_GRANULARITY) return false;

    size_t pageCount = size / 1024;

    auto* buffer = static_cast<uint8_t*>(GuestMemory::allocateShared(size));
    if (!buffer) buffer = static_cast<uint8_t*>(calloc(size, 1));
    if (!buffer) return false;

    const size_t dirtyPagesSize = ((pageCount + 31) / 32) * sizeof(uint32_t);
    auto* dirtyPages = static_cast<uint32_t*>(calloc(dirtyPagesSize, 1));
    if (!dirtyPages) {
        if (!GuestMemory::releaseShared(buffer)) free(buffer);
        return false;
    }

    *memoryBuffer = {size, dirtyPagesSize, buffer, dirtyPages, false};

    return true;
}

void memoryBufferRelease(struct MemoryBuffer* memoryBuffer) {
    if (memoryBuffer->isSubBuffer) return;

    if (!GuestMemory::releaseShared(memoryBuffer->buffer)) free(memoryBuffer->buffer);
    free(memoryBuffer->dirtyPages);
    *memoryBuffer = {};
}

bool memoryBufferGetSubBuffer(struct MemoryBuffer* memoryBuffer, struct MemoryBuffer* subBuffer,
                              size_t offset, size_t size) {
    if (size % MEMORY_BUFFER_GRANULARITY || offset % MEMORY_BUFFER_GRANULARITY ||
        offset + size > memoryBuffer->size)
        return false;

    subBuffer->size = size;
    subBuffer->dirtyPagesSize = size / (1024 * 8);
    subBuffer->buffer = memoryBuffer->buffer + offset;
    subBuffer->dirtyPages = memoryBuffer->dirtyPages + offset / (1024 * 32);
    subBuffer->isSubBuffer = true;

    return true;
}

void memoryBufferMarkRangeDirty(struct MemoryBuffer* memoryBuffer, size_t address, size_t size) {
    if (size == 0) return;

    for (size_t page = address >> 10; page <= (address + size - 1) >> 10; page++)
        memoryBuffer->dirtyPages[page >> 5] |= (1 << (page & 0x1f));
}

bool memoryBufferValid(struct MemoryBuffer* memoryBuffer) { return memoryBuffer->buffer; }
