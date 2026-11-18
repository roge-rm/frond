// S3C2442 machine checks on the CPU: idle ends or stops, the budget bounds idle time, and start() resets
// the undefined instruction stop.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "CPU.h"
#include "s3c2442_machine.h"

namespace {
    constexpr uint32_t code = 0x30000000;
    constexpr uint32_t wfi = 0xee070f90, loop = 0xeafffffe, undefined = 0xe7f000f0;

    // A machine with the watchdog off (its reset value runs it with the reset on), every interrupt
    // masked and a branch to itself on each vector.
    std::unique_ptr<S3c2442Machine> make(const std::vector<uint32_t>& program) {
        std::string error;
        auto m = S3c2442Machine::create(S3c2442Options{}, error);
        assert(m);
        m->watchdog.wtcon = 0;
        for (uint32_t v = 0; v < 0x20; v += 4) std::memcpy(m->steppingstone + v, &loop, 4);
        assert(m->loadRam(code, reinterpret_cast<const uint8_t*>(program.data()), program.size() * 4));
        m->start(code);
        return m;
    }

    unsigned mode(S3c2442Machine& m) { return cpuGetRegExternal(m.cpu(), ARM_REG_NUM_CPSR) & 0x1f; }

    void pwmTimer4(S3c2442Machine& m, uint32_t buffer) {
        m.pwm.write(0x3c, buffer);                // TCNTB4
        m.pwm.write(0x08, (1u << 21) | (1u << 22));  // manual update, auto reload
        m.pwm.write(0x08, (1u << 20) | (1u << 22));  // start, auto reload
    }
}

int main() {
    // A masked RTC tick can't end a wait for interrupt.
    {
        auto m = make({wfi, loop});
        m->rtc.ticnt = 0x80;
        assert(m->run(1000000000ull) == S3c2442Stop::idleNoWake);
    }
    // Unmasked, it does, and the CPU takes the IRQ. The tick is every 1/128 s, 8.4 M cycles at the reset
    // PLL, and idle cycles count against the budget.
    {
        auto m = make({0xe321f013, wfi, loop});  // msr cpsr_c, #0x13
        m->rtc.ticnt = 0x80;
        m->intc.intmsk &= ~(1u << s3cIrq::tick);
        const auto stop = m->run(20000000);
        if (stop != S3c2442Stop::budget || mode(*m) != 0x12)
            std::fprintf(stderr, "tick wake: %s, mode %x, pc %08x\n", m->stopText().c_str(), mode(*m), m->pc());
        assert(stop == S3c2442Stop::budget && mode(*m) == 0x12);
    }
    // A masked auto-reload PWM timer under CLKCON IDLE.
    {
        auto m = make({loop});
        pwmTimer4(*m, 100);
        m->clock.clkcon |= 4;
        assert(m->run(1000000000ull) == S3c2442Stop::idleNoWake);
    }
    // The watchdog with WTCNT and WTDAT 0 never runs out, with the interrupt or the reset on.
    for (uint32_t on : {4u, 1u}) {
        auto m = make({wfi, loop});
        m->watchdog.wtdat = m->watchdog.wtcnt = 0;
        m->watchdog.wtcon = 0x20 | on;
        m->intc.intmsk &= ~(1u << s3cIrq::wdtAc97);
        m->intc.intsubmsk &= ~(1u << s3cSubIrq::wdt);
        assert(m->run(1000000000ull) == S3c2442Stop::idleNoWake);
    }
    // Idle time counts against the budget: an unmasked timer far off doesn't run past it.
    {
        auto m = make({wfi, loop});
        m->pwm.write(0x00, 0xff00);  // prescaler 255 for timers 2-4
        m->pwm.write(0x04, 3u << 16);  // divider 1/16 for timer 4
        pwmTimer4(*m, 0xffff);
        m->intc.intmsk &= ~(1u << s3cIrq::timer4);
        assert(m->run(1000) == S3c2442Stop::budget);
        assert(m->retired() + m->idleCycles() <= 1000 + 64 && m->idleCycles() > 0);
    }
    // An undefined instruction stops the run; after start() the next run doesn't stop for it again.
    {
        auto m = make({undefined, loop});
        assert(m->run(1000) == S3c2442Stop::undefined && m->stopInfo().pc == code);
        const uint32_t again[] = {loop};
        m->loadRam(code, reinterpret_cast<const uint8_t*>(again), 4);
        m->start(code);
        assert(m->run(1000) == S3c2442Stop::budget);
    }
    // A real undefined instruction followed by a VFP probe in the same batch still stops, at the first one.
    // The undefined vector returns to the next instruction.
    {
        constexpr uint32_t vfp = 0xeef00a10, returnNext = 0xe1b0f00e;  // fmrx r0, fpsid; movs pc, lr
        auto m = make({undefined, vfp, loop});
        std::memcpy(m->steppingstone + 4, &returnNext, 4);
        assert(m->run(1000) == S3c2442Stop::undefined && m->stopInfo().pc == code);
        assert(m->vfpProbeCount() == 1);
        auto n = make({vfp, undefined, loop});
        std::memcpy(n->steppingstone + 4, &returnNext, 4);
        assert(n->run(1000) == S3c2442Stop::undefined && n->stopInfo().pc == code + 4);
    }
    // Host input: the power key on GPF0 as EINT0 on a falling edge, and the stylus down with the touch
    // screen waiting for it, each raise their interrupt at once.
    {
        auto m = make({loop});
        m->gpio.write(0x88, 2, m->intc);  // EXTINT0: EINT0 on a falling edge
        m->gpio.write(0x50, 2, m->intc);  // GPFCON: GPF0 as EINT0
        m->setPin(S3cGpio::f, 0, false);
        assert(m->intc.pending() & (1u << s3cIrq::eint0));
        m->setPin(S3cGpio::f, 0, true);
        m->adc.write(0x04, 0xd3, m->intc);
        m->intc.intsubmsk &= ~(1u << s3cSubIrq::tc);
        m->setStylus(true, 100, 200);
        assert((m->intc.pending() & (1u << s3cIrq::adc)) && m->adc.stylusDown());
    }
    std::printf("s3c2442 machine: ok\n");
}
