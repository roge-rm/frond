// The UART cache flag stays set once uart() has handed out a reference, through copy and move assignment.
// With the CPU lines connected, the known-only interrupt gate gives what the full gate gives, and a GPIO drive
// the run cache calls accepted (at one level or at both, for the in-place graphics advance) is accepted by the
// full drive too, over random AITC and GPIO register writes.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define private public
#include "imx1_bootstrap.h"
#undef private

namespace {
// The gate before the known-only shortcut: every source's inputs into a copy of the controller.
Imx1BootstrapAccess fullGate(const Imx1Bootstrap& b, const Imx1Gpio& pins) {
    auto controller = b.aitc;
    b.setInterruptInputs(controller, b.i2c, b.rtc, b.timer1, b.timer2, b.asp, b.pwm, pins);
    return b.interruptGate(controller);
}
bool fullDrive(const Imx1Bootstrap& b, uint32_t value) {
    auto next = b.gpio;
    return next.driveInput(0, value, 0x2000, false) == Imx1GpioAccess::ok && fullGate(b, next) == Imx1BootstrapAccess::ok;
}
uint32_t rng = 12345;
uint32_t next() { rng = rng * 1103515245 + 12345; return rng >> 8; }

bool samePort(const Imx1Gpio& a, const Imx1Gpio& b, unsigned n) {
    const auto& x = a.ports[n];
    const auto& y = b.ports[n];
    return !std::memcmp(x.registers, y.registers, sizeof(x.registers)) && x.sample == y.sample &&
           x.inputKnown == y.inputKnown && x.statusKnown == y.statusKnown && x.pad == y.pad && x.padKnown == y.padKnown;
}

// Committed board states always pass the gate, so the gates are compared on candidate A13 drives as well, some
// of them unknown, which make port A's output unknown when its interrupt is unmasked.
void gates() {
    Imx1Bootstrap b;
    b.connectCpuInterrupts();
    // A13 driven by the board, as the graphics chip's interrupt line is.
    assert(b.reserveGpioInput(0, 0x2000));
    // AITC enables over the GPIO ports (11, 12, 13), the UARTs (19-30) and the rest.
    const uint32_t low[] = {0x800, 0x1000, 0x2000, 0x3800, 0x800 | 0x00080000, 0x02000000, 0x7ffe0000, 0, 0xffffffff};
    const uint32_t high[] = {0, 0x40000000, 0x08000000, 0xffffffff};
    unsigned candidatesOk = 0, candidatesRefused = 0, enablesOk = 0, enablesRefused = 0, both = 0, settled = 0;
    for (unsigned n = 0; n < 40000; ++n) {
        uint32_t value;
        switch (next() % 9) {
        case 0: value = low[next() % 9]; b.access(0x00223014, 4, true, value); break;
        case 1: value = high[next() % 4]; b.access(0x00223010, 4, true, value); break;
        case 2:
            // NIMASK, mostly back to 31 (nothing masked), or INTENNUM / INTDISNUM.
            if (next() % 3) { value = next() % 64; b.access(0x00223008 + 4 * (next() & 1), 4, true, value); }
            else { value = next() % 4 ? 31 : next() % 32; b.access(0x00223004, 4, true, value); }
            break;
        case 3: value = next() & 0x2000 ? 0x2000 : 0; b.access(0x0021c000 + 0x100 * (next() % 4) + 0x30, 4, true, value); break;
        case 4: value = next() & 0x0c000000; b.access(0x0021c000 + 0x28 + 4 * (next() & 1), 4, true, value); break;
        case 5: value = next() & 0x2000 ? 0x2000 : 0; b.access(0x0021c000 + 0x20 + 0x14 * (next() & 1), 4, true, value); break;
        case 6: b.driveGpioInput(0, 0, 0, true, false); break;
        default: b.driveGpioInput(0, next() & 1 ? 0x2000 : 0, 0x2000, true, false); break;
        }
        assert(b.connectedGate(b.gpio) == fullGate(b, b.gpio));
        for (auto [level, known] : {std::pair<uint32_t, uint32_t>{0, 0}, {0, 0x2000}, {0x2000, 0x2000}}) {
            auto candidate = b.gpio;
            if (candidate.driveInput(0, level, known, false) != Imx1GpioAccess::ok) continue;
            const auto gate = b.connectedGate(candidate);
            assert(gate == fullGate(b, candidate));
            ++(gate == Imx1BootstrapAccess::ok ? candidatesOk : candidatesRefused);
        }
        // And on candidate AITC enables, which the board's own gate would refuse to commit.
        {
            auto candidate = b;
            uint32_t enable = low[next() % 9];
            candidate.aitc.access(0x00223014, 4, true, enable);
            enable = high[next() % 4];
            candidate.aitc.access(0x00223010, 4, true, enable);
            const auto gate = candidate.connectedGate(candidate.gpio);
            assert(gate == fullGate(candidate, candidate.gpio));
            ++(gate == Imx1BootstrapAccess::ok ? enablesOk : enablesRefused);
        }
        for (uint32_t level : {0u, 0x2000u}) {
            if (b.gpioDriveAccepted(0, level, 0x2000)) assert(fullDrive(b, level));
            // A settled drive changes nothing when it comes again.
            if (b.gpioDriveSettled(0, level, 0x2000)) {
                auto again = b.gpio;
                assert(again.driveInput(0, level, 0x2000, false) == Imx1GpioAccess::ok);
                for (unsigned port = 0; port < 4; ++port) assert(samePort(again, b.gpio, port));
                ++settled;
            }
        }
        if (b.gpioDriveBothAccepted(0, 0x2000)) { assert(fullDrive(b, 0) && fullDrive(b, 0x2000)); ++both; }
    }
    std::printf("gates: candidate drives accepted %u, refused %u; candidate enables accepted %u, refused %u; both levels %u; "
                "settled %u\n", candidatesOk, candidatesRefused, enablesOk, enablesRefused, both, settled);
    std::fflush(stdout);
    assert(candidatesOk && candidatesRefused && enablesOk && enablesRefused && both && settled);
}
}  // namespace

int main() {
    gates();
    Imx1Bootstrap shared;
    shared.uart(1);
    assert(shared.runCache.uartShared);
    Imx1Bootstrap fresh;
    assert(!fresh.runCache.uartShared);
    shared = fresh;
    assert(shared.runCache.uartShared);
    shared = Imx1Bootstrap();
    assert(shared.runCache.uartShared);
    fresh = shared;
    assert(fresh.runCache.uartShared);
    std::puts("PASS: uartShared is kept through board assignment, known-only gates and cached drives match the full gate");
}
