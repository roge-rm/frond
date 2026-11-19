// S3C2442 machine checks on the CPU: idle ends or stops, the budget bounds idle time, and start() resets
// the undefined instruction stop.

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "CPU.h"
#include "rx1950_board.h"
#include "rx1950_power.h"
#include "s3c2442_machine.h"

namespace {
    constexpr uint32_t code = 0x30000000;
    constexpr uint32_t wfi = 0xee070f90, loop = 0xeafffffe, undefined = 0xe7f000f0;

    // A machine with the watchdog off (its reset value runs it with the reset on), every interrupt
    // masked and a branch to itself on each vector.
    std::unique_ptr<S3c2442Machine> make(const std::vector<uint32_t>& program, const S3c2442Options& options = {}) {
        std::string error;
        auto m = S3c2442Machine::create(options, error);
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
    // An SFR write that unmasks a pending interrupt: the CPU runs the next instruction before it takes the
    // IRQ. UART0's level TX interrupt is pending with INT_UART0 open; the guest unmasks TXD0, then sets r2.
    {
        auto m = make({0xe321f013, 0xe3a0044a, 0xe3e01002, 0xe580101c, 0xe3a02001, loop});
        // msr cpsr_c, #0x13; mov r0, #0x4a000000; mvn r1, #2; str r1, [r0, #0x1c]; mov r2, #1
        m->uarts[0].write(0x04, (1u << 2) | 0x200, m->intc);
        m->intc.intmsk &= ~(1u << s3cIrq::uart0);
        assert(!(m->intc.pending() & (1u << s3cIrq::uart0)));
        assert(m->run(1000) == S3c2442Stop::budget);
        assert(mode(*m) == 0x12 && cpuGetRegExternal(m->cpu(), 2) == 1);
    }
    // The rx1950 with no serial cable reads GPF6 low; every other pin stays high.
    {
        S3c2442Options options;
        options.pinLevels[rx1950CablePort] &= ~(1u << rx1950CableBit);
        auto m = make({loop}, options);
        uint32_t gpf = 0;
        assert(m->gpio.read(0x54, gpf) == S3cAccess::ok && (gpf & 0xff) == 0xbf);
    }
    // Fast against Exact on a WTCNT polling loop (the lean path's SFR read) with the watchdog counting and
    // its interrupt raised (IRQs stay masked): the same registers, instructions and time at every stop.
    // mov r0, #0x53000000; ldr r1, [r0, #8]; add r2, r2, r1; add r3, r3, #1; b to the ldr
    const std::vector<uint32_t> poll{0xe3a00453, 0xe5901008, 0xe0822001, 0xe2833001, 0xeafffffb};
    {
        auto exact = make(poll), fast = make(poll);
        if (fast->setFastEngine(true)) {
            for (auto* m : {exact.get(), fast.get()}) {
                m->watchdog.wtdat = m->watchdog.wtcnt = 0x400;
                m->watchdog.wtcon = 0x0124;  // prescaler 1, divide by 16, counting, interrupt, no reset
                m->intc.intmsk &= ~(1u << s3cIrq::wdtAc97);
                m->intc.intsubmsk &= ~(1u << s3cSubIrq::wdt);
            }
            for (const uint64_t slice : {1u, 7u, 1000u, 33333u, 250000u, 1000000u}) {
                exact->run(slice);
                fast->run(slice);
                for (unsigned r = 0; r < 16; ++r)
                    assert(cpuGetRegExternal(exact->cpu(), r) == cpuGetRegExternal(fast->cpu(), r));
                assert(exact->retired() == fast->retired() && exact->nanoseconds() == fast->nanoseconds());
            }
            assert(fast->leanRetired() + fast->jitRetired() > fast->retired() / 2);
        }
    }
    // The battery setting on the rx1950's inputs: every level battdrvr can read back reads back exactly through
    // its own sums; the ones no count gives read the next percent up. All stay above its 3300 mV power off.
    {
        bool reachable[101] = {};
        for (unsigned c = 0; c < 1024; ++c) {
            const unsigned got = rx1950GuestPercent(rx1950CountMilliVolts(uint16_t(c)));
            if (got <= 100) reachable[got] = true;
        }
        const unsigned unreachable[] = {18, 26, 29, 32, 35, 38, 41, 44, 47};
        for (unsigned p = 0; p <= 100; ++p) {
            const bool listed = std::find(std::begin(unreachable), std::end(unreachable), p) != std::end(unreachable);
            assert(reachable[p] == !listed);
            const unsigned mv = rx1950CountMilliVolts(rx1950BatteryAin0(p));
            const unsigned got = rx1950GuestPercent(mv);
            if (got != p + listed) std::fprintf(stderr, "battery %u%%: %u mV reads %u%%\n", p, mv, got);
            assert(got == p + listed && mv > 3300);
        }
    }
    {
        BatteryPower full;
        assert(rx1950CountMilliVolts(rx1950PowerInputs(full).adc[0]) >= 4120);
    }
    // Each level and charging state on a running machine: AIN0-3, GPF2 (low with AC) and GPF3 (high when the
    // charge is done or off the charger), and EINT2 on both edges as the OAL sets it, raised by plugging in
    // and pulling out.
    {
        BatteryPower start;
        assert(BatteryPower::Parse("level 30", start));
        S3c2442Options options;
        rx1950BatteryPins(start, options);
        auto m = make({loop}, options);
        rx1950ApplyBattery(*m, start);
        assert(m->gpio.write(0x50, 2u << 4, m->intc) == S3cAccess::ok);           // GPF2 as EINT2
        assert(m->gpio.write(0x88, 6u << 8, m->intc) == S3cAccess::ok);           // EINT2 both edges
        m->intc.srcpnd = 0;
        struct Case { const char* spec; bool ac, done; bool edge; };
        const Case cases[] = {
            {"level 30", false, true, false},  {"level 30 charging", true, false, true},
            {"level 5 charging", true, false, false}, {"max charging", true, true, false},
            {"level 100 battery", false, true, true},  {"host 0", false, true, false},
            {"host 70 charging", true, false, true},   {"max", false, true, true},
        };
        for (const auto& c : cases) {
            BatteryPower b;
            assert(BatteryPower::Parse(c.spec, b));
            rx1950ApplyBattery(*m, b);
            uint32_t gpf = 0, adcdat = 0;
            assert(m->gpio.read(0x54, gpf) == S3cAccess::ok);
            assert(bool(gpf & 4) == !c.ac && bool(gpf & 8) == c.done);
            assert(bool(m->intc.srcpnd & (1u << s3cIrq::eint2)) == c.edge);
            m->intc.srcpnd = 0;
            assert(rx1950GuestPercent(rx1950CountMilliVolts(m->adc.channel[0])) + 1 >= b.Percent());
            assert(m->adc.channel[1] == 0 && m->adc.channel[2] == 451 && m->adc.channel[3] == rx1950PackIdCount);
            // AIN0 through a normal conversion, as battdrvr reads it: ADCCON prescaler 49, channel 0, start.
            m->adc.write(0, 0x4c40 | 1, m->intc);
            m->run(100000);
            assert(m->adc.read(0x0c, adcdat) == S3cAccess::ok && (adcdat & 0x3ff) == m->adc.channel[0]);
            assert(rx1950BatteryState(b) == b.Describe() && rx1950BatteryState(b).find("no-charging") == std::string::npos);
        }
        // Pack ID 0 (500-700 mV, no 1.3191 divider) and the thermistor at 25 C.
        const unsigned packMv = rx1950PackIdCount * 3300 / 1023;
        assert(packMv >= 500 && packMv <= 700);
    }
    // Sleep: CLKCON POWER_OFF stops the CPU, time goes on for the RTC only, and the power key (EINT0 on a
    // falling edge) wakes it with the wake-up reset: GSTATUS2 OFFRST, the source pending, blocks reset, the
    // resetLoader's start, the GPIO block kept. Then the RTC alarm wakes it on its own.
    // mov r0, #0x4c000000; mov r1, #8; str r1, [r0, #0xc] (CLKCON POWER_OFF); b .
    for (const bool fast : {false, true}) {
        std::vector<uint32_t> program{0xe3a0044c, 0xe3a01008, 0xe580100c, loop};
        program.resize(0x100 / 4 + 1, 0);
        program.back() = loop;  // where the loader starts
        auto m = make(program);
        if (fast && !m->setFastEngine(true)) continue;
        unsigned loads = 0;
        m->resetLoader = [&] { ++loads; m->start(code + 0x100); };
        assert(m->gpio.write(0x50, 2, m->intc) == S3cAccess::ok);  // GPF0 as EINT0
        assert(m->gpio.write(0x88, 2, m->intc) == S3cAccess::ok);  // falling edge
        m->pwm.timers[0].buffer = 150;
        m->pwm.timers[0].compare = 75;
        m->pwm.tcon = 1;
        assert(m->gpio.write(0x10, 2, m->intc) == S3cAccess::ok);  // GPB0 as TOUT0
        assert(m->backlightLevel() == 76 * 1000 / 151);
        // TCMPB0 above TCNTB0: the count never meets the compare, TOUT0 stays at its idle level (low, or high
        // with the inverter on).
        m->pwm.timers[0].compare = 200;
        assert(m->backlightLevel() == 0);
        m->pwm.tcon = 1 | 4;
        assert(m->backlightLevel() == 1000);
        m->pwm.timers[0].compare = 150;
        assert(m->backlightLevel() == 0);
        m->pwm.tcon = 1;
        assert(m->backlightLevel() == 1000);
        m->pwm.timers[0].compare = 75;
        assert(m->run(1000) == S3c2442Stop::budget && m->asleep() && m->sleepCount() == 1);
        assert(m->backlightLevel() == -1);
        const uint64_t ns = m->nanoseconds(), retired = m->retired();
        assert(m->run(50000000) == S3c2442Stop::budget && m->asleep() && m->retired() == retired && m->nanoseconds() > ns);
        assert(m->pwm.tcon == 1);  // nothing but the RTC moves asleep
        m->setPin(S3cGpio::f, 0, false);
        m->run(10);
        assert(!m->asleep() && m->wakeCount() == 1 && loads == 1 && m->gpio.gstatus[2] == 2 && m->lastWakeSources() == 1);
        assert((m->intc.srcpnd & 1) && m->intc.intmsk == 0xffffffff && m->pwm.tcon == 0 && !m->clock.powerOff());
        assert(m->gpio.con[S3cGpio::f] == 2 && m->pc() == code + 0x100);
        m->setPin(S3cGpio::f, 0, true);
        // Asleep again (from the start of the program), then the alarm two seconds on.
        m->start(code);
        m->rtc.rtcalm = 0x41;  // ALMEN, seconds
        const unsigned second = ((m->rtc.bcd[0] >> 4) * 10 + (m->rtc.bcd[0] & 0xf) + 2) % 60;
        m->rtc.alarm[0] = (second / 10) << 4 | second % 10;
        m->intc.srcpnd = 0;
        assert(m->run(1000) == S3c2442Stop::budget && m->asleep());
        m->run(4000000000ull);
        assert(!m->asleep() && m->wakeCount() == 2 && (m->intc.srcpnd & (1u << s3cIrq::rtc)));
        assert(m->lastWakeSources() == 1u << s3cIrq::rtc);
    }
    // The LEDs from the OAL's LED states (nk 0x800529c8) once the loader has made their pins outputs.
    {
        auto m = make({loop});
        rx1950LoaderPins(m->gpio);
        assert((m->gpio.con[S3cGpio::b] & 3) == 2);
        auto gpa = [&](uint32_t v, bool gpj6) {
            m->gpio.dat[S3cGpio::a] = v;
            m->gpio.dat[S3cGpio::j] = gpj6 ? 0x40 : 0;
        };
        gpa(0x201000, false);  // all off
        auto l = rx1950Leds(*m);
        assert(!l.any() && !rx1950NotificationLed(*m));
        gpa(0x201000 | 0x18, true);  // charging: amber blinking
        l = rx1950Leds(*m);
        assert(l.green && l.red && l.blinking && !l.blue && !rx1950NotificationLed(*m));
        gpa(0x201000 | 0xc0, false);  // charged
        l = rx1950Leds(*m);
        assert(l.green && l.red && !l.blinking);
        gpa(0x201000 | 0x10, true);  // notification
        l = rx1950Leds(*m);
        assert(l.green && !l.red && l.blinking && rx1950NotificationLed(*m));
        gpa(0x201000 | 0x800, false);  // wireless
        assert(rx1950Leds(*m).blue);
    }
    std::printf("s3c2442 machine: ok\n");
}
