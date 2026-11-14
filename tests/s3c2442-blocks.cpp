// The S3C2442 blocks on their own: register reads and writes, timer ticks, interrupt priority and
// sub-source masking, EINT edges and levels, the RTC and watchdog, UART output and the NAND reads.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "s3c2442_blocks.h"
#include "rx1950_nand_image.h"
#include "s3c2442_nand.h"

template <class Block>
static uint32_t get(Block& b, uint32_t offset) {
    uint32_t v = 0xdeadbeef;
    assert(b.read(offset, v) == S3cAccess::ok);
    return v;
}

static uint32_t get(S3cNand& nand, uint32_t offset) {
    uint32_t v = 0xdeadbeef;
    assert(nand.read(offset, 4, v) == S3cAccess::ok);
    return v;
}

static void intcTests() {
    S3cIntc intc;
    intc.reset();
    assert(get(intc, 0x08) == 0xffffffff && get(intc, 0x0c) == 0x7f && get(intc, 0x1c) == 0x7fff);
    uint32_t v;
    assert(intc.read(0x20, v) == S3cAccess::unknown);
    // A masked source pends in SRCPND but not INTPND.
    intc.pulse(s3cIrq::timer4);
    assert(get(intc, 0x00) == 1u << 14 && get(intc, 0x10) == 0 && !intc.irq());
    intc.write(0x08, ~(1u << 14));
    assert(get(intc, 0x10) == 1u << 14 && get(intc, 0x14) == 14 && intc.irq());
    // SRCPND and INTPND clear by writing 1.
    intc.write(0x00, 1u << 14);
    intc.write(0x10, 1u << 14);
    assert(!intc.irq() && get(intc, 0x00) == 0);

    // Fixed priority (PRIORITY 0): EINT0 (arbiter 0) beats timer 0 (arbiter 2); in arbiter 2 timer 0
    // (REQ0) beats timer 4 (REQ4).
    intc.reset();
    intc.write(0x0c, 0);
    intc.write(0x08, 0);
    intc.pulse(s3cIrq::timer4);
    assert(get(intc, 0x14) == 14);
    intc.write(0x00, ~0u); intc.write(0x10, ~0u);
    intc.write(0x08, ~0u);
    intc.pulse(s3cIrq::timer4); intc.pulse(s3cIrq::timer0); intc.pulse(s3cIrq::eint0);
    intc.write(0x08, 0);
    assert(get(intc, 0x14) == 0);
    intc.write(0x00, 1); intc.write(0x10, 1);
    assert(get(intc, 0x14) == 10);
    intc.write(0x00, 1u << 10); intc.write(0x10, 1u << 10);
    assert(get(intc, 0x14) == 14);
    intc.write(0x00, 1u << 14); intc.write(0x10, 1u << 14);
    assert(!intc.irq());
    // ARB_SEL2 = 01 orders arbiter 2 as REQ0, 2, 3, 4, 1, 5: timer 2 before timer 1.
    intc.write(0x08, ~0u);
    intc.write(0x0c, 1u << 11);
    intc.pulse(s3cIrq::timer0 + 1); intc.pulse(s3cIrq::timer0 + 2);
    intc.write(0x08, 0);
    assert(get(intc, 0x14) == 12);
    // With ARB_MODE2 on, the winner goes last: after timer 2 (REQ2) the order is REQ0, 3, 4, 1, 2, 5.
    intc.reset();
    intc.write(0x0c, 1u << 2);
    intc.pulse(s3cIrq::timer0 + 2);
    intc.write(0x08, 0);
    assert(get(intc, 0x14) == 12 && ((get(intc, 0x0c) >> 11) & 3) == 2);
    // FIQ mode sources go to the FIQ line, not INTPND.
    intc.reset();
    intc.write(0x04, 1u << s3cIrq::rtc);
    intc.write(0x08, 0);
    intc.pulse(s3cIrq::rtc);
    assert(intc.fiq() && !intc.irq() && get(intc, 0x10) == 0);

    // Sub-sources: SUBSRCPND latches, INTSUBMSK keeps the group bit out of SRCPND.
    intc.reset();
    intc.write(0x08, 0);
    intc.subPulse(s3cSubIrq::txd0);
    assert(get(intc, 0x18) == 2 && get(intc, 0x00) == 0 && !intc.irq());
    intc.write(0x1c, 0x7fff & ~2u);
    assert(get(intc, 0x00) == 1u << s3cIrq::uart0 && get(intc, 0x14) == s3cIrq::uart0);
    // Clearing SRCPND before SUBSRCPND brings it straight back; the right order clears it.
    intc.write(0x00, 1u << s3cIrq::uart0);
    assert(get(intc, 0x00) == 1u << s3cIrq::uart0);
    intc.write(0x18, 2);
    intc.write(0x00, 1u << s3cIrq::uart0);
    intc.write(0x10, 1u << s3cIrq::uart0);
    assert(get(intc, 0x00) == 0 && !intc.irq());
    // A level source comes back after a clear while it's on.
    intc.level(s3cIrq::lcd, true);
    intc.write(0x00, 1u << s3cIrq::lcd);
    assert(get(intc, 0x00) == 1u << s3cIrq::lcd);
    intc.level(s3cIrq::lcd, false);
    intc.write(0x00, 1u << s3cIrq::lcd);
    assert(get(intc, 0x00) == 0);
}

static void clockTests() {
    S3cClock clock(16934400);
    unsigned changes = 0;
    clock.changed = [&] { ++changes; };
    assert(get(clock, 0x14) == 4 && get(clock, 0x0c) == 0xfffff0);
    // MPLL with MDIV 0x6e, PDIV 3, SDIV 1: 2 * 118 * 16.9344 MHz / (5 * 2) = 399.65 MHz.
    clock.write(0x04, (0x6e << 12) | (3 << 4) | 1);
    assert(clock.mpll() == 2ull * 118 * 16934400 / 10 && changes == 1);
    // CLKDIVN 7: HCLK FCLK/3, PCLK HCLK/2. CAMDIVN HCLK3_HALF makes it FCLK/6.
    clock.write(0x14, 7);
    assert(clock.hclk() == clock.fclk() / 3 && clock.pclk() == clock.hclk() / 2);
    clock.write(0x18, 0x100);
    assert(clock.hclk() == clock.fclk() / 6);
    // DVS runs the CPU at HCLK.
    assert(clock.cpuClock() == clock.fclk());
    clock.write(0x18, 0x1100);
    assert(clock.dvs() && clock.cpuClock() == clock.hclk());
    // IDLE holds until woken.
    clock.write(0x0c, 0xfffff4);
    assert(clock.idle());
    clock.wake();
    assert(!clock.idle() && get(clock, 0x0c) == 0xfffff0);
    uint32_t v;
    assert(clock.read(0x1c, v) == S3cAccess::unknown);

    S3cMemoryController memory;
    memory.reset();
    assert(get(memory, 0x1c) == 0x18008 && get(memory, 0x24) == 0xac0000);
    memory.write(0x2c, 0x30);
    assert(get(memory, 0x2c) == 0x30);
    assert(memory.read(0x34, v) == S3cAccess::unknown);
}

static void pwmTests() {
    S3cIntc intc;
    intc.reset();
    S3cPwm pwm;
    pwm.reset();
    // Timer 4 at PCLK / (prescaler1 + 1) / 2: prescaler 1, so one tick every 4 PCLK.
    pwm.write(0x00, 1 << 8);
    pwm.write(0x04, 0);
    pwm.write(0x3c, 9);
    pwm.write(0x08, 1u << 21);  // manual update
    assert(pwm.count(4) == 9 && get(pwm, 0x40) == 9);
    pwm.write(0x08, (1u << 20) | (1u << 22));  // start, auto reload
    assert(pwm.ticksToEvent() == 36);
    pwm.advance(35, intc);
    assert(!(intc.pending() & (1u << 14)) && pwm.count(4) == 1);
    pwm.advance(1, intc);
    assert(intc.pending() & (1u << 14) && pwm.count(4) == 0);
    intc.write(0x00, 1u << 14);
    // Reload takes a tick, so the period is TCNTB + 1 ticks.
    assert(pwm.ticksToEvent() == 40);
    pwm.advance(4, intc);
    assert(pwm.count(4) == 9 && !(intc.pending() & (1u << 14)));
    pwm.advance(36, intc);
    assert(intc.pending() & (1u << 14) && pwm.count(4) == 0);
    // A long skip lands in the right place: 10 periods and 2 ticks on.
    intc.write(0x00, 1u << 14);
    pwm.advance(4 * (10 * 10 + 2), intc);
    assert(intc.pending() & (1u << 14) && pwm.count(4) == 8);
    // TCNTB written while running waits for the reload.
    pwm.write(0x3c, 99);
    assert(pwm.count(4) == 8);
    // One shot: the timer stops at 0.
    pwm.reset();
    intc.reset();
    pwm.write(0x24, 3);  // TCNTB2
    pwm.write(0x08, 1u << 13);
    pwm.write(0x08, 1u << 12);
    pwm.advance(2 * 3, intc);
    assert(intc.pending() & (1u << 12) && get(pwm, 0x2c) == 0);
    assert(pwm.ticksToEvent() == UINT64_MAX);
    pwm.advance(1000, intc);
    assert(get(pwm, 0x2c) == 0);
    // TCMPB0 reads back.
    pwm.write(0x10, 0x55);
    assert(get(pwm, 0x10) == 0x55);
    uint32_t v;
    assert(pwm.read(0x44, v) == S3cAccess::unknown);
}

static void watchdogTests() {
    S3cIntc intc;
    intc.reset();
    S3cWatchdog wdt;
    wdt.reset();
    assert(get(wdt, 0) == 0x8021 && get(wdt, 8) == 0x8000);
    // Prescaler 0, division 16, interrupt on, reset off: count 3 runs out in 48 PCLK.
    wdt.write(0, 0x20 | 4);
    wdt.write(8, 3);
    wdt.write(4, 3);
    assert(wdt.ticksToEvent() == 48);
    wdt.advance(47, intc);
    assert(!(get(intc, 0x18) & (1u << s3cSubIrq::wdt)));
    wdt.advance(1, intc);
    assert(get(intc, 0x18) & (1u << s3cSubIrq::wdt) && !wdt.resetRequested);
    wdt.write(0, 0x20 | 1);
    wdt.advance(16 * 4, intc);
    assert(wdt.resetRequested);
    // Off, it doesn't count.
    wdt.reset();
    wdt.write(0, 0);
    wdt.advance(1u << 30, intc);
    assert(get(wdt, 8) == 0x8000 && wdt.ticksToEvent() == UINT64_MAX);
}

static void rtcTests() {
    S3cIntc intc;
    intc.reset();
    S3cRtc rtc;
    rtc.reset({6, 2, 28, 3, 23, 59, 58});
    assert(get(rtc, 0x70) == 0x58 && get(rtc, 0x7c) == 0x28 && get(rtc, 0x84) == 0x02);
    // 2006 isn't a leap year: two seconds on is March 1, and the day of week moves on.
    rtc.advance(2 * 32768, intc);
    assert(get(rtc, 0x70) == 0x00 && get(rtc, 0x74) == 0x00 && get(rtc, 0x78) == 0x00);
    assert(get(rtc, 0x7c) == 0x01 && get(rtc, 0x84) == 0x03 && get(rtc, 0x80) == 4);
    // Writes need RTCEN.
    rtc.write(0x70, 0x30);
    assert(get(rtc, 0x70) == 0x00);
    rtc.write(0x40, 1);
    rtc.write(0x70, 0x30);
    rtc.write(0x40, 0);
    assert(get(rtc, 0x70) == 0x30);
    // Tick: TICNT 0x80 | 127 is one a second.
    rtc.write(0x44, 0x80 | 127);
    assert(rtc.ticksToEvent() == 32768);
    rtc.advance(32767, intc);
    assert(!(intc.pending() & (1u << s3cIrq::tick)));
    rtc.advance(1, intc);
    assert(intc.pending() & (1u << s3cIrq::tick));
    // Alarm on seconds 0x35.
    rtc.write(0x44, 0);
    rtc.write(0x54, 0x35);
    rtc.write(0x50, 0x41);
    rtc.advance(3 * 32768, intc);
    assert(!(intc.pending() & (1u << s3cIrq::rtc)));
    rtc.advance(32768, intc);
    assert(get(rtc, 0x70) == 0x35 && intc.pending() & (1u << s3cIrq::rtc));
}

static void gpioTests() {
    S3cIntc intc;
    intc.reset();
    S3cGpio gpio;
    gpio.reset(0x32440aaa, 1);
    assert(get(gpio, 0xb0) == 0x32440aaa && get(gpio, 0xb4) == 1 && get(gpio, 0xa4) == 0xfffff0);
    // GSTATUS2 clears by writing 1.
    gpio.write(0xb4, 1, intc);
    assert(get(gpio, 0xb4) == 0);
    // Inputs read the board's level, outputs the latch.
    gpio.setInputs(S3cGpio::b, 0x7ff, 0x5, intc);
    assert(get(gpio, 0x14) == 0x5);
    gpio.write(0x10, 1u << 2, intc);  // GPB1 output
    gpio.write(0x14, 0x2, intc);
    assert(get(gpio, 0x14) == 0x7);
    gpio.write(0x14, 0x0, intc);
    assert(get(gpio, 0x14) == 0x5);
    // Port A: CON bit clear is output.
    gpio.write(0x00, 0x1fffffe, intc);
    gpio.write(0x04, 1, intc);
    assert(get(gpio, 0x04) & 1);
    // Port J and the misc registers.
    gpio.write(0xd0, 0x5, intc);
    assert(get(gpio, 0xd0) == 0x5);
    uint32_t v;
    assert(gpio.read(0x08, v) == S3cAccess::unknown);

    // EINT0 on GPF0, falling edge: straight to SRCPND bit 0.
    intc.write(0x08, 0);
    gpio.write(0x50, 2, intc);
    gpio.write(0x88, 2, intc);
    assert(!(intc.pending() & 1));
    gpio.setInputs(S3cGpio::f, 1, 0, intc);
    assert(intc.pending() & 1 && get(intc, 0x14) == 0);
    intc.write(0x00, 1); intc.write(0x10, 1);
    gpio.setInputs(S3cGpio::f, 1, 1, intc);
    assert(!(intc.pending() & 1));
    // EINT13 on GPG5, low level: EINTPEND latches, EINTMASK keeps it from SRCPND bit 5.
    gpio.write(0x60, 2u << 10, intc);
    gpio.write(0x8c, 0, intc);  // EXTINT1, all low level
    gpio.setInputs(S3cGpio::g, 1u << 5, 0, intc);
    assert(get(gpio, 0xa8) & (1u << 13) && !(intc.pending() & (1u << 5)));
    gpio.write(0xa4, 0xfffff0 & ~(1u << 13), intc);
    assert(intc.pending() & (1u << 5));
    // A held level comes back after a clear; let go, the clear sticks.
    gpio.write(0xa8, 1u << 13, intc);
    assert(get(gpio, 0xa8) & (1u << 13));
    gpio.setInputs(S3cGpio::g, 1u << 5, 1u << 5, intc);
    gpio.write(0xa8, 1u << 13, intc);
    intc.write(0x00, 1u << 5);
    assert(!(get(gpio, 0xa8) & (1u << 13)) && !(intc.pending() & (1u << 5)));
    // A pin not set to EINT makes no interrupt.
    gpio.write(0x60, 0, intc);
    gpio.setInputs(S3cGpio::g, 1u << 5, 0, intc);
    assert(!(get(gpio, 0xa8) & (1u << 13)));
}

static void uartTests() {
    S3cIntc intc;
    intc.reset();
    std::string sent;
    S3cUart uart(1);
    uart.sink = [&](unsigned channel, uint8_t byte) { assert(channel == 1); sent += char(byte); };
    for (const char c : std::string("NK\r\n")) uart.write(0x20, uint8_t(c), intc);
    assert(sent == "NK\r\n");
    // The transmitter always reads empty, nothing received.
    uint32_t v;
    assert(uart.read(0x10, v, intc) == S3cAccess::ok && v == 6);
    // Pulse TX interrupt: each byte sent raises TXD1 (sub-source 4).
    uart.write(0x04, 1u << 2, intc);
    assert(!(get(intc, 0x18) & (1u << 4)));
    uart.write(0x20, 'x', intc);
    assert(get(intc, 0x18) & (1u << 4));
    // Level TX interrupt holds while the mode is on.
    intc.write(0x18, ~0u);
    uart.write(0x04, (1u << 2) | 0x200, intc);
    intc.write(0x18, ~0u);
    assert(get(intc, 0x18) & (1u << 4));
    uart.write(0x04, 0, intc);
    intc.write(0x18, ~0u);
    assert(!(get(intc, 0x18) & (1u << 4)));
    // Received bytes: ready bit, FIFO count, RXD1 (sub-source 3).
    uart.write(0x08, 1, intc);
    uart.write(0x04, 1, intc);
    uart.receive('a', intc);
    uart.receive('b', intc);
    assert(get(intc, 0x18) & (1u << 3));
    assert(uart.read(0x10, v, intc) == S3cAccess::ok && (v & 1));
    assert(uart.read(0x18, v, intc) == S3cAccess::ok && v == 2);
    assert(uart.read(0x24, v, intc) == S3cAccess::ok && v == 'a');
    assert(uart.read(0x24, v, intc) == S3cAccess::ok && v == 'b');
    assert(uart.read(0x10, v, intc) == S3cAccess::ok && !(v & 1));
    uart.write(0x28, 0x1a, intc);
    assert(uart.read(0x28, v, intc) == S3cAccess::ok && v == 0x1a);
    assert(uart.read(0x2c, v, intc) == S3cAccess::unknown);
}

static void lcdTests() {
    S3cIntc intc;
    intc.reset();
    S3cLcd lcd;
    lcd.reset();
    // 240x320: CLKVAL 4, line 10 + 20 + 240 + 2 VCLKs (each field minus one in the registers), frame
    // 2 + 2 + 320 + 2 lines.
    lcd.write(0x04, (1u << 24) | (319u << 14) | (1u << 6) | 1, intc);
    lcd.write(0x08, (19u << 19) | (239u << 8) | 1, intc);
    lcd.write(0x0c, 9, intc);
    lcd.write(0x00, (4u << 8) | (0xcu << 1) | 1, intc);
    assert(lcd.lineTicks() == 272 * 10 && lcd.frameLines() == 326);
    // Frame sync on: INT_LCD once a frame.
    lcd.write(0x5c, 1, intc);
    intc.write(0x08, 0);
    lcd.advance(lcd.ticksToEvent() - 1, intc);
    assert(!intc.irq());
    // LINECNT in the active area counts down.
    lcd.advance(0, intc);
    uint32_t v;
    assert(lcd.read(0x10, v) == S3cAccess::ok && ((v >> 15) & 3) == 3);
    lcd.advance(1, intc);
    assert(intc.irq() && get(intc, 0x14) == s3cIrq::lcd && get(lcd, 0x54) == 2);
    lcd.advance(lcd.lineTicks() * 14, intc);
    assert(lcd.read(0x00, v) == S3cAccess::ok && (v >> 18) == 319 - 10);
    // Clearing LCDINTPND lets INT_LCD go.
    lcd.write(0x54, 2, intc);
    intc.write(0x00, 1u << s3cIrq::lcd);
    assert(!(intc.pending() & (1u << s3cIrq::lcd)));
    // The FIFO interrupt isn't modelled.
    assert(lcd.write(0x5c, 0, intc) == S3cAccess::unsupported);
}

static void nandTests() {
    S3cIntc intc;
    intc.reset();
    S3cNand nand;
    nand.reset();
    std::vector<uint8_t> flash(size_t(S3cNand::pages) * S3cNand::pageSize, 0xff);
    for (uint32_t i = 0; i < S3cNand::pageSize; ++i) flash[size_t(S3cNand::pageSize) * 33 + i] = uint8_t(i * 7);
    nand.setImage(flash);
    nand.write(0x04, 4, 1, intc);  // enabled, chip selected
    // Read ID.
    nand.write(0x08, 1, 0x90, intc);
    nand.write(0x0c, 1, 0, intc);
    uint32_t v;
    nand.read(0x10, 1, v); assert(v == 0xec);
    nand.read(0x10, 1, v); assert(v == 0x76);
    // Read A of page 33 (block 1, page 1) from column 4, with ECC from InitECC.
    nand.write(0x04, 4, 1 | 0x10, intc);
    nand.write(0x08, 1, 0x00, intc);
    for (uint32_t a : {4u, 33u, 0u, 0u}) nand.write(0x0c, 1, a, intc);
    nand.read(0x10, 4, v);
    assert(v == (uint32_t(uint8_t(4 * 7)) | uint32_t(uint8_t(5 * 7)) << 8 | uint32_t(uint8_t(6 * 7)) << 16 | uint32_t(uint8_t(7 * 7)) << 24));
    // From column 0 the whole data area gives the ECC that ecc512 works out.
    nand.write(0x04, 4, 1 | 0x10, intc);
    nand.write(0x08, 1, 0x00, intc);
    for (uint32_t a : {0u, 33u, 0u, 0u}) nand.write(0x0c, 1, a, intc);
    for (unsigned i = 0; i < 128; ++i) nand.read(0x10, 4, v);
    const uint32_t ecc = S3cNand::ecc512(&flash[size_t(S3cNand::pageSize) * 33]);
    assert(get(nand, 0x2c) == ecc);
    // The spare follows; then read C goes straight there.
    nand.read(0x10, 1, v);
    assert(v == uint8_t(512 * 7));
    nand.write(0x08, 1, 0x50, intc);
    for (uint32_t a : {5u, 33u, 0u, 0u}) nand.write(0x0c, 1, a, intc);
    nand.read(0x10, 1, v);
    assert(v == uint8_t(517 * 7));
    // ECC check: the stored ECC in NFMECCD0/1 matches, then doesn't.
    nand.write(0x14, 4, (ecc & 0xff) | ((ecc >> 8) & 0xff) << 16, intc);
    nand.write(0x18, 4, (ecc >> 16) & 0xff, intc);
    assert(get(nand, 0x24) == 0);
    nand.write(0x18, 4, ((ecc >> 16) & 0xff) ^ 1, intc);
    assert(get(nand, 0x24) == 2);
    // The SmartMedia ECC of an erased page is 0xffffff.
    std::vector<uint8_t> erased(512, 0xff);
    assert((S3cNand::ecc512(erased.data()) & 0xffffff) == 0xffffff);
    // Unique ID: 30h 65h, then a read gives the ID page until a reset.
    nand.write(0x08, 1, 0x30, intc);
    nand.write(0x08, 1, 0x65, intc);
    nand.write(0x08, 1, 0x00, intc);
    for (unsigned i = 0; i < 4; ++i) nand.write(0x0c, 1, 0, intc);
    std::vector<uint8_t> page(S3cNand::pageSize);
    for (auto& b : page) { nand.read(0x10, 1, v); b = uint8_t(v); }
    assert(page[0] == 0xec && !std::memcmp(page.data(), nand.uid, 16));
    for (unsigned i = 0; i < 16; ++i) assert(uint8_t(page[16 + i] + page[i]) == 0xff && page[224 + i] == nand.uid[i]);
    assert(page[512] == 0x55 && page[513] == 0xaa && page[518] == 0x55 && page[519] == 0xaa);
    nand.write(0x08, 1, 0xff, intc);
    nand.write(0x08, 1, 0x00, intc);
    for (uint32_t a : {4u, 33u, 0u, 0u}) nand.write(0x0c, 1, a, intc);
    nand.read(0x10, 1, v);
    assert(v == uint8_t(4 * 7));
    // 65h on its own and program aren't modelled.
    assert(nand.write(0x08, 1, 0x65, intc) == S3cAccess::unsupported);
    assert(nand.write(0x08, 1, 0x80, intc) == S3cAccess::unsupported);
}

static void nandImageTests() {
    // Two NBF sectors after the 0xa5a byte header: data, then SectorInfo.
    std::vector<uint8_t> nbf(0xa5a + 2 * 520, 0);
    for (unsigned s = 0; s < 2; ++s) {
        uint8_t* sector = &nbf[0xa5a + s * 520];
        std::memset(sector, 0x40 + s, 512);
        const uint8_t info[8] = {uint8_t(s), 0, 0, 0, 0xfd, 0xff, 0xff, 0xff};
        std::memcpy(sector + 512, info, 8);
    }
    std::vector<uint8_t> eboot(1000, 0x5a), flash;
    std::string error;
    assert(!rx1950NandImage(eboot.data(), eboot.size(), nbf.data(), nbf.size() - 1, flash, error));
    Rx1950NandOptions options;
    options.debugMessages = true;
    assert(rx1950NandImage(eboot.data(), eboot.size(), nbf.data(), nbf.size(), flash, error, options));
    assert(flash.size() == size_t(S3cNand::pages) * S3cNand::pageSize);
    auto page = [&](uint32_t block, uint32_t p) { return &flash[(size_t(block) * S3cNand::pagesPerBlock + p) * S3cNand::pageSize]; };
    // Block 0 and the eboot blocks are reserved; eboot starts at block 1.
    assert(page(0, 0)[512 + 4] == 0xfc && page(20, 31)[512 + 4] == 0xfc && page(21, 0)[512 + 4] == 0xfd);
    assert(page(1, 0)[0] == 0x5a && page(1, 1)[1000 - 512 - 1] == 0x5a && page(1, 1)[1000 - 512] == 0xff);
    // The settings page with debug messages on clears dwDbgMsgFlags.
    assert(page(0, 9)[16] == 0 && page(0, 9)[19] == 0 && page(0, 9)[12] == 0xff);
    // The store starts in block 21 with each sector's SectorInfo and ECC in the spare.
    assert(page(21, 1)[0] == 0x41 && page(21, 1)[512] == 1 && page(21, 1)[517] == 0xff);
    const uint32_t ecc = S3cNand::ecc512(page(21, 0));
    assert(page(21, 0)[520] == uint8_t(ecc) && page(21, 0)[522] == uint8_t(ecc >> 16));
    assert(page(21, 2)[0] == 0xff && page(21, 2)[512] == 0xff);
}

int main() {
    intcTests();
    clockTests();
    pwmTests();
    watchdogTests();
    rtcTests();
    gpioTests();
    uartTests();
    lcdTests();
    nandTests();
    nandImageTests();
    std::puts("s3c2442 blocks: ok");
    return 0;
}
