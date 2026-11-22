#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>

#include "MediaQUsbDevice.h"

int main() {
    bool irq = false;
    MediaQUsbDevice controller([&](bool asserted) { irq = asserted; });
    auto port = controller.Port();
    const auto usb = MediaQUsbDevice::registers;
    const auto ram = MediaQUsbDevice::packetRam;
    const auto write = [&](unsigned offset, uint16_t value) { controller.Write(usb + offset, 2, value, false); };
    const auto read = [&](unsigned offset) { return controller.Read(usb + offset, 2, false); };

    controller.SetCable(true);
    write(2, 0x0936);
    assert(!port.Listening());
    write(2, 0x08c4);
    assert(port.Listening() && port.EnableCount() == 1);
    assert(port.Reset() && read(0x40) == 0x800 && controller.InterruptStatus() == 1);
    assert(irq);
    write(0x40, 0x800);
    assert(!irq);
    const uint8_t setup[]{0, 5, 9, 0, 0, 0, 0, 0};
    assert(port.Setup(setup) == UsbPacketResult::Ack);
    assert(read(0x18) == 0x4a0);
    assert(read(0x1c) == 0x0500 && read(0x1e) == 9);
    write(0x18, 0xff);
    write(0x40, 4);
    write(4, 9);
    write(0x0c, 0x0a);
    assert(port.Address() == 0);
    std::array<uint8_t, 64> buffer{};
    unsigned size = 0;
    assert(port.In(buffer.data(), &size) == UsbPacketResult::Ack && size == 0);
    assert(port.Address() == 9 && read(0x40) == 2);

    write(0x30, 1);
    write(0x38, 1);
    controller.Write(ram, 2, 0x1234, false);
    write(0x66, 1);
    write(0x60, 0x101);
    assert(port.PeekIn(2, &size) == UsbPacketResult::Ack && size == 2);
    assert(port.EpIn(2, buffer.data(), &size) == UsbPacketResult::Ack && size == 2);
    assert(buffer[0] == 0x34 && buffer[1] == 0x12 && read(0x60) == 0 && (read(0x40) & 0x40));

    const uint8_t data[]{0x67, 0x89};
    for (unsigned i = 0; i < 2; ++i) assert(port.EpOut(3, data, sizeof data) == UsbPacketResult::Ack);
    assert(port.EpOut(3, data, sizeof data) == UsbPacketResult::Nak);
    assert(read(0x6c) == 0x300 && read(0x78) == 2 && read(0x7c) == 2);
    assert(controller.Read(ram + 0x2000, 2, false) == 0x8967);
    assert(controller.Read(ram + 0x3000, 2, true) == 0x6789);
    write(0x6c, 0x107);
    assert(read(0x6c) == 0x200);

    write(2, 0x0936);
    assert(!port.Listening() && read(0x6c) == 0 && controller.InterruptStatus() == 0 && !irq);
    controller.SetCable(false);
    std::puts("MediaQ USB control, packet RAM and bulk endpoint checks passed");
}
