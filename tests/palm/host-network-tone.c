/* Position-independent ARM SndStream callback. State and PCM use ARM byte order. */
typedef unsigned long U32;
typedef struct {
    U32 phase, calls;
} Tone;
int hostTone(Tone* tone, U32 channel, short* output, U32 frames) {
    U32 phase = tone->phase;
    while (frames--) {
        short sample = (phase & 0x80000000UL) ? 2000 : -2000;
        phase += 85704562UL;
        *output++ = sample;
        *output++ = sample;
    }
    tone->phase = phase;
    ++tone->calls;
    return 0;
}
