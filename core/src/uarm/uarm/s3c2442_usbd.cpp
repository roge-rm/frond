#include "s3c2442_blocks.h"

namespace {
    constexpr uint32_t first = 0x140, last = 0x1fc;
    constexpr uint32_t epIntReg = 0x148, usbIntReg = 0x158;
    // EP0 to EP4 FIFOs, read as empty.
    bool fifo(uint32_t offset) { return offset >= 0x1c0 && offset <= 0x1d0; }
}

void S3cUsbDevice::reset() {
    for (auto& r : regs) r = 0;
    regs[(0x144 - first) / 4] = 0x08;  // PWR_REG: no suspend, MCU resume clear, ISO update clear
    regs[(0x15c - first) / 4] = 0x1f;  // EP_INT_EN_REG
    regs[(0x16c - first) / 4] = 0x04;  // USB_INT_EN_REG: reset interrupt on
}

S3cAccess S3cUsbDevice::read(uint32_t offset, uint32_t& value) {
    if (offset & 3 || offset < first || offset > last) return S3cAccess::unknown;
    value = fifo(offset) ? 0 : regs[(offset - first) / 4];
    return S3cAccess::ok;
}

S3cAccess S3cUsbDevice::write(uint32_t offset, uint32_t value) {
    if (offset & 3 || offset < first || offset > last) return S3cAccess::unknown;
    auto& r = regs[(offset - first) / 4];
    if (offset == epIntReg || offset == usbIntReg) r &= ~value;
    else if (!fifo(offset)) r = value & 0xff;
    return S3cAccess::ok;
}
