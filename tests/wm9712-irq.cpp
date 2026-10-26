// Exercise the real codec model through its AC97 register callbacks.
#include <cstdio>
#include <cstdlib>

#include "ac97dev_WM9712L.h"
#include "audio_queue.h"

static Ac97CodecRegR readRegister;
static Ac97CodecRegW writeRegister;
static void* codec;
static bool levels[128]{};
static uint32_t samples[16]{};
static unsigned sampleCount = 0;

static void check(bool success) {
    if (!success) {
        std::fputs("FAIL: WM9712 IRQ regression\n", stderr);
        std::abort();
    }
}

void pxaAC97clientAdd(PxaAC97*, Ac97Codec which, Ac97CodecRegR read,
                     Ac97CodecRegW write, void* context) {
    if (which == Ac97PrimaryAudio) {
        readRegister = read;
        writeRegister = write;
        codec = context;
    }
}
bool pxaAC97clientClientWantData(PxaAC97*, Ac97Codec, uint32_t*) { return false; }
void pxaAC97clientClientHaveData(PxaAC97*, Ac97Codec which, uint32_t data) {
    if (which == Ac97PrimaryModem) {
        check(sampleCount < 16);
        samples[sampleCount++] = data;
    }
}
void audioQueuePush(AudioQueue*, uint32_t) {}
void pxaGpioSetState(PxaGpio*, uint_fast8_t pin, bool on) { levels[pin] = on; }
extern "C" void uarmAbort() { std::abort(); }

static void write(unsigned address, uint16_t value) {
    check(writeRegister(codec, address, value));
}
static uint16_t read(unsigned address) {
    uint16_t value = 0;
    check(readRegister(codec, address, &value));
    return value;
}

int main() {
    auto* tx = wm9712LInit(nullptr, nullptr, -1, 27);
    check(read(0x26) == 0x000f);
    write(0x26, 0x410f);  // ready bits are read-only; PR0 powers down the ADC
    check(read(0x26) == 0x410e);
    write(0x26, 0x0600);
    check(read(0x26) == 0x0605);
    write(0x26, 0x0800);
    check(read(0x26) == 0x080b);
    write(0x26, 0);
    write(0x4c, 0xf83a);  // GPIO2 is an output
    write(0x4e, 0xffff);  // active-high inputs
    write(0x50, 0x2000);  // pen detector is sticky
    write(0x54, 0xdfff);  // acknowledge pen status
    write(0x52, 0x2000);  // enable pen interrupt
    write(0x56, 0xf83a);  // GPIO2 drives IRQ
    check(!levels[27]);
    write(0x76, 0x3f18);  // continuous coordinate data in slot 5
    write(0x78, 0x4001);  // arm digitiser wake-up
    for (unsigned i = 0; i < 64; ++i) wm9712Lperiodic(tx);
    check(sampleCount == 0);  // powered-off digitiser must not fill the FIFO
    wm9712LsetPen(tx, 1000, 2000, 1000);
    check(read(0x78) == 0xc001);
    for (unsigned i = 0; i < 3; ++i) {
        wm9712Lperiodic(tx);
        check(read(0x7a) == samples[i]);  // slot mode must also update the result register
    }
    check(sampleCount == 3 && samples[0] == (0x9000 | 1000) &&
          samples[1] == (0xa000 | 2000) && samples[2] == (0xb000 | 1000));
    write(0x78, 0x8001);  // detector enabled, digitiser off, no auto wake
    for (unsigned i = 0; i < 64; ++i) wm9712Lperiodic(tx);
    check(sampleCount == 3);
    check(levels[27] && (read(0x54) & 0x2000));
    wm9712LsetPen(tx, -1, -1, 0);
    check(levels[27]);  // release does not discard an unacknowledged sticky event
    write(0x54, 0xdfff);
    check(!levels[27] && !(read(0x54) & 0x2000));
    wm9712LsetPen(tx, 1000, 2000, 1000);
    check(levels[27]);
    write(0x54, 0xdfff);
    wm9712LsetPen(tx, 1100, 2100, 1000);
    check(!levels[27]);  // motion does not re-latch the acknowledged down edge
    wm9712LsetPen(tx, -1, -1, 0);
    wm9712LsetPen(tx, 1200, 2200, 1000);
    check(levels[27]);
    write(0x58, 1);  // IRQ inversion
    check(!levels[27]);
    write(0x54, 0xdfff);
    check(levels[27]);
    write(0x52, 0);  // wake mask disables the event
    write(0x58, 0);
    check(!levels[27]);
    check(!writeRegister(codec, 0xffff, 0));

    auto* aux = wm9712LInit(nullptr, nullptr, -1);
    sampleCount = 0;
    wm9712LsetAuxVoltage(aux, WM9712LauxPinBmon, 1400);
    write(0x78, 0x4001);
    write(0x76, 0xef38);  // TX coordinate poll ending in a battery measurement
    wm9712Lperiodic(aux);
    check(read(0x76) & 0x8000);
    wm9712Lperiodic(aux);
    check(read(0x76) & 0x8000);
    wm9712Lperiodic(aux);
    check(!(read(0x76) & 0x8000));
    check(read(0x7a) == (0x6000 | ((1400 * 4095 + 1650) / 3300)));

    auto* e2 = wm9712LInit(nullptr, nullptr, 50);
    write(0x4c, 0xf836);
    write(0x56, 0xf836);  // GPIO3 drives direct PENDOWN
    wm9712LsetPen(e2, 1000, 2000, 1000);
    check(levels[50]);
    wm9712LsetPen(e2, -1, -1, 0);
    check(!levels[50]);
    std::puts("PASS: TX IRQ, digitiser wake-up, coordinate samples, and E2 direct pen output");
}
