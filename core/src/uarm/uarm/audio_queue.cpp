#include "audio_queue.h"

#include "queue.h"
#include <utility>
#include <cstdlib>
#include <array>
#include <memory>
#include <algorithm>

    #define AUDIO_QUEUE_THREADSAFE

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
    void Clear() { previousBegin = previousEnd; queue.Clear(); pending = 0; markCount = 0; }
    size_t capacity;
    uint64_t generation{0};
    AudioQueueAccounting accounting{};
    uint32_t lastPushed{0};

    // Guest time marks, oldest first from markFirst, in a ring. At least markSpacingNs apart unless no frame came
    // in between, so the ring covers a few seconds, more than the deepest queue.
    struct Mark { uint64_t committed, guestNs; };
    static constexpr size_t markSlots = 512;
    static constexpr uint64_t markSpacingNs = 5000000, frameRate = 44100;
    std::array<Mark, markSlots> marks{};
    size_t markFirst{}, markCount{};
    const Mark& MarkAt(size_t i) const { return marks[(markFirst + i) % markSlots]; }
    // Guest ns of frame `index` (counted as committed counts them), from the newest mark at or before it.
    uint64_t GuestNsOf(uint64_t index) const {
        size_t i = markCount;
        while (i > 0 && MarkAt(i - 1).committed > index) --i;
        if (i == 0) {
            const Mark& oldest = MarkAt(0);
            const uint64_t back = (oldest.committed - index) * 1000000000ull / frameRate;
            return oldest.guestNs > back ? oldest.guestNs - back : 0;
        }
        const Mark& m = MarkAt(i - 1);
        uint64_t ns = m.guestNs + (index - m.committed) * 1000000000ull / frameRate;
        if (i < markCount && ns > MarkAt(i).guestNs) ns = MarkAt(i).guestNs;
        return ns;
    }

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
                                     size_t count, uint64_t generation, AudioQueueSpan* span) {
    AUDIO_QUEUE_LOCK;
    if (span) *span = AudioQueueSpan{};
    if (audioQueue->generation != generation) return 0;
    const uint64_t head = audioQueue->accounting.committed - audioQueue->pending;
    size_t actualCount = 0;
    while (audioQueue->pending > 0 && actualCount < count)
        destination[actualCount++] = audioQueue->Pop();
    audioQueue->accounting.consumed += actualCount;
    if (span && audioQueue->markCount) {
        span->known = true;
        span->startNs = audioQueue->GuestNsOf(head);
        span->endNs = audioQueue->GuestNsOf(head + actualCount);
        span->frames = actualCount;
    }
    return actualCount;
}

void audioQueueMark(struct AudioQueue* audioQueue, uint64_t guestNs) {
    AUDIO_QUEUE_LOCK;
    const uint64_t committed = audioQueue->accounting.committed;
    if (audioQueue->markCount) {
        auto& last = audioQueue->marks[(audioQueue->markFirst + audioQueue->markCount - 1) % AudioQueue::markSlots];
        // No frame since: the newer time says more about the frames still to come.
        if (last.committed == committed) {
            last.guestNs = guestNs;
            return;
        }
        if (guestNs < last.guestNs + AudioQueue::markSpacingNs && guestNs >= last.guestNs) return;
    }
    if (audioQueue->markCount == AudioQueue::markSlots) {
        audioQueue->markFirst = (audioQueue->markFirst + 1) % AudioQueue::markSlots;
        --audioQueue->markCount;
    }
    audioQueue->marks[(audioQueue->markFirst + audioQueue->markCount++) % AudioQueue::markSlots] = {committed, guestNs};
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
