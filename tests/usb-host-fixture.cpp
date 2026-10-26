#include <cassert>
#include <cstdio>
#include <cstdlib>
#include "usb-host-fixture.h"
static ArmMemAccessF mmio;
static bool irq;
extern "C" void uarmAbort() { std::abort(); }
bool memRegionAdd(ArmMem*, uint32_t, uint32_t, ArmMemAccessF f, void*) { mmio = f; return true; }
void pxaIcInt(PxaIc*, uint_fast8_t n, bool level) { assert(n == PXA_I_USB); irq = level; }
int main() {
    auto* udc = pxa270UdcInit(nullptr, nullptr, nullptr);
    UsbHostFixture host{udc};
    auto write = [&](unsigned offset, uint32_t value) { assert(mmio(udc, 0x40600000 + offset, 4, true, &value)); };
    auto read = [&](unsigned offset) { uint32_t value = 0; assert(mmio(udc, 0x40600000 + offset, 4, false, &value)); return value; };
    UsbHostFixture::Packet packet;
    assert(!host.resetStimulus() && !host.descriptorSetup(18));
    write(0, 1); write(8, 1u << 27);
    assert(host.resetStimulus() && irq && read(0x10) == (1u << 27));
    write(0x10, 1u << 27); assert(!irq);
    // The fixture respects masking; enabling later cannot invent an old event.
    assert(host.descriptorSetup(18) && !irq && read(0x100) == 0xc1);
    write(4, 1); assert(!irq);
    assert(read(0x300) == 0x01000680 && read(0x300) == 0x00120000);
    write(0x100, 0x81);
    assert(host.descriptorSetup(18) && irq);
    Savestate<ChunkType> saved; assert(saved.Save(host));
    read(0x300); write(0x100, 0x81); write(0x0c, 1);
    SavestateLoader<ChunkType> loader;
    assert(loader.Load(saved.GetBuffer(), saved.GetSize(), host) && irq);
    assert(read(0x200) == 8 && read(0x300) == 0x01000680 && read(0x300) == 0x00120000);
    write(0x100, 0x81); write(0x0c, 1); assert(!irq);
    // A synthetic guest with separate setup acknowledgement and IN completion
    // can return a full packet, a two-byte tail, and accept the status OUT.
    for (unsigned i = 0; i < 4; ++i) write(0x300, 0x03020100 + i * 0x04040404);
    assert(host.in(packet) == UsbHostFixture::Result::Data && packet.size == 16 && irq);
    for (unsigned i = 0; i < 16; ++i) assert(packet.bytes[i] == i);
    write(0x0c, 1);
    uint16_t tail = 0x1110; assert(mmio(udc, 0x40600300, 2, true, &tail));
    write(0x100, 2);
    assert(saved.Save(host)); write(0x100, 4);
    assert(host.in(packet) == UsbHostFixture::Result::Nak);
    assert(loader.Load(saved.GetBuffer(), saved.GetSize(), host));
    assert(host.in(packet) == UsbHostFixture::Result::Data && packet.size == 2 && packet.bytes[1] == 17);
    write(0x0c, 1); assert(host.statusOut() && irq && read(0x100) == 1 && read(0x200) == 0);
    write(0x100, 1); write(0x0c, 1);
    // Reproduce the observed problematic ordering without any firmware bytes:
    // setup event -> guest queues IN -> guest IRQ acknowledgement flushes IN.
    assert(host.descriptorSetup(18)); read(0x300); read(0x300); write(0x100, 0x81);
    for (unsigned i = 0; i < 4; ++i) write(0x300, i);
    write(0x0c, 1); write(0x100, 4);
    assert(host.in(packet) == UsbHostFixture::Result::Nak && !irq && udc->tx0Count == 0);
    // Sampling IN earlier cannot fix an ACK cleared by the outstanding setup ISR.
    assert(host.descriptorSetup(18)); read(0x300); read(0x300); write(0x100, 0x81);
    for (unsigned i = 0; i < 4; ++i) write(0x300, i);
    assert(host.in(packet) == UsbHostFixture::Result::Data && irq);
    write(0x0c, 1); write(0x100, 4);
    assert(!irq && host.in(packet) == UsbHostFixture::Result::Nak);
    assert(host.resetStimulus() && !(read(0x100) | read(0x200)));
    write(0, 0); assert(host.in(packet) == UsbHostFixture::Result::Unsupported);
    free(udc);
    puts("PASS: synthetic-guest EP0 stimuli, masks, 16+2/status exchange, state restore and reset");
    puts("PASS: FTF discards queued data; W1C clears overlapping events without fabricating completion");
}
