#include "audio_queue.h"

#include "queue.h"
#include <utility>
#include <cstdlib>
#include <array>
#include <memory>
#include <algorithm>

#ifndef __EMSCRIPTEN__
    #define AUDIO_QUEUE_THREADSAFE
#endif

#ifdef AUDIO_QUEUE_THREADSAFE
    #include <mutex>

    #define AUDIO_QUEUE_LOCK std::lock_guard lock(audioQueue->mutex)
#else
    #define AUDIO_QUEUE_LOCK
#endif

struct AudioQueue {
    Queue<uint32_t> queue;
    // Geometric growth needs at most size_t's bit count of old segments.
    std::array<std::unique_ptr<Queue<uint32_t>>, sizeof(size_t) * 8> previous;
    size_t previousBegin{}, previousEnd{};
    size_t pending{};
    uint32_t Pop() {
        uint32_t result;
        if (previousBegin != previousEnd) {
            result = previous[previousBegin]->Pop();
            if (!previous[previousBegin]->GetSize()) ++previousBegin;
        } else result = queue.Pop();
        --pending;
        return result;
    }
    void Clear() { previousBegin = previousEnd; queue.Clear(); pending = 0; }
    size_t capacity;
    uint64_t generation{0};
    AudioQueueAccounting accounting{};
    uint32_t lastPushed{0};

    AudioQueue(size_t capacity) : queue(capacity), capacity(capacity) {}

#ifdef AUDIO_QUEUE_THREADSAFE
    std::mutex mutex;
#endif
};

struct AudioQueue* audioQueueCreate(size_t capacity) {
    AudioQueue* audioQueue = new AudioQueue(capacity);

    return audioQueue;
}

void audioQueueEnsureCapacity(struct AudioQueue* audioQueue, size_t capacity) {
    size_t existing;
    {
        AUDIO_QUEUE_LOCK;
        existing = audioQueue->capacity;
        if (capacity <= existing) return;
    }
    // Allocate outside the callback lock, retain the old ordered segment without copying.
    const size_t expandedCapacity = std::max(capacity,
        existing <= SIZE_MAX / 2 ? existing * 2 : capacity);
    Queue<uint32_t> expanded(expandedCapacity);
    auto oldSegment = std::make_unique<Queue<uint32_t>>(0);
    AUDIO_QUEUE_LOCK;
    if (capacity <= audioQueue->capacity) return;
    *oldSegment = std::move(audioQueue->queue);
    if (oldSegment->GetSize()) {
        if (audioQueue->previousEnd == audioQueue->previous.size()) std::abort();
        audioQueue->previous[audioQueue->previousEnd++] = std::move(oldSegment);
    }
    audioQueue->queue = std::move(expanded);
    audioQueue->capacity = expandedCapacity;
}

void audioQueuePush(struct AudioQueue* audioQueue, uint32_t sample) {
    AUDIO_QUEUE_LOCK;

    if (audioQueue->pending == audioQueue->capacity) { audioQueue->Pop(); ++audioQueue->accounting.discarded; }
    audioQueue->queue.Push(sample);
    ++audioQueue->pending;
    ++audioQueue->accounting.committed;
    // A constant level, zero or not, is silence; only a change counts as sound.
    if (sample != audioQueue->lastPushed) ++audioQueue->accounting.nonSilent;
    audioQueue->lastPushed = sample;
}

size_t audioQueuePopChunk(struct AudioQueue* audioQueue, uint32_t* destination, size_t count) {
    AUDIO_QUEUE_LOCK;

    size_t actualCount = 0;
    while (audioQueue->pending > 0 && actualCount < count)
        destination[actualCount++] = audioQueue->Pop();
    audioQueue->accounting.consumed += actualCount;

    return actualCount;
}

size_t audioQueuePendingSamples(struct AudioQueue* audioQueue) {
    AUDIO_QUEUE_LOCK;

    return audioQueue->pending;
}

AudioQueueState audioQueueGetState(struct AudioQueue* audioQueue) {
    AUDIO_QUEUE_LOCK;
    return {audioQueue->pending, audioQueue->generation};
}

size_t audioQueuePopChunkIfGeneration(struct AudioQueue* audioQueue, uint32_t* destination,
                                     size_t count, uint64_t generation) {
    AUDIO_QUEUE_LOCK;
    if (audioQueue->generation != generation) return 0;
    size_t actualCount = 0;
    while (audioQueue->pending > 0 && actualCount < count)
        destination[actualCount++] = audioQueue->Pop();
    audioQueue->accounting.consumed += actualCount;
    return actualCount;
}

void audioQueueClear(struct AudioQueue* audioQueue) {
    AUDIO_QUEUE_LOCK;

    audioQueue->accounting.discarded += audioQueue->pending;
    audioQueue->Clear();
    ++audioQueue->generation;
}

AudioQueueAccounting audioQueueAccounting(AudioQueue* audioQueue) {
    AUDIO_QUEUE_LOCK;
    return audioQueue->accounting;
}
