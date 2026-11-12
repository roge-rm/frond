#ifndef _SAVESTATE_H_
#define _SAVESTATE_H_

#include <memory>
#include <unordered_map>

#include "CPEndian.h"
#include "Chunk.h"
#include "Logging.h"
#include "SavestateProbe.h"

template <typename ChunkType>
class Savestate {
   public:
    using chunkMapT = std::unordered_map<ChunkType, Chunk>;
    using chunkT = Chunk;

   public:
    Savestate() = default;

    template <typename T>
    bool Save(T& t);

    Chunk* GetChunk(ChunkType type);
    Chunk* GetChunk(ChunkType type, uint32_t version);

    void NotifyError();

    void* GetBuffer() { return buffer.get(); }
    size_t GetSize() const { return size; }

    void Reset();

   private:
    template <typename T>
    bool AllocateBuffer(T& t);
    template <typename T>
    bool SaveChunks(T& t);

    // No chunk was asked for that the layout lacks. A chunk that came out short is fine to load.
    bool LayoutHeld() const;

   private:
    std::unique_ptr<uint32_t[]> buffer;
    size_t size{0};

    bool error{false};
    bool missingChunk{false};

    chunkMapT chunkMap;

   private:
    Savestate(const Savestate&) = delete;
    Savestate(Savestate&&) = delete;
    Savestate& operator=(const Savestate&) = delete;
    Savestate& operator=(Savestate&&) = delete;
};

///////////////////////////////////////////////////////////////////////////////
// IMPLEMENTATION
///////////////////////////////////////////////////////////////////////////////

template <typename ChunkType>
template <typename T>
bool Savestate<ChunkType>::Save(T& target) {
    if (!buffer && !AllocateBuffer(target)) {
        error = true;
        return false;
    }

    if (SaveChunks(target) && LayoutHeld()) return true;

    // The layout is probed at the first save, and the machine can change it since: a card in a
    // second slot or an MMC card adds words, taking one out leaves fewer. Probe again and save once more.
    Reset();
    if (!AllocateBuffer(target)) {
        error = true;
        return false;
    }

    return SaveChunks(target);
}

template <typename ChunkType>
template <typename T>
bool Savestate<ChunkType>::SaveChunks(T& target) {
    error = false;
    missingChunk = false;
    for (auto& [chunkType, chunk] : chunkMap) chunk.Reset();

    target.Save(*this);

    for (auto& [chunkType, chunk] : chunkMap) error = error || chunk.HasError();

    return !error;
}

template <typename ChunkType>
bool Savestate<ChunkType>::LayoutHeld() const {
    return !missingChunk;
}

template <typename ChunkType>
template <typename T>
bool Savestate<ChunkType>::AllocateBuffer(T& target) {
    SavestateProbe<ChunkType> probe;

    target.Save(probe);

    if (probe.HasError()) {
        logPrintf("failed to determine savestate layout");
        return false;
    }

    const typename SavestateProbe<ChunkType>::chunkMapT& probeMap(probe.GetChunkMap());
    size_t chunkCount = probeMap.size();
    size = 4;

    for (auto& [chunkType, chunk] : probeMap) size = size + chunk.GetSize() * 4 + 8;

    buffer = std::make_unique<uint32_t[]>(size / sizeof(uint32_t));

    uint32_t* nextTocEntry = buffer.get();
    *(nextTocEntry++) = chunkCount;

    uint32_t* nextChunk = buffer.get() + 1 + 2 * chunkCount;

    for (auto& [chunkType, chunk] : probeMap) {
        *nextTocEntry = static_cast<uint32_t>(chunkType);
        *(nextTocEntry + 1) = chunk.GetSize();

#if (__BYTE_ORDER == __BIG_ENDIAN)
        *nextTocEntry = htole32(*nextTocEntry);
        *(nextTocEntry + 1) = htole32(*(nextTocEntry + 1));
#endif

        nextTocEntry += 2;

        chunkMap.emplace(chunkType, Chunk(chunk.GetSize(), nextChunk));
        nextChunk += chunk.GetSize();
    }

    return true;
}

template <typename ChunkType>
Chunk* Savestate<ChunkType>::GetChunk(ChunkType type) {
    if (!buffer) {
        logPrintf("tried to request chunk before probing layout");
        return nullptr;
    }

    if (chunkMap.find(type) == chunkMap.end()) {
        missingChunk = true;
        return nullptr;
    }

    Chunk& chunk = chunkMap.at(type);

    error = error || chunk.HasError();
    chunk.Reset();

    return &chunk;
}

template <typename ChunkType>
Chunk* Savestate<ChunkType>::GetChunk(ChunkType type, uint32_t version) {
    Chunk* chunk = GetChunk(type);
    if (!chunk) {
        NotifyError();
        return nullptr;
    }

    chunk->Put32(version);

    return chunk;
}

template <typename ChunkType>
void Savestate<ChunkType>::NotifyError() {
    error = true;
}

template <typename ChunkType>
void Savestate<ChunkType>::Reset() {
    buffer.reset();
    size = 0;
    error = false;
    missingChunk = false;
    chunkMap.clear();
}

#endif  // _SAVESTATE_H_
