#ifndef _AUDIO_INPUT_H_
#define _AUDIO_INPUT_H_

#include <cstdint>

// Host sound input for a codec ADC (the AK4534 on the TG50, the WM9712L on the Zire 72).
// All calls run on the emulator thread. Pull one mono frame at the requested guest rate;
// zero on underrun, never block. Units: signed16, 32768 is the codec's ADC full scale
// before its analogue gain.
// A capture driver owns resampling and a bounded queue (at most 250ms), drops
// oldest on overflow and counts drops. Fence freezes consumption/publication;
// disarm closes the source and invalidates queued callback generations.
// Host callbacks and queues are never portable state. A fresh load stays disarmed.
enum class AudioInputRoute { internalMic, externalMic, analogIn };
struct AudioInputSource {
    void *context{};
    int16_t (*pull)(void *, AudioInputRoute, uint32_t rateNumerator, uint32_t rateDenominator){};
    void (*fence)(void *, bool){};
    void (*disarm)(void *){};
};
// Guest analogue power/clock readiness, independent of host arm/fence state.
// Poll on the emulator thread; the callback must never open a host device.
struct AudioInputDemand {
    bool active{}, fenced{};
    AudioInputRoute route{};
    uint32_t rateNumerator{}, rateDenominator{};
};

#endif
