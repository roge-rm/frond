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

// Guest time marks: the producer notes the guest time now and then (audioQueueMark), so a consumer can tell the
// guest time of the frames it pops. A mark (frames committed, guest ns) says that many frames were in by then;
// frames in between are taken as 44100 a second.
// span: the guest ns of the first frame popped (startNs) and of the next one still to come (endNs), and how many
// were popped. known is false until there is a mark, and after a clear until the next one.
struct AudioQueueSpan {
    bool known = false;
    uint64_t startNs = 0, endNs = 0;
    size_t frames = 0;
};
void audioQueueMark(struct AudioQueue* audioQueue, uint64_t guestNs);

struct AudioQueueState {
    size_t pendingSamples;
    uint64_t generation;
};

// A clear marks a discontinuity; consumers must re-prime for the new stream.
AudioQueueState audioQueueGetState(struct AudioQueue* audioQueue);
// span, when given, gets the popped frames' guest times; a count of 0 gives the guest time of the next frame.
size_t audioQueuePopChunkIfGeneration(struct AudioQueue* audioQueue, uint32_t* destination,
                                     size_t count, uint64_t generation, AudioQueueSpan* span = nullptr);
void audioQueueClear(struct AudioQueue* audioQueue);

#endif  //  _AUDIO_QUEUE_H_
