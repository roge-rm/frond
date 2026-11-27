#include "pxa270_OHCI.h"
#include "mem.h"
#include "RAM.h"
#include "memory_buffer.h"
#include "soc_PXA.h"
#include "spitz_nand.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
constexpr uint32_t ram = 0xa0000000, usb = 0x4c000000;
constexpr uint32_t hcca = ram, ed = ram + 0x100, td = ram + 0x110, tail = ram + 0x120;
constexpr uint32_t data = ram + 0x200;
constexpr uint32_t CC = 0xf0000000;

struct Machine {
    ArmMem* mem = memInit();
    std::array<uint8_t, 0x5000> memory{};
    bool irq = false;
    unsigned kicks = 0;
    uint64_t now = 0;
    Pxa270Ohci ohci{mem, [this](bool level) { irq = level; }, [this] { ++kicks; }};

    uint32_t dirtyPages = 0;
    ArmRam* backing = nullptr;

    Machine() {
        MemoryBuffer buffer{};
        buffer.size = memory.size();
        buffer.buffer = memory.data();
        buffer.dirtyPagesSize = sizeof(dirtyPages);
        buffer.dirtyPages = &dirtyPages;
        backing = ramInit(mem, nullptr, ram, memory.size(), &buffer, true);
        ramSetNoFramebuffer(backing);
    }

    ~Machine() {
        memDeinit(mem);
        std::free(backing);
    }

    void write(uint32_t addr, uint32_t value) {
        assert(memAccess(mem, addr, 4, true, &value));
    }

    uint32_t read(uint32_t addr) {
        uint32_t value;
        assert(memAccess(mem, addr, 4, false, &value));
        return value;
    }

    void frame(unsigned count = 1) {
        for (unsigned i = 0; i < count; ++i) {
            now += 1000000;
            ohci.Tick(now);
        }
    }

    void transaction(unsigned address, unsigned endpoint, unsigned pid,
                     uint32_t start = 0, uint32_t end = 0, unsigned packet = 8) {
        write(usb + 0x0c, 2);
        write(hcca + 0x84, 0);
        write(ed, address | (endpoint << 7) | (packet << 16));
        write(ed + 4, tail);
        write(ed + 8, td);
        write(ed + 12, 0);
        write(td, CC | (pid << 19) | (1 << 18));
        write(td + 4, start);
        write(td + 8, tail);
        write(td + 12, end);
        write(usb + 8, endpoint ? 4 : 2);
        frame();
        assert(!(read(td) & CC));
        assert((read(ed + 8) & ~0xfu) == tail);
        assert((read(hcca + 0x84) & ~1u) == td && irq);
    }

    void setup(unsigned address, std::array<uint8_t, 8> bytes) {
        std::copy(bytes.begin(), bytes.end(), memory.begin() + (data - ram));
        transaction(address, 0, 0, data, data + 7);
    }

    void command(uint8_t opcode, uint32_t expected, bool input, unsigned count = 0) {
        auto* cbw = memory.data() + (data - ram);
        std::memset(cbw, 0, 31);
        std::memcpy(cbw, "USBC", 4);
        cbw[4] = 7;
        cbw[8] = expected;
        cbw[9] = expected >> 8;
        cbw[12] = input ? 0x80 : 0;
        cbw[14] = 10;
        cbw[15] = opcode;
        cbw[23] = count;
        transaction(2, 2, 1, data, data + 30, 64);
    }

    void status(uint8_t result) {
        transaction(2, 1, 2, data, data + 12, 64);
        const uint8_t* status = memory.data() + (data - ram);
        assert(!std::memcmp(status, "USBS", 4));
        assert(status[4] == 7 && status[12] == result);
    }
};
}

int main(int argc, char** argv) {
    assert(argc == 2);
    Machine m;
    assert(m.read(usb) == 0x10 && (m.read(usb + 0x48) & 0xff) == 3);
    assert(m.ohci.OpenDrive(argv[1]));
    m.ohci.SetBus(true, true, m.now);
    m.write(usb + 0x10, 0x80000042);
    assert(!m.irq);
    m.write(usb + 0x58, 0x100);
    assert(m.read(usb + 0x58) == 0x10101 && m.irq);
    m.write(usb + 0x58, 1 << 16);
    m.write(usb + 0xc, 1 << 6);
    assert(!m.irq);
    m.write(usb + 0x58, 0x10);
    m.write(usb + 8, 1);
    assert((m.read(usb + 0x58) & 0x111) == 0x111);
    m.write(usb + 0x10, 0x80000042);
    m.frame(9);
    assert(!(m.read(usb + 0x58) & 2));
    m.frame();
    assert((m.read(usb + 0x58) & 0x100013) == 0x100003 && m.irq);
    m.write(usb + 0x58, 1 << 20);
    m.write(usb + 0xc, 1 << 6);
    m.write(usb + 0x18, hcca);
    m.write(usb + 0x20, ed);
    m.write(usb + 0x04, 0x90);
    m.setup(0, {0x80, 6, 0, 1, 0, 0, 18, 0});
    m.transaction(0, 0, 2, data, data + 17);
    assert(m.memory[data - ram] == 18 && m.memory[data - ram + 1] == 1);
    m.transaction(0, 0, 1);
    m.setup(0, {0, 5, 2, 0, 0, 0, 0, 0});
    m.transaction(0, 0, 2);
    m.setup(2, {0, 9, 1, 0, 0, 0, 0, 0});
    m.transaction(2, 0, 2);
    m.frame();
    assert(!(m.read(usb + 8) & 2));
    m.write(usb + 0x04, 0xa0);
    m.write(usb + 0x28, ed);

    m.command(0x25, 8, true);
    m.transaction(2, 1, 2, data, data + 7, 64);
    assert(m.read(data) == 0 && m.read(data + 4) == 0x00020000);
    m.status(0);

    m.command(0x28, 512, true, 1);
    m.transaction(2, 1, 2, ram + 0xff0, ram + 0x31ef, 64);
    assert(m.read(ram + 0xff0) == 0x01010101 && m.read(ram + 0x3000) == 0x01010101 &&
           m.read(ram + 0x31ec) == 0x01010101);
    m.status(0);
    assert(m.ohci.Diagnostics().find("sectors=1") != std::string::npos);

    m.command(0x2a, 512, false, 1);
    m.transaction(2, 2, 1, ram + 0x1200, ram + 0x13ff, 64);
    m.status(1);

    m.ohci.Eject();
    assert(m.ohci.Diagnostics() == "off" && (m.read(usb + 0x58) & 0x30003) == 0x30000);
    m.write(usb + 0x20, usb + 0x40);
    m.write(usb + 0x04, 0x90);
    m.frame();
    assert(!(m.read(usb + 0x0c) & 0x10));
    m.write(usb + 8, 2);
    m.frame();
    assert((m.read(usb + 0x0c) & 0x10) && (m.read(usb + 4) & 0xc0) == 0xc0);
    // A reset starts a new bus session with the same drive still plugged in.
    // Loading a session drops the live host file attachment.
    std::vector<uint8_t> rom(8u << 20, 0xff), nand(spitzNandRawSize, 0xff);
    const uint32_t loop = 0xeafffffe;
    std::memcpy(rom.data(), &loop, sizeof(loop));
    SocPXA soc(deviceTypeSpitz, 64u << 20, rom.data(), rom.size(), nand.data(), nand.size(), -1, 2);
    assert(soc.SetSpitzUsbDrive(argv[1]));
    assert(soc.SpitzUsbDriveAttached() && !soc.SetUsbTestCable(true));
    assert(soc.Save());
    const auto checkpoint = soc.GetSavestate();
    const auto* bytes = static_cast<const uint8_t*>(checkpoint.data);
    std::vector<uint8_t> saved(bytes, bytes + checkpoint.size);
    soc.Reset();
    assert(soc.SpitzUsbDriveAttached() && !soc.SetUsbTestCable(true));
    assert(soc.Load(saved.size(), saved.data()));
    assert(!soc.SpitzUsbDriveAttached() && soc.SetUsbTestCable(true));

    std::puts("Zaurus OHCI hub, USB enumeration, multi-page READ(10), write protection and DMA checks passed");
}
