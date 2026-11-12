#include <cassert>
#include <cstdio>
#include <vector>

#include "imx1_i2c_read_window.h"

namespace {
constexpr uint32_t base = 0x00217000;
uint32_t read(Imx1I2c& c, unsigned offset) {
    uint32_t value = 0;
    assert(c.access(base + offset, 4, false, value) == Imx1I2cAccess::ok);
    return value;
}
void write(Imx1I2c& c, unsigned offset, uint32_t value) {
    assert(c.access(base + offset, 4, true, value) == Imx1I2cAccess::ok);
}
void finish(Imx1I2c& c) {
    c.advance(269);
    assert(!(read(c, 12) & 0x80));
    c.advance(1);
    assert(c.clockStatus() == Imx1I2cAccess::ok);
    assert((read(c, 12) & 0x83) == 0x82);
    write(c, 12, 0);
}
struct Events {
    std::vector<Imx1I2cTransfer> receives;
    static void observe(void* context, const Imx1I2cTransfer& t, Imx1I2cResponse r) {
        if (t.phase == Imx1I2cPhase::receive) {
            assert(r == Imx1I2cResponse::ack);
            static_cast<Events*>(context)->receives.push_back(t);
        }
    }
};
Imx1I2cReadWindow::Image image() {
    Imx1I2cReadWindow::Image bytes{};
    for (unsigned i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(i ^ 0x5a);
    return bytes;
}
void transaction(unsigned offset, unsigned count) {
    auto source = image();
    Imx1I2cReadWindow window(source);
    source.fill(0); // The endpoint owns its image independently of the caller.
    Events events; Imx1I2c c;
    c.setSingleMasterScenario(true); c.setBus(&window); c.setObserver(Events::observe, &events);
    write(c, 8, 0x80); write(c, 8, 0xb0);
    write(c, 16, 0xa0); finish(c);
    write(c, 16, offset); finish(c);
    write(c, 8, 0xb4);
    write(c, 16, 0xa1); finish(c);
    write(c, 8, count == 1 ? 0xa8 : 0xa0);
    assert(read(c, 16) == 0xa1); // Dummy read starts the first receive.
    for (unsigned i = 0; i < count; ++i) {
        finish(c);
        if (i + 1 == count) write(c, 8, 0x88);
        else if (i + 2 == count) write(c, 8, 0xa8);
        assert(read(c, 16) == uint8_t((offset + i) ^ 0x5a));
    }
    assert(events.receives.size() == count);
    for (unsigned i = 0; i < count; ++i) {
        assert(events.receives[i].byte == uint8_t((offset + i) ^ 0x5a));
        assert(events.receives[i].acknowledge == (i + 1 < count));
    }
    c.advance(100000);
    assert(read(c, 16) == uint8_t((offset + count - 1) ^ 0x5a));
    assert(events.receives.size() == count && !c.active());
}
void select(Imx1I2cReadWindow& w, uint8_t offset) {
    w.signal({Imx1I2cPhase::start});
    assert(w.transfer({Imx1I2cPhase::address, 0x50, 0xa0, false}).response == Imx1I2cResponse::ack);
    assert(w.transfer({Imx1I2cPhase::transmit, 0x50, offset, false}).response == Imx1I2cResponse::ack);
    w.signal({Imx1I2cPhase::repeatedStart});
    assert(w.transfer({Imx1I2cPhase::address, 0x50, 0xa1, true}).response == Imx1I2cResponse::ack);
}
void unsupported() {
    Imx1I2cReadWindow w(image());
    const Imx1I2cTransfer receive{Imx1I2cPhase::receive, 0x50, 0, true, true};
    const auto unknown = [&](const Imx1I2cTransfer& t) {
        assert(w.transfer(t).response == Imx1I2cResponse::unknown);
    };
    unknown(receive); // No START or address.
    w.signal({Imx1I2cPhase::start});
    unknown({Imx1I2cPhase::address, 0x51, 0xa2, false});
    w.signal({Imx1I2cPhase::start});
    unknown({Imx1I2cPhase::address, 0x50, 0xa1, true}); // No offset/repeated START.
    w.signal({Imx1I2cPhase::start});
    unknown({Imx1I2cPhase::address, 0x50, 0xa2, false}); // Inconsistent raw address.
    w.signal({Imx1I2cPhase::start});
    w.signal({Imx1I2cPhase::repeatedStart});
    unknown({Imx1I2cPhase::address, 0x50, 0xa1, true});
    select(w, 255);
    const auto last = w.transfer(receive);
    assert(last.response == Imx1I2cResponse::ack && last.byte == (255 ^ 0x5a));
    unknown(receive); // The window ends at 256, it never wraps.
    select(w, 7);
    assert(w.transfer({Imx1I2cPhase::receive, 0x50, 0, true, false}).byte == (7 ^ 0x5a));
    unknown(receive); // Master NACK terminates the read.
    select(w, 0);
    w.signal({Imx1I2cPhase::stop});
    unknown(receive);
    w.signal({Imx1I2cPhase::start});
    assert(w.transfer({Imx1I2cPhase::address, 0x50, 0xa0, false}).response == Imx1I2cResponse::ack);
    assert(w.transfer({Imx1I2cPhase::transmit, 0x50, 0, false}).response == Imx1I2cResponse::ack);
    unknown({Imx1I2cPhase::transmit, 0x50, 0xff, false}); // Data writes are unsupported.
    select(w, 0);
    assert(w.transfer(receive).byte == 0x5a); // Unsupported writes and signals preserve bytes.
}
}
int main() {
    transaction(0, 256); transaction(1, 3); transaction(254, 2); transaction(255, 1);
    unsupported();
    std::puts("PASS: owned 256-byte I2C window, controller reads, offsets, bounds, NACK/STOP and unsupported protocols");
}
