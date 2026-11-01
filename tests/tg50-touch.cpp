// AD7873 command pipeline and pen IRQ, without a guest or firmware.
#include <cassert>
#include <cstdio>
#include "sspdev_AD7873.h"
#include "savestate/savestateAll.h"

static SspClientProcF transfer;
static void *client;
static bool penIrqHigh;
bool pxaSspAddClient(PxaSsp *, SspClientProcF callback, void *context) {
    transfer = callback;
    client = context;
    return true;
}
void pxaGpioSetState(PxaGpio *, uint_fast8_t pin, bool state) {
    assert(pin == 9);
    penIrqHigh = state;
}
struct State {
    Ad7873 *touch;
    template <typename T> void Save(T &target) { ad7873Save(touch, target); }
    void Load(SavestateLoader<ChunkType> &source) { assert(ad7873Load(touch, source, false)); }
};
int main() {
    auto *touch = ad7873Init(nullptr, nullptr, 9);
    assert(penIrqHigh);
    ad7873setVbatt(touch, 4200);
    ad7873setVaux(touch, 1250);
    assert(transfer(client, 12, 0xa6) == 0);
    assert(transfer(client, 12, 0xe6) == 1720);
    assert(transfer(client, 12, 0xe7) == 2048);
    ad7873PenInput(touch, 1234, 2345);
    ad7873Periodic(touch);
    assert(!penIrqHigh);
    assert(transfer(client, 12, 0x93) == 2048);
    assert(transfer(client, 12, 0xd0) == 2345);
    assert(transfer(client, 12, 0xb8) == 1234);
    assert(transfer(client, 12, 0xa6) == 0x7ff);
    assert(transfer(client, 8, 0xa6) == 0);
    assert(transfer(client, 12, 0xa6) == 1720);
    auto input = ad7873CaptureInput(touch);
    ad7873PenInput(touch, -1, -1);
    ad7873Periodic(touch);
    assert(penIrqHigh);
    input();
    ad7873Periodic(touch);
    assert(!penIrqHigh);
    State state{touch};
    Savestate<ChunkType> saved;
    assert(saved.Save(state));
    ad7873setVbatt(touch, 1000);
    SavestateLoader<ChunkType> loader;
    assert(loader.Load(saved.GetBuffer(), saved.GetSize(), state));
    ad7873Periodic(touch);
    assert(penIrqHigh);
    transfer(client, 12, 0xa6);
    assert(transfer(client, 12, 0xb8) == 1720);
    assert(transfer(client, 12, 0x93) == 0);
    ad7873setVaux(touch,2000);
    ad7873Reset(touch);
    assert(penIrqHigh && ad7873getVaux(touch)==3276);
    assert(transfer(client,12,0xe6)==0);
    assert(transfer(client,12,0xa6)==3276);
    assert(transfer(client,12,0xe6)==1720);
    ad7873Destroy(touch);
    puts("TG50 AD7873 pipeline, pen IRQ and state passed");
}
