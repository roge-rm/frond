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
    using R = UsbHostFixture::Result;
    auto write = [&](unsigned offset, uint32_t value) { assert(mmio(udc, 0x40600000 + offset, 4, true, &value)); };
    auto read = [&](unsigned offset) { uint32_t value = 0; assert(mmio(udc, 0x40600000 + offset, 4, false, &value)); return value; };
    UsbHostFixture::Packet packet;
    assert(!host.resetStimulus() && !host.descriptorSetup(18));
    write(0, 1); write(8, 1u << 27);
    assert(host.resetStimulus() && irq && read(0x10) == (1u << 27));
    write(0x10, 1u << 27); assert(!irq);
    // Masked events are not raised later when the mask is enabled.
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
    puts("PASS: EP0 SETUP through the model, masks and SETUP restore");

    // Test 7: the XLLP ROM sequence without firmware bytes. The ROM acknowledges
    // each EP0 event with UDCISR0 = 1 and then UDCCSR0 = FTF after loading the next
    // packet; the host IN comes only after the guest has done both.
    assert(host.descriptorSetup(18) && irq);
    uint8_t request[8];
    for (unsigned i = 0; i < 2; ++i) { uint32_t w = read(0x300); std::memcpy(request + 4 * i, &w, 4); }
    assert(request[1] == 6 && request[6] == 18 && !(read(0x100) & 0x40));
    write(0x100, 0x81);
    for (unsigned i = 0; i < 4; ++i) write(0x300, 0x03020100 + i * 0x04040404);
    write(0x0c, 1); write(0x100, 4);
    assert(!irq && read(0x100) == 2 && udc->tx0Count == 16);
    assert(host.in(packet) == R::Data && packet.size == 16 && irq);
    for (unsigned i = 0; i < 16; ++i) assert(packet.bytes[i] == i);
    uint16_t tail = 0x1110; assert(mmio(udc, 0x40600300, 2, true, &tail));
    write(0x100, 2);
    write(0x0c, 1); write(0x100, 4); assert(!irq);
    // Test 8: a ready packet survives save/restore, and a flush after save is undone.
    assert(saved.Save(host));
    assert(host.statusOut() && irq && !udc->tx0Count); write(0x100, 1); write(0x0c, 1);
    assert(loader.Load(saved.GetBuffer(), saved.GetSize(), host) && !irq);
    assert(host.in(packet) == R::Data && packet.size == 2 && packet.bytes[0] == 16 && packet.bytes[1] == 17);
    write(0x0c, 1); write(0x100, 4); // the ROM's ack of the last IN, on an empty FIFO
    assert(host.in(packet) == R::Nak);
    assert(host.statusOut() && irq && read(0x100) == 1 && read(0x200) == 0 && !(read(0) & 2));
    write(0x100, 0x81); write(0x0c, 1); assert(!irq && !read(0x100));
    puts("PASS: XLLP replay: SETUP, 16 + 2 bytes across FTF acks, status OUT, ready packet restore");

    // Test 8: an event raised between the guest's ISR read and its W1C is lost; that
    // is documented W1C behaviour, which is why the host must not ACK an IN inside the
    // guest's handler.
    assert(host.descriptorSetup(18)); read(0x300); read(0x300); write(0x100, 0x81);
    for (unsigned i = 0; i < 4; ++i) write(0x300, i);
    assert(host.in(packet) == R::Data && irq); // too early on a real bus
    write(0x0c, 1); write(0x100, 4);
    assert(!irq && host.in(packet) == R::Nak);
    // A guest that loads data while the IN packet is ready only loses those bytes.
    uint32_t extra = 0xabcd;
    for (unsigned i = 0; i < 4; ++i) write(0x300, i);
    assert(mmio(udc, 0x40600300, 4, true, &extra) && udc->tx0Count == 16 && udc->tx0[0] == 0);
    write(0x0c, 1);
    assert(host.resetStimulus() && !(read(0x100) | read(0x200)) && host.in(packet) == R::Nak);
    // The status OUT of a control write: the guest arms a ZLP, the host reads it.
    const uint8_t setOut[8] = {0x40, 1, 0, 0, 0, 0, 2, 0};
    write(0x10, 1u << 27);
    assert(host.setup(setOut)); read(0x300); read(0x300); write(0x100, 0x81); write(0x0c, 1);
    const uint8_t data[2] = {0xa5, 0x5a};
    assert(read(0) & 2); // UDA from SETUP to the end of the status stage
    assert(host.out(data, 2) == R::Data && read(0x100) == 0x41 && read(0x200) == 2 && read(0x300) == 0x5aa5);
    assert(host.out(data, 2) == R::Nak);
    write(0x100, 1); write(0x0c, 1);
    assert(host.statusOut() && (read(0) & 2)); // a zero-length OUT data packet is not the status
    write(0x100, 1); write(0x100, 2); write(0x0c, 1); write(0x100, 4);
    assert(host.in(packet) == R::Data && packet.size == 0 && irq && !(read(0) & 2)); write(0x0c, 1);
    // A control read keeps UDA across a ZLP IN that ends its data stage.
    assert(host.descriptorSetup(32)); read(0x300); read(0x300); write(0x100, 0x81); write(0x0c, 1);
    write(0x100, 2); assert(host.in(packet) == R::Data && packet.size == 0 && (read(0) & 2));
    write(0x0c, 1); assert(host.statusOut() && !(read(0) & 2)); write(0x100, 1); write(0x0c, 1);
    // A STALL ends the transfer.
    assert(host.setup(setOut) && (read(0) & 2)); read(0x300); read(0x300); write(0x100, 0xa1); write(0x0c, 1);
    assert(host.in(packet) == R::Stall && !(read(0) & 2));
    assert(UsbHostFixture::inTransactionNs(16) == 18167 && UsbHostFixture::inTransactionNs(0) == 7500);
    write(0, 0); assert(host.in(packet) == R::Unsupported && !host.statusOut());
    free(udc);
    puts("PASS: overlapping W1C loses an early IN event; writes to a ready packet are dropped; reset; control-write ZLP status");
}
