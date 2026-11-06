// E2 and TX connector: the FFUART is the serial port, and the HotSync line (E2 GPIO4, TX GPIO10)
// goes low on a press and comes back, also after a save, load or hard reset during the hold.
// The Zire 72 has neither.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include "CPU.h"
#include "mem.h"
#include "pxa_UART.h"
#include "soc_PXA.h"

static uint32_t read(SocPXA& soc, uint32_t address) {
    uint32_t value = 0;
    assert(memAccess(cpuGetMem(soc.GetCpu()), address, 4, false, &value));
    return value;
}
static void write(SocPXA& soc, uint32_t address, uint32_t value) {
    assert(memAccess(cpuGetMem(soc.GetCpu()), address, 4, true, &value));
}
static void run(SocPXA& soc, unsigned ms) {
    for (unsigned i = 0; i < ms; ++i) assert(soc.Run(1000, 1000000) > 0);
}
static void idle(SocPXA& soc) {
    write(soc, 0xa0000000, 0xeafffffe);
    cpuInvalidateCodeCache(soc.GetCpu());
    cpuSetReg(soc.GetCpu(), ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC | ARM_SR_I | ARM_SR_F);
    cpuSetReg(soc.GetCpu(), 15, 0xa0000000);
}
static bool high(SocPXA& soc, unsigned pin) { return read(soc, 0x40e00000) & (1u << pin); }

struct Peer {
    std::deque<uint16_t> in;
    std::vector<uint16_t> out;
};

static std::vector<uint8_t> save(SocPXA& soc) {
    assert(soc.Save());
    auto state = soc.GetSavestate();
    const auto* bytes = static_cast<const uint8_t*>(state.data);
    return {bytes, bytes + state.size};
}

static void board(DeviceType5 type, int pin, int powerPin) {
    const bool e2 = type == deviceTypeE2;
    std::vector<uint8_t> rom(e2 ? 8u << 20 : 16u << 20), nand(e2 ? 34603008 : 2112u * 64 * 1024, 0xff);
    SocPXA soc(type, e2 ? 16u << 20 : 32u << 20, rom.data(), rom.size(), nand.data(), nand.size(), -1, e2 ? 0 : 2);
    idle(soc);
    // The FFUART is the serial port.
    auto* uart = soc.GetSerialUart();
    assert(uart);
    Peer peer;
    // A host endpoint takes the port over the way HostSerial does.
    assert(pxaUartAttachExternal(uart, [](void* p) -> uint_fast16_t {
        auto& peer = *static_cast<Peer*>(p);
        if (peer.in.empty()) return UART_CHAR_NONE;
        auto c = peer.in.front(); peer.in.pop_front(); return c;
    }, [](uint_fast16_t c, void* p) { static_cast<Peer*>(p)->out.push_back(c); },
    [](void* p) { return !static_cast<Peer*>(p)->in.empty(); }, nullptr, [](void*) {}, &peer));
    const uint32_t base = PXA_FFUART_BASE;
    write(soc, base + 4 * 3, 0x83); write(soc, base, 12); write(soc, base + 4, 0); write(soc, base + 4 * 3, 3);
    write(soc, base + 4 * 2, 7); write(soc, base + 4, 0x40);
    write(soc, base, 0x5a);
    peer.in = {0xa5};
    run(soc, 20);
    assert(peer.out.size() == 1 && peer.out[0] == 0x5a);
    assert(read(soc, base + 4 * 5) & 1 && read(soc, base) == 0xa5);
    // The port runs at its baud rate: at 76800 baud a 16-byte burst takes about 2 ms.
    peer.out.clear();
    for (unsigned i = 0; i < 16; ++i) write(soc, base, i);
    run(soc, 1);
    assert(!peer.out.empty() && peer.out.size() < 16);
    run(soc, 3);
    assert(peer.out.size() == 16);

    // HotSync: high at rest, low while pressed, high again, no second press while the first is held.
    assert(high(soc, pin));
    assert(soc.PressHotSync());
    assert(!high(soc, pin));
    assert(!soc.PressHotSync());
    run(soc, 300);
    assert(!high(soc, pin));
    run(soc, 300);
    assert(high(soc, pin));
    assert(soc.PressHotSync());
    run(soc, 600);
    assert(high(soc, pin));

    // A session saved during the hold loads with the line high, and it stays high.
    assert(soc.PressHotSync());
    run(soc, 100);
    auto held = save(soc);
    assert(soc.Load(held.size(), held.data()));
    assert(high(soc, pin));
    run(soc, 600);
    assert(high(soc, pin));

    // Another hold lets go of the HotSync press.
    assert(soc.PressHotSync());
    soc.JamKey(keyIdPower, 100);
    assert(high(soc, pin) && !high(soc, powerPin));
    run(soc, 200);
    assert(high(soc, powerPin));

    // A hard reset during the press holds power, with the HotSync line high.
    assert(soc.PressHotSync());
    soc.HardReset();
    idle(soc);
    assert(high(soc, pin) && !high(soc, powerPin));
    run(soc, 600);
    assert(high(soc, pin) && !high(soc, powerPin));
    // Saved during the 9 s power hold, the session loads with power up.
    auto powerHeld = save(soc);
    assert(soc.Load(powerHeld.size(), powerHeld.data()));
    assert(high(soc, pin) && high(soc, powerPin));
    assert(soc.PressHotSync());
}

int main() {
    board(deviceTypeE2, 4, 2);
    board(deviceTypeTX, 10, 0);
    std::vector<uint8_t> rom(16u << 20);
    SocPXA zire(deviceTypeZire72, 32u << 20, rom.data(), rom.size(), nullptr, 0, -1, 2);
    assert(!zire.GetSerialUart() && !zire.PressHotSync());
    puts("PASS: E2 and TX serial port and HotSync line; the Zire 72 has neither");
}
