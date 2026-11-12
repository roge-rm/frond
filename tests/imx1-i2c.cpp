#include <cassert>
#include <cstdio>
#include <vector>

#include "imx1_bootstrap.h"

namespace {
constexpr uint32_t base = 0x00217000;
uint32_t read(Imx1I2c& c, unsigned offset) {
    uint32_t value = 0xdeadbeef;
    assert(c.access(base + offset, 4, false, value) == Imx1I2cAccess::ok);
    return value;
}
void write(Imx1I2c& c, unsigned offset, uint32_t value) {
    assert(c.access(base + offset, 4, true, value) == Imx1I2cAccess::ok);
}
struct Bus : Imx1I2cBus {
    std::vector<Imx1I2cTransfer> calls, signals;
    void signal(const Imx1I2cTransfer& t) override { signals.push_back(t); }
    Imx1I2cResponse response = Imx1I2cResponse::ack;
    uint8_t next = 0x40;
    Imx1I2cReply transfer(const Imx1I2cTransfer& t) override {
        calls.push_back(t);
        return {response, next++};
    }
};
struct Events {
    Imx1I2c* controller = nullptr;
    std::vector<Imx1I2cTransfer> calls;
    std::vector<uint32_t> statuses;
    std::vector<Imx1I2cAccess> failures;
    static void observe(void* context, const Imx1I2cTransfer& t, Imx1I2cResponse) {
        auto& events = *static_cast<Events*>(context);
        events.calls.push_back(t);
        events.statuses.push_back(events.controller->statusValue());
        events.failures.push_back(events.controller->clockStatus());
    }
};
void enable(Imx1I2c& c, Bus* bus = nullptr) {
    c.setSingleMasterScenario(true);
    c.setBus(bus);
    write(c, 8, 0x80);
    write(c, 8, 0xb0);
}
void resetAndDecode() {
    Imx1I2c c;
    for (unsigned offset = 0; offset <= 16; offset += 4) {
        assert(Imx1I2c::registerName(base + offset));
        assert(read(c, offset) == (offset == 12 ? 0x81u : 0));
        for (auto size : {1u, 2u, 8u}) {
            uint32_t value = 0;
            assert(c.access(base + offset, size, true, value) == Imx1I2cAccess::width);
        }
        uint32_t value = 0;
        assert(c.access(base + offset + 1, 4, true, value) == Imx1I2cAccess::width);
        value = 0x100;
        assert(c.access(base + offset, 4, true, value) == Imx1I2cAccess::ok);
    }
    uint32_t value = 0;
    assert(c.access(base + 20, 4, true, value) == Imx1I2cAccess::unmapped);
    value = 1;
    assert(c.access(base, 4, true, value) == Imx1I2cAccess::ok);
    assert(read(c, 0) == 0);
    write(c, 0, 0xa4); write(c, 4, 0x3f); write(c, 16, 0x5a);
    assert(read(c, 0) == 0xa4 && read(c, 4) == 0x3f && read(c, 16) == 0x5a);
    write(c, 8, 0); assert(read(c, 12) == 0x81 && read(c, 16) == 0);
    assert(read(c, 0) == 0xa4 && read(c, 4) == 0x3f);
    value = 0xb0;
    assert(c.access(base + 8, 4, true, value) == Imx1I2cAccess::sideEffect);
    assert(read(c, 8) == 0);
    write(c, 8, 0x80);
    value = 0xdeadbeef;
    assert(c.access(base + 12, 4, false, value) == Imx1I2cAccess::externalInput && value == 0xdeadbeef);
    value = 0xb0;
    assert(c.access(base + 8, 4, true, value) == Imx1I2cAccess::externalInput);
    assert(read(c, 8) == 0x80);
}
void timingAndResponses() {
    constexpr uint16_t divisors[] = {
        30,32,36,42,48,52,60,72,80,88,104,128,144,160,192,240,
        288,320,384,480,576,640,768,960,1152,1280,1536,1920,2304,2560,3072,3840,
        22,24,26,28,32,36,40,44,48,56,64,72,80,96,112,128,
        160,192,224,256,320,384,448,512,640,768,896,1024,1280,1536,1792,2048
    };
    for (unsigned divider = 0; divider < 64; ++divider) {
        Imx1I2c a, b; Bus ba, bb; enable(a, &ba); enable(b, &bb);
        write(a, 4, divider); write(b, 4, divider);
        assert(a.clockDivider() == divisors[divider]);
        write(a, 16, 0xa4); write(b, 16, 0xa4);
        for (unsigned n = 0; n < 100; ++n) assert(read(a, 12) == 0x21);
        assert(ba.calls.empty());
        const uint32_t cycles = 9 * divisors[divider];
        a.advance(cycles - 1);
        for (unsigned n = 0; n < cycles - 1; ++n) b.advance(1);
        assert(read(a, 12) == read(b, 12) && ba.calls.empty() && bb.calls.empty());
        a.advance(1); b.advance(1);
        assert(read(a, 12) == 0xa2 && read(b, 12) == 0xa2);
        assert(ba.calls.size() == 1 && bb.calls.size() == 1);
        assert(ba.calls[0].address == 0x52 && ba.calls[0].byte == 0xa4 && !ba.calls[0].read);
        assert(!a.interruptOutput()); write(a, 8, 0xf0); assert(a.interruptOutput());
        write(a, 12, 0xff & ~8); assert(a.interruptOutput());
        write(a, 12, 0); assert(!a.interruptOutput() && read(a, 12) == 0xa0);
        write(a, 16, 0x27); assert(read(a, 12) == 0x20);
        a.advance(cycles); assert(ba.calls.back().phase == Imx1I2cPhase::transmit && ba.calls.back().byte == 0x27);
    }
    Imx1I2c unknown; Events unknownEvents; unknownEvents.controller = &unknown;
    unknown.setObserver(Events::observe, &unknownEvents);
    enable(unknown); write(unknown, 16, 0xa5); unknown.advance(270);
    assert(unknownEvents.statuses.back() == 0x21);
    assert(unknownEvents.failures.back() == Imx1I2cAccess::externalInput);
    assert(unknown.clockStatus() == Imx1I2cAccess::externalInput);
    assert(read(unknown, 12) == 0x21); // No invented completion or NACK.
    Bus unresolved; unresolved.response = Imx1I2cResponse::unknown;
    Imx1I2c stalled; enable(stalled, &unresolved); write(stalled, 16, 0xa0);
    stalled.advance(270); stalled.advance(100000);
    assert(unresolved.calls.size() == 1 && stalled.clockStatus() == Imx1I2cAccess::externalInput);
    Bus absent; absent.response = Imx1I2cResponse::nack;
    Imx1I2c nack; enable(nack, &absent); write(nack, 16, 0xa5); nack.advance(270);
    assert(nack.clockStatus() == Imx1I2cAccess::ok && read(nack, 12) == 0xa3);
    write(nack, 8, 0x90); assert(read(nack, 12) == 0x83);
    Imx1I2c collision; absent.response = Imx1I2cResponse::arbitrationLost;
    enable(collision, &absent); write(collision, 16, 0xa4); collision.advance(270);
    assert(collision.clockStatus() == Imx1I2cAccess::sideEffect && read(collision, 12) == 0x21);
}
void receiveAndControls() {
    Bus bus; Events events; Imx1I2c c; events.controller = &c;
    c.setObserver(Events::observe, &events); enable(c, &bus);
    assert(events.calls.size() == 1 && events.calls[0].phase == Imx1I2cPhase::start);
    write(c, 16, 0xa5); c.advance(270);
    assert(events.statuses.back() == 0xa2 && events.failures.back() == Imx1I2cAccess::ok);
    write(c, 12, 0); write(c, 8, 0xa0);
    assert(read(c, 16) == 0xa5); // Dummy read starts the first receive.
    uint32_t value = 0;
    assert(c.access(base + 16, 4, false, value) == Imx1I2cAccess::sideEffect);
    assert(c.access(base + 8, 4, true, value) == Imx1I2cAccess::sideEffect);
    value = 1;
    assert(c.access(base + 4, 4, true, value) == Imx1I2cAccess::sideEffect);
    assert(read(c, 4) == 0);
    c.advance(270); assert(bus.calls.back().acknowledge);
    assert(events.statuses.back() == 0xa2 && events.calls.back().byte == 0x41);
    write(c, 12, 0); write(c, 8, 0xa8);
    assert(read(c, 16) == 0x41); // Reading the next-to-last byte schedules NACK for the last.
    c.advance(270); assert(!bus.calls.back().acknowledge);
    write(c, 8, 0x88); // STOP before reading the final byte.
    assert(events.calls.back().phase == Imx1I2cPhase::stop && !(read(c, 12) & 0x20));
    const auto count = bus.calls.size(); assert(read(c, 16) == 0x42);
    c.advance(100000); assert(bus.calls.size() == count);
    write(c, 8, 0xb0); write(c, 16, 0xa4); c.advance(270);
    write(c, 8, 0xb4);
    assert(read(c, 8) == 0xb0 && events.calls.back().phase == Imx1I2cPhase::repeatedStart);
    write(c, 16, 0xa5); c.advance(270); assert(bus.calls.back().read);
    write(c, 8, 0x90); write(c, 8, 0x94); // Repeated START without mastership loses arbitration.
    assert(read(c, 8) == 0x90 && (read(c, 12) & 0x12) == 0x12);
    write(c, 12, 0x12); assert((read(c, 12) & 0x12) == 0x12);
    write(c, 12, 0x10); assert((read(c, 12) & 0x12) == 0x10);
    write(c, 12, 0); assert(!(read(c, 12) & 0x12));
    write(c, 8, 0); assert(read(c, 12) == 0x81 && !c.interruptOutput());
}
uint32_t access(Imx1Bootstrap& b, uint32_t address, bool write, uint32_t value,
                Imx1BootstrapAccess expected = Imx1BootstrapAccess::ok) {
    assert(b.access(address, 4, write, value) == expected); return value;
}
void setup(Imx1Bootstrap& b, Bus& bus) {
    b.setI2cScenario(true); b.setI2cBus(&bus);
    access(b, 0x0021c020, true, 0); // GPIO-A primary peripheral mux, DDIR reset inputs.
    access(b, base + 8, true, 0x80);
    access(b, base + 8, true, 0xb0);
}
void bootstrap() {
    Imx1Bootstrap b; Bus bus;
    // PERCLK1 divides by 1 while BCLK divides by 12; I2C must still use HCLK.
    access(b, 0x0021b020, true, 0x000b00b0);
    setup(b, bus);
    assert(bus.signals.size() == 1);
    access(b, 0x00223008, true, 39); // Known inactive request derived from I2CR/I2SR.
    access(b, base + 16, true, 0xa4);
    uint32_t sentinel = 0xdeadbeef;
    assert(!b.instructionWord(base + 16, sentinel) && sentinel == 0);
    assert(bus.calls.empty());
    b.advanceFastBusCycles(269); assert(bus.calls.empty());
    b.advanceFastBusCycles(1); assert(bus.calls.size() == 1);
    assert(b.clockStatus() == Imx1BootstrapAccess::ok);
    access(b, base + 8, true, 0xf0, Imx1BootstrapAccess::sideEffect);
    assert(access(b, base + 8, false, 0) == 0xb0);
    access(b, 0x0022300c, true, 39);
    access(b, base + 8, true, 0xf0);
    access(b, 0x00223008, true, 39, Imx1BootstrapAccess::sideEffect);
    access(b, base + 12, true, 0);
    access(b, 0x00223008, true, 39);
    access(b, base + 16, true, 0x55); b.advanceFastBusCycles(270);
    assert(b.clockStatus() == Imx1BootstrapAccess::sideEffect);
    Imx1Bootstrap mux; mux.setI2cScenario(true);
    access(mux, base + 8, true, 0x80);
    access(mux, base + 8, true, 0xb0, Imx1BootstrapAccess::sideEffect);
    assert(access(mux, base + 8, false, 0) == 0x80);
    access(b, 0x0021c020, true, 1u << 15, Imx1BootstrapAccess::sideEffect);
}
}
int main() {
    resetAndDecode(); timingAndResponses(); receiveAndControls(); bootstrap();
    std::puts("PASS: I2C register/reset guards, HCLK timing, bus responses, receive priming, START/STOP and source 39");
}
