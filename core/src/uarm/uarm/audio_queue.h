#ifndef _AUDIO_QUEUE_H_
#define _AUDIO_QUEUE_H_

#include <cstddef>
#include <cstdint>

struct AudioQueue;

struct AudioQueue* audioQueueCreate(size_t capacity);
void audioQueueEnsureCapacity(struct AudioQueue* audioQueue, size_t capacity);

// A full queue drops its oldest frame. One emulator-thread producer.
// Queues outlive attached SoCs.
// nonSilent counts committed frames that differ from the frame pushed before them.
struct AudioQueueAccounting { uint64_t committed, consumed, discarded, nonSilent; };
AudioQueueAccounting audioQueueAccounting(struct AudioQueue*);

void audioQueuePush(struct AudioQueue* audioQueue, uint32_t sample);

size_t audioQueuePopChunk(struct AudioQueue* audioQueue, uint32_t* destination, size_t count);

size_t audioQueuePendingSamples(struct AudioQueue* audioQueue);

struct AudioQueueState {
    size_t pendingSamples;
    uint64_t generation;
};

// A clear marks a discontinuity; consumers must re-prime for the new stream.
AudioQueueState audioQueueGetState(struct AudioQueue* audioQueue);
size_t audioQueuePopChunkIfGeneration(struct AudioQueue* audioQueue, uint32_t* destination,
                                     size_t count, uint64_t generation);
void audioQueueClear(struct AudioQueue* audioQueue);

#endif  //  _AUDIO_QUEUE_H_
