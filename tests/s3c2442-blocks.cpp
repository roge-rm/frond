// The S3C2442 blocks on their own: register reads and writes, timer ticks, interrupt priority and
// sub-source masking, EINT edges and levels, the RTC and watchdog, UART output and the NAND reads.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "s3c2442_blocks.h"
#include "rx1950_board.h"
#include "rx1950_nand_image.h"
#include "s3c2442_nand.h"
#include "uda1380.h"

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
    // The rx1950's start: its OAL reads BCDYEAR + 1950 as the year and BCDDAY - 1 as the day of week.
    S3cRtc rx;
    rx.reset(rx1950RtcStart);
    assert(get(rx, 0x88) + 1950 == 2006 && get(rx, 0x84) == 0x01 && get(rx, 0x7c) == 0x01);
    assert(get(rx, 0x80) - 1 == 0 && get(rx, 0x78) == 0x12 && get(rx, 0x74) == 0 && get(rx, 0x70) == 0);
    // A year byte that isn't BCD (2010 is 60, 0x3c) stays as it is while the seconds count; a minute on is 12:01.
    S3cRtc rx2010;
    rx2010.reset({rx1950RtcYear(2010), 1, 1, 6, 12, 0, 0});
    rx2010.advance(61 * 32768, intc);
    assert(get(rx2010, 0x88) == 0x3c && get(rx2010, 0x78) == 0x12 && get(rx2010, 0x74) == 0x01 && get(rx2010, 0x70) == 0x01);
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

    // The frame: 4 x 2 pixels at 16 bpp 5:6:5 with HWSWP (the first pixel in the low halfword), lines 6
    // halfwords apart (PAGEWIDTH 4, OFFSIZE 2), from 0x30100000.
    S3cLcd small;
    small.reset();
    small.write(0x04, (1u << 14), intc);
    small.write(0x08, (3u << 8), intc);
    small.write(0x10, (1u << 11) | 1, intc);
    small.write(0x14, 0x30100000 >> 1, intc);
    small.write(0x1c, (2u << 11) | 4, intc);
    const uint16_t mem[12] = {0xf800, 0x07e0, 0x001f, 0xffff, 0, 0, 0x0000, 0x8410, 0x0821, 0xf800, 0, 0};
    auto memory = [&](uint32_t pa, void* out, size_t n) {
        if (pa < 0x30100000 || pa + n > 0x30100000 + sizeof mem) return false;
        std::memcpy(out, reinterpret_cast<const uint8_t*>(mem) + (pa - 0x30100000), n);
        return true;
    };
    std::vector<uint32_t> pixels;
    unsigned w = 0, h = 0;
    std::string why;
    assert(!small.frame(memory, pixels, w, h, why) && why.find("off") != std::string::npos);
    small.write(0x00, (0x3u << 5) | (0xcu << 1) | 1, intc);
    assert(small.frame(memory, pixels, w, h, why) && w == 4 && h == 2);
    const uint32_t want[8] = {0xff0000, 0x00ff00, 0x0000ff, 0xffffff, 0x000000, 0x868286, 0x080408, 0xff0000};
    for (unsigned i = 0; i < 8; ++i) assert(pixels[i] == want[i]);
    // Without HWSWP the first pixel is the high halfword.
    small.write(0x10, 1u << 11, intc);
    assert(small.frame(memory, pixels, w, h, why) && pixels[0] == 0x00ff00 && pixels[1] == 0xff0000);
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
    // Row bits above the chip's size are ignored, as for program and erase: row 0x20000 + 33 is page 33.
    nand.write(0x08, 1, 0x00, intc);
    for (uint32_t a : {4u, 33u, 0u, 2u}) nand.write(0x0c, 1, a, intc);
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
    // 65h on its own isn't a command, nor is a confirm without its setup.
    assert(nand.write(0x08, 1, 0x65, intc) == S3cAccess::unsupported);
    assert(nand.write(0x08, 1, 0x10, intc) == S3cAccess::unsupported);
    assert(nand.write(0x08, 1, 0xd0, intc) == S3cAccess::unsupported);
    assert(nand.write(0x10, 4, 0, intc) == S3cAccess::unsupported);

    // Program page 70 through pointer A with 32-bit writes: the main area ECC is made as data goes out,
    // bits only clear, and its block is dirty.
    nand.clearDirty();
    std::vector<uint8_t> data(512);
    for (uint32_t i = 0; i < 512; ++i) data[i] = uint8_t(i * 13 + 1);
    nand.write(0x08, 1, 0x00, intc);
    nand.write(0x04, 4, 1 | 0x10, intc);
    nand.write(0x08, 1, 0x80, intc);
    for (uint32_t a : {0u, 70u, 0u, 0u}) nand.write(0x0c, 1, a, intc);
    for (uint32_t i = 0; i < 512; i += 4) {
        uint32_t w;
        std::memcpy(&w, &data[i], 4);
        assert(nand.write(0x10, 4, w, intc) == S3cAccess::ok);
    }
    assert(get(nand, 0x2c) == S3cNand::ecc512(data.data()));
    const uint8_t info[8] = {7, 0, 0, 0, 0xff, 0xff, 0xff, 0xff};
    for (uint8_t b : info) nand.write(0x10, 1, b, intc);
    nand.write(0x20, 4, 4, intc);
    assert(nand.write(0x08, 1, 0x10, intc) == S3cAccess::ok);
    assert(get(nand, 0x20) & 4);
    nand.write(0x08, 1, 0x70, intc);
    nand.read(0x10, 1, v);
    assert(!(v & 1));
    assert(!std::memcmp(&nand.flash[size_t(S3cNand::pageSize) * 70], data.data(), 512));
    assert(!std::memcmp(&nand.flash[size_t(S3cNand::pageSize) * 70 + 512], info, 8));
    assert(nand.flash[size_t(S3cNand::pageSize) * 70 + 8 + 512] == 0xff);
    assert(nand.blockDirty(2) && !nand.blockDirty(1) && !nand.blockDirty(3) && nand.programs == 1);
    // Pointer C programs the spare from its column, ANDed with what's there.
    nand.write(0x08, 1, 0x50, intc);
    nand.write(0x08, 1, 0x80, intc);
    for (uint32_t a : {4u, 70u, 0u, 0u}) nand.write(0x0c, 1, a, intc);
    nand.write(0x10, 1, 0x0f, intc);
    nand.write(0x08, 1, 0x10, intc);
    assert(nand.flash[size_t(S3cNand::pageSize) * 70 + 512 + 4] == 0x0f);
    assert(nand.flash[size_t(S3cNand::pageSize) * 70 + 512] == 7);
    // Erase block 2 (pages 64-95) from its row address.
    nand.write(0x08, 1, 0x60, intc);
    for (uint32_t a : {70u, 0u, 0u}) nand.write(0x0c, 1, a, intc);
    assert(nand.write(0x08, 1, 0xd0, intc) == S3cAccess::ok);
    for (uint32_t i = 64 * S3cNand::pageSize; i < 96 * S3cNand::pageSize; ++i) assert(nand.flash[i] == 0xff);
    assert(nand.flash[size_t(S3cNand::pageSize) * 33 + 4] == uint8_t(4 * 7) && nand.erases == 1);
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

// The blocks the OS's drivers set up before they're modelled in full: an empty SD slot, no USB cable,
// the ADC with the stylus up, DMA and IIS registers, IIC with every device answering, and RTCRST.
static void quietBlockTests() {
    S3cSdi sdi;
    sdi.reset();
    assert(sdi.write(0x08, 0) == S3cAccess::ok && sdi.write(0x0c, 0x140) == S3cAccess::ok);
    assert(get(sdi, 0x10) == 0x400 && !(get(sdi, 0x0c) & 0x100) && sdi.commands == 1);
    sdi.write(0x10, 0x400);
    assert(get(sdi, 0x10) == 0 && get(sdi, 0x14) == 0);

    S3cUsbDevice usb;
    usb.reset();
    assert(get(usb, 0x16c) == 0x04 && usb.write(0x15c, 0x3) == S3cAccess::ok && get(usb, 0x15c) == 0x3);
    assert(get(usb, 0x1c0) == 0);
    uint32_t v;
    assert(usb.read(0x100, v) == S3cAccess::unknown);


    S3cDma dma;
    dma.reset();
    dma.write(0x80, 0x3000a000);
    dma.write(0x90, (1u << 23) | 0x400);
    assert(get(dma, 0x80) == 0x3000a000 && (get(dma, 0x94) & 0xfffff) == 0);
    // Turning it on loads the current count and source.
    assert(dma.write(0xa0, 2) == S3cAccess::ok && get(dma, 0xa0) == 2);
    assert((get(dma, 0x94) & 0xfffff) == 0x400 && get(dma, 0x98) == 0x3000a000);
    dma.write(0xd0, 0x10);
    assert(dma.write(0xe0, 3) == S3cAccess::unsupported);  // software trigger

    S3cIis iis;
    iis.reset();
    iis.write(0x04, 0x249);
    assert(get(iis, 0x04) == 0x249 && (get(iis, 0x0c) & 0xfff) == 0);

    S3cIic iic;
    iic.reset();
    iic.write(0x00, 0xaf);
    iic.write(0x0c, 0x30);
    iic.write(0x04, 0xf0);  // master transmit start
    assert((get(iic, 0x00) & 0x10) && !(get(iic, 0x04) & 1));
    iic.write(0x0c, 0x22);
    iic.write(0x00, 0xaf & ~0x10);  // the next byte
    assert((get(iic, 0x00) & 0x10) && iic.bytes == 2);

    S3cRtc rtc;
    rtc.reset({6, 1, 1, 7, 12, 0, 0});
    assert(rtc.write(0x6c, 0x9) == S3cAccess::ok && get(rtc, 0x6c) == 0x9);
}

// The ADC: a normal conversion of a channel takes 5 ADC clocks, then ECFLG and INT_ADC_S; the touch
// screen's pen-down interrupt in wait-for-interrupt mode and its X and Y conversions.
static void adcTests() {
    S3cIntc intc;
    intc.reset();
    intc.intmsk &= ~(1u << s3cIrq::adc);
    intc.intsubmsk &= ~((1u << s3cSubIrq::adc) | (1u << s3cSubIrq::tc));
    S3cAdc adc;
    adc.reset();
    adc.channel[2] = 0x123;
    // Prescaler 49: 250 PCLK ticks.
    adc.write(0x00, 0x4000 | 49 << 6 | 2 << 3 | 1, intc);
    assert((get(adc, 0x00) & 0x8001) == 0 && adc.ticksToEvent() == 250);
    adc.advance(249, intc);
    assert(!(get(adc, 0x00) & 0x8000) && !intc.irq());
    adc.advance(1, intc);
    assert((get(adc, 0x00) & 0x8000) && (get(adc, 0x0c) & 0x83ff) == 0x8123 && adc.channelsRead == 4);
    assert(adc.firstChannel == 2 && (intc.subsrcpnd & (1u << s3cSubIrq::adc)) && intc.irq());
    intc.write(0x18, 1u << s3cSubIrq::adc);
    intc.write(0x00, 1u << s3cIrq::adc);
    intc.write(0x10, 1u << s3cIrq::adc);
    assert(!intc.irq());

    // Waiting for the stylus to go down (0xd3): no interrupt while it's up, one as it goes down.
    adc.write(0x04, 0xd3, intc);
    assert(!(intc.subsrcpnd & (1u << s3cSubIrq::tc)));
    adc.setStylus(true, 0x150, 0x2a0, intc);
    assert((intc.subsrcpnd & (1u << s3cSubIrq::tc)) && (get(adc, 0x14) & 1) && !(get(adc, 0x0c) & 0x8000));
    intc.write(0x18, 1u << s3cSubIrq::tc);
    // Going back to the wait with the stylus still down makes another; moving it while waiting doesn't.
    adc.write(0x04, 0x0c, intc);  // auto X and Y
    adc.write(0x04, 0xd3, intc);
    assert(intc.subsrcpnd & (1u << s3cSubIrq::tc));
    intc.write(0x18, 1u << s3cSubIrq::tc);
    adc.setStylus(true, 0x151, 0x2a0, intc);
    assert(!(intc.subsrcpnd & (1u << s3cSubIrq::tc)));
    // Auto conversion: ADCDLY then the conversion, X in ADCDAT0 and Y in ADCDAT1.
    adc.write(0x08, 1000, intc);
    adc.write(0x04, 0x0c, intc);
    adc.write(0x00, 0x4000 | 49 << 6 | 7 << 3 | 1, intc);
    assert(adc.ticksToEvent() == 1250);
    adc.advance(1250, intc);
    assert((get(adc, 0x0c) & 0x3ff) == 0x151 && (get(adc, 0x10) & 0x3ff) == 0x2a0 && (get(adc, 0x0c) & 0x4000));
    // Separate Y.
    adc.setStylus(true, 0x100, 0x80, intc);
    adc.write(0x04, 0x6a, intc);
    adc.write(0x00, 0x4000 | 49 << 6 | 1, intc);
    adc.advance(2000, intc);
    assert((get(adc, 0x10) & 0x3ff) == 0x80 && (get(adc, 0x0c) & 0x3ff) == 0x151);
    // Waiting for up (0x1d3): the interrupt comes as the stylus lifts, with the up history bit.
    intc.write(0x18, 0x7fff);
    adc.write(0x04, 0x1d3, intc);
    assert(!(intc.subsrcpnd & (1u << s3cSubIrq::tc)));
    adc.setStylus(false, 0, 0, intc);
    assert((intc.subsrcpnd & (1u << s3cSubIrq::tc)) && (get(adc, 0x14) & 2) && (get(adc, 0x10) & 0x8000));
}

// DMA transfers: hardware requests move units through the bus, the count runs down, the interrupt and an
// auto reload at the end, RELOAD turning the channel off, bursts, and the IIS request routing.
static void dmaTests() {
    S3cIntc intc;
    intc.reset();
    S3cDma dma;
    dma.reset();
    std::vector<uint32_t> moved;
    uint32_t reads = 0;
    dma.bus = [&](uint32_t pa, unsigned size, bool write, uint32_t& value) {
        if (!write) {
            assert(size == 2);
            value = pa & 0xffff;
            ++reads;
            return true;
        }
        assert(pa == 0x55000010);
        moved.push_back(value);
        return true;
    };
    // Channel 2 as wavedev.dll sets it for playback: SDRAM (AHB, increment) to IISFIFO (APB, fixed), HW
    // source I2SSDO, halfwords, interrupt at the end, auto reload, count 3.
    dma.write(0x80, 0x3000a000);
    dma.write(0x84, 0);
    dma.write(0x88, 0x55000010);
    dma.write(0x8c, 3);
    dma.write(0x90, 0xa0900000 | 3);
    assert(dma.channelFor(S3cDmaRequest::iisOut) == -1);
    dma.write(0xa0, 2);
    assert(dma.channelFor(S3cDmaRequest::iisOut) == 2 && dma.channelFor(S3cDmaRequest::iisIn) == -1);
    assert(dma.unitBytes(2) == 2 && dma.remaining(2) == 3);
    // The next run starts elsewhere, as the driver flips its buffers.
    dma.write(0x80, 0x3000b8c0);
    dma.service(2, intc);
    dma.service(2, intc);
    assert(moved.size() == 2 && moved[0] == 0xa000 && moved[1] == 0xa002 && get(dma, 0x98) == 0x3000a004);
    assert(!(intc.srcpnd & (1u << 19)));
    dma.service(2, intc);
    assert((intc.srcpnd & (1u << 19)) && dma.on(2) && dma.remaining(2) == 3 && get(dma, 0x98) == 0x3000b8c0);
    dma.service(2, intc);
    assert(moved.back() == 0xb8c0);
    // RELOAD: the channel goes off at the end, keeping its count of 0.
    dma.write(0x90, 0xa0d00000 | 1);
    dma.service(2, intc);
    dma.service(2, intc);
    dma.service(2, intc);
    assert(!dma.on(2) && dma.channelFor(S3cDmaRequest::iisOut) == -1 && (get(dma, 0x94) & 0xfffff) == 0);
    // A burst moves four units a request.
    dma.write(0x90, 0xb0900000 | 8);
    dma.write(0xa0, 2);
    moved.clear();
    dma.service(2, intc);
    assert(moved.size() == 4 && dma.remaining(2) == 4);
    dma.write(0xa0, 4);  // stop
    assert(!dma.on(2));
    // Channel 1 source 2 and channel 2 source 1 are I2SSDI.
    dma.write(0x50, 0xa2900c60);
    dma.write(0x60, 2);
    assert(dma.channelFor(S3cDmaRequest::iisIn) == 1);
    dma.write(0x60, 4);
    dma.write(0x90, 0xa1900c60);
    dma.write(0xa0, 2);
    assert(dma.channelFor(S3cDmaRequest::iisIn) == 2);
    // A unit nothing answers is dropped and counted.
    dma.bus = [](uint32_t, unsigned, bool, uint32_t&) { return false; };
    dma.service(2, intc);
    assert(dma.busErrors == 1 && dma.remaining(2) == 0xc5f);
}

// IIS: the FIFOs through IISFIFO and IISFCON's counts, a transmitting frame taking left then right, an
// underrun, a receiving frame, overrun, and the DMA request flags.
static void iisTests() {
    S3cIis iis;
    iis.reset();
    // wavedev.dll's set-up: MPLLin, 16 bits, 384fs, 32fs bit clock.
    iis.write(0x04, 0x20d);
    assert(iis.mpllIn() && iis.frameDivider() == 384);
    iis.push(1);  // FIFO off: dropped
    assert((get(iis, 0x0c) & 0xfff) == 0);
    iis.write(0x0c, 0xa000);  // transmit FIFO on, DMA access
    iis.write(0x00, 0x21);
    assert(iis.txDmaOn() && iis.txWantsDma() && !iis.transmitting());
    iis.write(0x04, 0x20d | 0x80);
    assert(iis.transmitting());
    for (uint16_t v = 1; v <= 32; ++v) iis.push(v);
    assert(((get(iis, 0x0c) >> 6) & 0x3f) == 32 && !iis.txWantsDma() && !(get(iis, 0x00) & 0x80));
    int16_t out[2], in[2] = {7, -7};
    assert(iis.frame(out, in) && out[0] == 1 && out[1] == 2 && ((get(iis, 0x0c) >> 6) & 0x3f) == 30);
    for (int i = 0; i < 15; ++i) iis.frame(out, in);
    assert(out[0] == 31 && out[1] == 32 && iis.underruns == 0);
    assert(iis.frame(out, in) && out[0] == 0 && out[1] == 0 && iis.underruns == 1);
    // TX idle holds the transmitter.
    iis.write(0x00, 0x29);
    assert(!iis.transmitting() && !iis.frame(out, in));
    // Receive: the codec's samples go in the receive FIFO, which IISFIFO and DMA drain.
    iis.write(0x0c, 0x5000);
    iis.write(0x00, 0x11);
    iis.write(0x04, 0x20d | 0x40);
    assert(iis.receiving() && !iis.rxWantsDma());
    assert(!iis.frame(out, in) && iis.framesReceived == 1 && iis.rxWantsDma() && (get(iis, 0x00) & 0x40));
    assert(get(iis, 0x10) == 7 && get(iis, 0x10) == 0xfff9 && !iis.rxWantsDma());
    for (int i = 0; i < 17; ++i) iis.frame(out, in);
    assert((get(iis, 0x0c) & 0x3f) == 32 && iis.overruns == 1);
    // A FIFO turned off empties.
    iis.write(0x0c, 0x4000);
    assert((get(iis, 0x0c) & 0x3f) == 0);
}

// The UDA1380 over IIC: wavedev.dll's register writes (address 0x34, register, high, low), the next register
// following on, reads, the reset register, and the DAC and ADC paths with their volume and mutes.
static void codecTests() {
    Uda1380 codec;
    S3cIic iic;
    iic.reset();
    iic.device = &codec;
    auto send = [&](std::initializer_list<uint8_t> bytes) {
        bool first = true;
        for (uint8_t b : bytes) {
            iic.write(0x0c, b);
            if (first) iic.write(0x04, 0xf0);  // start with the address byte
            else iic.write(0x00, 0xaf & ~0x10);
            first = false;
        }
        iic.write(0x04, 0xd0);  // stop
    };
    send({0x34, 0x02, 0x04, 0x00});
    assert(codec.reg(0x02) == 0x400 && codec.writes == 1);
    send({0x34, 0x10, 0x00, 0x00, 0xff, 0x00});  // master volume, then mixer volume
    assert(codec.reg(0x10) == 0 && codec.reg(0x11) == 0xff00 && codec.writes == 3);
    send({0x36, 0x02, 0x00, 0x00});  // another address
    assert(codec.reg(0x02) == 0x400);
    // Muted at reset (MTM, MT1): the DAC path needs both cleared.
    assert(!codec.dacOn());
    send({0x34, 0x13, 0x08, 0x00});
    assert(codec.dacOn());
    int16_t l = 1000, r = -1000;
    codec.play(l, r);
    assert(l == 1000 && r == -1000);
    // 24 steps of 0.25 dB halve it; left is the high byte.
    send({0x34, 0x10, 24, 48});
    l = 1000, r = 1000;
    codec.play(l, r);
    assert(l == 500 && r == 250);
    send({0x34, 0x10, 0xfc, 0});
    l = 1000;
    codec.play(l, r);
    assert(l == 0);
    // A read: register pointer written, then a repeated start in master receive.
    iic.write(0x0c, 0x34);
    iic.write(0x04, 0xf0);
    iic.write(0x0c, 0x02);
    iic.write(0x00, 0xaf & ~0x10);
    iic.write(0x0c, 0x35);
    iic.write(0x04, 0xb0);
    iic.write(0x00, 0xaf & ~0x10);
    const uint32_t high = get(iic, 0x0c);
    iic.write(0x00, 0xaf & ~0x10);
    assert(high == 0x04 && get(iic, 0x0c) == 0x00);
    iic.write(0x04, 0x90);
    // ADC: powered left and unmuted; the decimator volume, +6 dB doubling.
    assert(!codec.adcOn());
    send({0x34, 0x02, 0x04, 0x04});
    assert(codec.adcOn());
    send({0x34, 0x20, 12, 0});
    int16_t rec[2];
    codec.record(1000, rec);
    assert(rec[0] == 2000 && rec[1] == 1000);
    codec.record(30000, rec);
    assert(rec[0] == 32767);
    send({0x34, 0x21, 0x80, 0x00});
    assert(!codec.adcOn());
    send({0x34, 0x7f, 0x00, 0x00});
    assert(codec.reg(0x02) == 0 && codec.reg(0x13) == 0x4808);
}

int main() {
    dmaTests();
    iisTests();
    codecTests();
    adcTests();
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
    quietBlockTests();
    std::puts("s3c2442 blocks: ok");
    return 0;
}
