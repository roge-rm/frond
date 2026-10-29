/* Original, position-independent ARM callback. No OS calls from the worker.
 * Shared words and output are little endian; the 68k controller converts them.
 * Event log: sequence, capacity bytes, actual bytes, seed, checksum, channel.
 * Extended callback sizes are bytes, basic callback counts are stereo frames. */
typedef unsigned long U32;
typedef struct {
    U32 extended, seed, calls, overflow;
    U32 events[256][6];
} AudioState;
int audioCallback(AudioState* state, U32 channel, short* output, U32 argument) {
    U32 sequence = state->calls, capacity, actual, i, sum = 0;
    U32* size = (U32*)argument;
    capacity = state->extended ? *size : argument * 4;
    actual = capacity;
    /* Full, half, zero, full: zeros test the API, not an end-of-stream claim. */
    if (state->extended && (sequence & 3) == 1) actual = (capacity / 2) & ~3UL;
    if (state->extended && (sequence & 3) == 2) actual = 0;
    for (i = 0; i < actual / 4; ++i) {
        short sample = (short)(((state->seed * 97 + sequence * 131 + i * 17) & 4095) - 2048);
        *output++ = sample;
        *output++ = -sample;
        sum += (U32)(unsigned short)sample;
        sum += (U32)(unsigned short)-sample;
    }
    if (state->extended) *size = actual;
    if (sequence < 256) {
        U32* event = state->events[sequence];
        event[0] = sequence; event[1] = capacity; event[2] = actual;
        event[3] = state->seed; event[4] = sum; event[5] = channel;
    } else state->overflow = 1;
    state->calls = sequence + 1; /* Commit the event after all other stores. */
    return 0;
}
