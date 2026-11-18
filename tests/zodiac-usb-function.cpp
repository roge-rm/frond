// The Zodiac's W4200 USB function (Imx1GraphicsUsb) driven the way the ROM's USB driver drives it (register
// facts from REA of the 1.1 ROM), against host packets: enable, bus reset, a control read and write, bulk IN
// through both buffers and bulk OUT with a short packet. No firmware.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "imx1_graphics_usb.h"

namespace {
using A = Imx1BootstrapAccess;
using R = UsbPacketResult;
constexpr uint32_t pc = 0x20177e40;  // inside the driver

uint32_t rd(Imx1GraphicsUsb& usb, uint32_t offset) {
    uint32_t value = 0;
    assert(usb.access(pc, 0x16200000 + offset, 4, false, value) == A::ok);
    return value;
}
void wr(Imx1GraphicsUsb& usb, uint32_t offset, uint32_t value) {
    assert(usb.access(pc, 0x16200000 + offset, 4, true, value) == A::ok);
}
void chip(Imx1GraphicsUsb& usb, uint32_t address, uint32_t value) {
    assert(usb.chipAccess(pc, address, true, value, 0x4b000200) == A::ok);
}

// The driver's start (20177e34): a8 pulse, b30, b1c, b10, soft reset, 1210, 1200, endpoints, events on.
void enable(Imx1GraphicsUsb& usb) {
    chip(usb, 0x160100a8, 0x4b000203);
    chip(usb, 0x160100a8, 0x4b000200);
    chip(usb, 0x16010b30, 0x401);
    chip(usb, 0x16010b1c, 0x5b4);
    chip(usb, 0x16010b10, 2);
    wr(usb, 0x1304, 1);
    assert(!(rd(usb, 0x1304) & 1));
    wr(usb, 0x1210, 2);
    rd(usb, 0x13e0);
    wr(usb, 0x1200, 0x10001);
    wr(usb, 0x1230, 0);
    const uint32_t config[] = {0x1009, 0x00160006, 0x10000000, 0, 0x1001, 0x00360026, 0xffff, 0,
                               0x1009, 0x00560046, 0x10000000, 0, 0x1001, 0x00760066, 0xffff, 0};
    for (unsigned i = 0; i < 16; ++i) if (i % 4 != 3) wr(usb, 0x1310 + 4 * i, config[i]);
    assert(!usb.listening());
    wr(usb, 0x13b4, 0xf01e);
    wr(usb, 0x13c4, 1);
    wr(usb, 0x1238, 1);
    assert(usb.listening() && usb.enableCount() == 1 && !usb.line());
}

void control() {
    Imx1GraphicsUsb usb;
    assert(usb.setup((const uint8_t*)"\x80\x06\0\1\0\0\x12\0") == R::Timeout);
    enable(usb);
    assert(usb.busReset());
    assert(usb.line() && rd(usb, 0x1234) == 1 && (rd(usb, 0x13c0) & 1));
    wr(usb, 0x13c8, 1);
    assert(!usb.line());

    // GET_DESCRIPTOR (device), 18 bytes: setup, then three IN packets and the OUT status stage.
    const uint8_t get[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};
    assert(usb.setup(get) == R::Ack);
    assert(usb.line() && (rd(usb, 0x13b0) & 0x4000) && (rd(usb, 0x1308) & 0x40000));
    wr(usb, 0x13b8, 0x4000);
    assert(rd(usb, 0x16200000 - 0x16200000) == 0x01000680 && rd(usb, 4) == 0x00120000);
    wr(usb, 0x1308, 0x40000);
    assert(!(rd(usb, 0x1308) & 0x40000) && !usb.line());
    uint8_t data[64];
    unsigned size = 99;
    assert(usb.ep0In(data, &size, true) == R::Nak);
    for (unsigned sent = 0, part = 0; sent < 18; sent += part) {
        part = std::min(18u - sent, 8u);
        wr(usb, 0x0010, 0x03020100 + sent * 0x01010101);
        wr(usb, 0x0014, 0x07060504 + sent * 0x01010101);
        wr(usb, 0x130c, part << 6);
        wr(usb, 0x1308, 0x10000);
        if (sent + part == 18) wr(usb, 0x1308, 0x20);
        // The status stage waits for the driver's 0x20.
        if (sent + part < 18) assert(usb.ep0Out(nullptr, 0) == R::Nak);
        assert(usb.ep0In(nullptr, &size, false) == R::Ack && size == part);
        assert(usb.ep0In(data, &size, true) == R::Ack && size == part && data[0] == sent && data[part - 1] == sent + part - 1);
        assert(rd(usb, 0x13b0) & 0x1000);
        wr(usb, 0x13b8, 0x1000);
    }
    assert(usb.ep0Out(nullptr, 0) == R::Ack);
    assert(usb.ep0Out(nullptr, 0) == R::Nak);

    // SET_ADDRESS: no data, an empty IN packet for the status stage.
    const uint8_t address[8] = {0, 5, 9, 0, 0, 0, 0, 0};
    assert(usb.setup(address) == R::Ack);
    wr(usb, 0x13b8, 0x4000);
    wr(usb, 0x1308, 0x40000);
    wr(usb, 0x1300, 9);
    wr(usb, 0x130c, rd(usb, 0x130c) & ~0x3c0u);
    wr(usb, 0x1308, 0x10000);
    wr(usb, 0x1308, 0x20);
    assert(usb.ep0In(data, &size, true) == R::Ack && size == 0 && usb.address() == 9);
    wr(usb, 0x13b8, 0x1000);
    assert(usb.ep0In(data, &size, true) == R::Nak);

    // A vendor control write with 3 bytes: OUT data, then the empty IN status packet once the driver says so.
    const uint8_t vendor[8] = {0x40, 1, 0, 0, 0, 0, 3, 0};
    assert(usb.setup(vendor) == R::Ack);
    wr(usb, 0x13b8, 0x4000);
    wr(usb, 0x1308, 0x40000);
    assert(usb.ep0Out((const uint8_t*)"abc", 3) == R::Ack);
    assert(usb.ep0Out((const uint8_t*)"d", 1) == R::Nak);
    assert((rd(usb, 0x13b0) & 0x2000) && (rd(usb, 0x1308) & 0x20000) && (rd(usb, 0x130c) & 15) == 3);
    assert((rd(usb, 8) & 0xffffff) == 0x636261);
    assert(usb.ep0In(data, &size, true) == R::Nak);
    wr(usb, 0x13b8, 0x2000);
    wr(usb, 0x1308, 0x20);
    wr(usb, 0x1308, 0x20000);
    assert(usb.ep0In(data, &size, true) == R::Ack && size == 0);

    // A stall holds until the next setup.
    assert(usb.setup(vendor) == R::Ack);
    wr(usb, 0x1308, 0x10);
    assert(usb.ep0Out((const uint8_t*)"a", 1) == R::Stall && usb.ep0In(data, &size, true) == R::Stall);
    assert(usb.setup(get) == R::Ack && usb.ep0In(data, &size, true) == R::Nak);

    // Writes the driver never makes are refused.
    uint32_t bad = 0x80;
    assert(usb.access(pc, 0x16201308, 4, true, bad) == A::sideEffect);
    assert(usb.access(0x20000000, 0x16201308, 4, false, bad) == A::externalInput);
    assert(usb.access(pc, 0x16201308, 2, false, bad) == A::width);
    std::puts("PASS zodiac usb control");
}

void bulk() {
    Imx1GraphicsUsb usb;
    enable(usb);
    assert(usb.busReset());
    wr(usb, 0x13c8, 1);
    uint8_t data[64];
    unsigned size = 0;
    assert(usb.endpointIn(1, data, &size, true) == R::Nak);
    assert(usb.endpointIn(2, data, &size, true) == R::Unsupported);

    // EP1 IN, 100 bytes: 2 packets of 64 set in 1318, buffer A with 64, then B with the last 36.
    wr(usb, 0x1318, (rd(usb, 0x1318) & 0x3f0000) | 2 | (64u << 22));
    for (unsigned i = 0; i < 16; ++i) wr(usb, 4 * (6 + i), 0x01010101 * i);
    wr(usb, 0x13a4, 2);
    assert(usb.endpointIn(1, nullptr, &size, false) == R::Ack && size == 64);
    assert(usb.endpointIn(1, data, &size, true) == R::Ack && size == 64 && data[63] == 15);
    assert((rd(usb, 0x13b0) & 2) && usb.line() && (rd(usb, 0x1318) & 0xffff) == 1);
    wr(usb, 0x13b8, 2);
    wr(usb, 0x1318, (((rd(usb, 0x1318) >> 16 << 16) + 1) & 0x3fffff) | (36u << 22));
    for (unsigned i = 0; i < 9; ++i) wr(usb, 4 * (0x16 + i), 0x40404040 + i);
    wr(usb, 0x13a4, 0x20000);
    assert(rd(usb, 0x13a0) & 0x20000);
    assert(usb.endpointIn(1, data, &size, true) == R::Ack && size == 36 && data[0] == 0x40 && data[32] == 0x48);
    assert((rd(usb, 0x1318) & 0xffff) == 0 && !(rd(usb, 0x13a0) & 0x20000));
    wr(usb, 0x13b8, 2);
    assert(!usb.line());

    // EP2 OUT: a full packet in buffer A, then a short one in B; one packet at a time.
    uint8_t out[64];
    for (unsigned i = 0; i < 64; ++i) out[i] = uint8_t(i + 1);
    assert(usb.endpointOutReady(2));
    assert(usb.endpointOut(2, out, 64) == R::Ack);
    assert(!usb.endpointOutReady(2) && usb.endpointOut(2, out, 10) == R::Nak);
    assert((rd(usb, 0x13a0) & 4) && (rd(usb, 0x13b0) & 4) && !(rd(usb, 0x13b0) & 0x40000));
    assert(rd(usb, 4 * 0x26) == 0x04030201 && rd(usb, 4 * 0x26 + 60) == 0x403f3e3d);
    wr(usb, 0x13b8, 4);
    wr(usb, 0x13a8, 4);
    assert(usb.endpointOut(2, out, 10) == R::Ack);
    assert((rd(usb, 0x13a0) & 0x40000) && (rd(usb, 0x13b0) & 0x40004) == 0x40004 && (rd(usb, 0x1328) >> 22) == 10);
    assert(rd(usb, 4 * 0x36) == 0x04030201);
    wr(usb, 0x13b8, 4);
    wr(usb, 0x13a8, 0x40000);
    wr(usb, 0x13b8, 0x40000);
    // Arming again starts at buffer A.
    wr(usb, 0x1328, 0xffff);
    assert(usb.endpointOut(2, out, 5) == R::Ack && (rd(usb, 0x13a0) & 4));
    assert(usb.endpointOut(4, out, 65) == R::Unsupported);

    // A bus reset frees every buffer.
    assert(usb.busReset() && !(rd(usb, 0x13a0) & 0x50005) && usb.address() == 0);
    std::puts("PASS zodiac usb bulk");
}

void displayOff() {
    Imx1GraphicsUsb usb;
    assert(usb.displayRead() == 0 && usb.displayWrite(0) && !usb.displayWrite(1));
    enable(usb);
    assert(usb.displayRead() == 0x401 && usb.displayWrite(0x400) && !usb.listening());
    std::puts("PASS zodiac usb display off");
}
}  // namespace

int main() {
    control();
    bulk();
    displayOff();
    return 0;
}
