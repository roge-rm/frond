// i.MX1 UART model, MC9328MX1RM chapter 27: reset values, software reset, baud timing, FIFOs,
// flags, interrupt outputs, modem pins, refusals, state, and both UARTs behind the bootstrap.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "imx1_bootstrap.h"
#include "machine_state_archive.h"

namespace {
using A = Imx1UartAccess;
enum : uint32_t { urxd = 0, utxd = 0x40, ucr1 = 0x80, ucr2 = 0x84, ucr3 = 0x88, ucr4 = 0x8c, ufcr = 0x90,
                  usr1 = 0x94, usr2 = 0x98, uesc = 0x9c, utim = 0xa0, ubir = 0xa4, ubmr = 0xa8, ubrc = 0xac,
                  bipr1 = 0xb0, bmpr4 = 0xcc, uts = 0xd0 };
A put(Imx1Uart& u, uint32_t offset, uint32_t value) { return u.access(offset, 4, true, value); }
uint32_t get(Imx1Uart& u, uint32_t offset) {
    uint32_t value = 0xdeadbeef;
    const auto status = u.access(offset, 4, false, value);
    if (status != A::ok) std::fprintf(stderr, "read %02x refused\n", offset);
    assert(status == A::ok);
    return value;
}
struct Sent { std::vector<uint8_t> bytes; unsigned infrared = 0; };
void collect(void* context, uint8_t character, bool infrared) {
    auto& sent = *static_cast<Sent*>(context);
    sent.bytes.push_back(character); sent.infrared += infrared;
}
// What the Zodiac's IrDA library programs: 8N1, PERCLK1 / 1, ratio 48 / 10000, RTS ignored.
void open(Imx1Uart& u, uint32_t control1 = 0x205) {
    assert(put(u, ucr1, 4) == A::ok && put(u, ucr2, 0) == A::ok);
    u.advance(20);
    assert(put(u, ufcr, 0x1290) == A::ok && put(u, ucr4, 0x6041) == A::ok && put(u, ucr3, 0x1800) == A::ok);
    assert(put(u, ucr1, control1) == A::ok && put(u, ubir, 0x2f) == A::ok && put(u, ubmr, 0x270f) == A::ok);
    assert(put(u, ucr2, 0x60a7) == A::ok);
}

void resetValues() {
    for (bool modem : {false, true}) {
        Imx1Uart u(modem);
        assert(get(u, ucr1) == 4 && get(u, ucr2) == 1 && get(u, ucr3) == 0 && get(u, ucr4) == 0x8040);
        assert(get(u, ufcr) == 0x0881 && get(u, usr2) == 0x4008 && get(u, uesc) == 0x2b && get(u, utim) == 0);
        assert(get(u, ubir) == 0 && get(u, ubmr) == 0 && get(u, ubrc) == 8 && get(u, uts) == 0x60);
        for (uint32_t offset = utxd; offset < ucr1; offset += 4) assert(get(u, offset) == 0);
        uint32_t value = 0xdeadbeef;
        // RTSS is a pin, the receive data and the presets have no reset value.
        assert(u.access(usr1, 4, false, value) == A::externalInput);
        assert(u.access(urxd, 4, false, value) == A::sideEffect && u.access(bipr1, 4, false, value) == A::sideEffect);
        assert(u.access(uts + 4, 4, false, value) == A::unmapped && value == 0xdeadbeef);
        for (uint32_t size : {1u, 2u, 8u}) assert(u.access(ucr1, size, false, value) == A::width);
        assert(u.access(ucr1 + 1, 4, false, value) == A::width);
        u.setRts(false);
        assert(get(u, usr1) == 0x2000);
        u.setRts(true);
        assert(get(u, usr1) == 0x7000 && put(u, usr1, 0x1000) == A::ok && get(u, usr1) == 0x6000);
        assert(!u.interruptOutputs() && !u.busy());
        assert(put(u, bipr1, 0x1234) == A::ok && get(u, bipr1) == 0x1234 && put(u, bmpr4, 0xffff) == A::ok);
        assert(get(u, bmpr4) == 0xffff);
    }
}

void softwareReset() {
    Imx1Uart u(true);
    Sent sent; u.setPeer(collect, &sent);
    open(u);
    assert(put(u, utxd, 0x31) == A::ok && u.receive(0x32) && (get(u, usr2) & 0x4089) == 0x81);
    // SRST low for four reference clocks: RFDIV 1 here.
    assert(put(u, ucr2, 0x60a6) == A::ok && get(u, ucr2) == 0x60a6 && (get(u, uts) & 1));
    assert(get(u, usr2) == 0x4008 && get(u, uts) == 0x61 && u.idleQuietCycles() == 4);
    assert(put(u, utxd, 0x33) == A::sideEffect && !u.receive(0x34));
    u.advance(3);
    assert(get(u, ucr2) == 0x60a6);
    u.advance(1);
    assert(get(u, ucr2) == 0x60a7 && get(u, uts) == 0x60 && !u.busy());
    u.advance(1000000);
    assert(sent.bytes.empty() && get(u, ucr1) == 0x205 && get(u, ufcr) == 0x1290);
    // Writing 1 does nothing, and the default RFDIV 5 gives twenty PERCLK1 cycles.
    Imx1Uart v;
    assert(put(v, ucr2, 1) == A::ok && !v.busy() && put(v, ucr2, 0) == A::ok && v.idleQuietCycles() == 20);
    v.advance(19); assert(get(v, ucr2) == 0);
    v.advance(1); assert(get(v, ucr2) == 1);
    // The reset can't run without the module clocks.
    assert(put(v, ucr1, 0) == A::ok && put(v, ucr2, 0) == A::sideEffect);
}

void transmit() {
    Imx1Uart u(true);
    Sent sent; u.setPeer(collect, &sent);
    open(u);
    u.advance(4);
    u.setRts(false);
    assert(get(u, usr1) == 0x2040 && get(u, usr2) == 0x4008);
    // A bit time is 16 * 10000 / 48 PERCLK1 cycles. The character loads on the first edge and takes ten more.
    assert(put(u, utxd, 0x141) == A::reservedBits && put(u, utxd, 0x41) == A::ok);
    assert(get(u, usr2) == 0 && u.idleQuietCycles() == 3334 - 4);
    u.advance(3329);
    assert(get(u, usr2) == 0 && u.idleQuietCycles() == 1);
    u.advance(1);
    assert(get(u, usr2) == 0x4000 && sent.bytes.empty() && u.idleQuietCycles() == 33333);
    u.advance(33332);
    assert(sent.bytes.empty());
    u.advance(1);
    assert(sent.bytes == std::vector<uint8_t>{0x41} && get(u, usr2) == 0x4008 && !u.busy() && !sent.infrared);
    // One advance gives the same as single steps, and characters follow each other without a gap.
    Imx1Uart a(true), b(true);
    Sent sentA, sentB; a.setPeer(collect, &sentA); b.setPeer(collect, &sentB);
    for (auto* x : {&a, &b}) {
        open(*x, 0x285);
        for (uint32_t n = 0; n < 6; ++n) assert(put(*x, utxd, 0x60 + n) == A::ok);
    }
    assert(get(a, usr2) == 0 && !(a.interruptOutputs() & Imx1Uart::txOutput));
    uint64_t steps = 0;
    while (a.busy()) { const auto quiet = a.idleQuietCycles(); a.advance(quiet); steps += quiet; }
    for (uint64_t n = 0; n < steps; ++n) b.advance(1);
    assert(steps == 203334 && sentA.bytes.size() == 6 && sentA.bytes == sentB.bytes && sentA.infrared == 6);
    assert(sentA.bytes[5] == 0x65 && a.transmittedCharacters() == 6 && !b.busy());
    // TRDY at TXTL 4, TXFE, TXDC and a full FIFO.
    Imx1Uart f(true);
    open(f); f.setRts(true);
    for (uint32_t n = 0; n < 32; ++n) {
        assert((get(f, usr1) & 0x2000) == (n <= 4 ? 0x2000u : 0u) && (get(f, uts) & 0x50) == (n ? 0u : 0x40u));
        assert(put(f, utxd, n) == A::ok);
    }
    assert(get(f, uts) == 0x30 && put(f, utxd, 0) == A::sideEffect);
    // 7 bits, parity and two stop bits: eleven bit times after the load, and bit 7 is dropped.
    Imx1Uart p(true);
    Sent sentP; p.setPeer(collect, &sentP);
    open(p);
    assert(put(p, ucr2, 0x41c7) == A::ok && put(p, utxd, 0xc1) == A::ok);
    p.advance(39999); assert(sentP.bytes.empty());
    p.advance(1); assert(sentP.bytes == std::vector<uint8_t>{0x41});
    // TXEN off drops the character in the shift register and keeps the FIFO.
    assert(put(p, utxd, 1) == A::ok && put(p, utxd, 2) == A::ok);
    p.advance(10000);
    assert(put(p, ucr2, 0x41c3) == A::ok && !p.busy() && (get(p, usr2) & 0x4008) == 0);
    p.advance(1000000);
    assert(sentP.bytes.size() == 1 && put(p, ucr2, 0x41c7) == A::ok);
    p.advance(1000000);
    assert(sentP.bytes == (std::vector<uint8_t>{0x41, 2}) && get(p, usr2) == 0x4008);
    // The baud rate and the divider hold while a character is on its way.
    assert(put(p, utxd, 3) == A::ok && put(p, ubir, 0xf) == A::ok && put(p, ubmr, 0x67) == A::sideEffect);
    assert(put(p, ufcr, 0x1210) == A::sideEffect);
    p.advance(1000000);
    assert(put(p, ubmr, 0x67) == A::ok && put(p, ufcr, 0x1210) == A::ok);
    // Sixteen PERCLK1 cycles a bit at 16 / 104 with RFDIV 2... 16 * 2 * 104 / 16 = 208.
    assert(put(p, utxd, 4) == A::ok);
    p.advance(208 * 12 - 1); assert(sentP.bytes.size() == 3);
    p.advance(1); assert(sentP.bytes.size() == 4);
}

void transmitInterrupts() {
    Imx1Uart u(true);
    open(u);
    // TXFE is held back between writes: one character waits for the shift register, several for the last load.
    assert(put(u, ucr1, 0x245) == A::ok && (u.interruptOutputs() & Imx1Uart::txOutput));
    assert(put(u, utxd, 1) == A::ok && !(u.interruptOutputs() & Imx1Uart::txOutput));
    u.advance(3334);
    assert(get(u, usr2) == 0x4000 && !(u.interruptOutputs() & Imx1Uart::txOutput));
    u.advance(33334);
    assert(u.interruptOutputs() & Imx1Uart::txOutput);
    assert(put(u, utxd, 1) == A::ok && put(u, utxd, 2) == A::ok && !(u.interruptOutputs() & Imx1Uart::txOutput));
    u.advance(3334); assert(!(u.interruptOutputs() & Imx1Uart::txOutput));
    u.advance(33334); assert((get(u, usr2) & 0x4008) == 0x4000 && (u.interruptOutputs() & Imx1Uart::txOutput));
    u.advance(33334);
    // TRDYEN and TCEN.
    assert(put(u, ucr1, 0x2205) == A::ok && u.interruptOutputs() == Imx1Uart::txOutput);
    for (uint32_t n = 0; n < 5; ++n) assert(put(u, utxd, n) == A::ok);
    assert(!u.interruptOutputs());
    u.advance(3334); assert(u.interruptOutputs() == Imx1Uart::txOutput);
    assert(put(u, ucr1, 0x205) == A::ok && put(u, ucr4, 0x6049) == A::ok && !u.interruptOutputs());
    while (u.busy()) u.advance(u.idleQuietCycles());
    assert(u.interruptOutputs() == Imx1Uart::txOutput);
}

void receive() {
    Imx1Uart u(true);
    assert(!u.receive(0x41));
    open(u); u.setRts(true);
    assert(get(u, usr1) == 0x6040 && !u.interruptOutputs());
    // DREN is on, RXTL is 16.
    assert(u.receive(0x41) && get(u, usr2) == 0x4089 && get(u, usr1) == 0x6040);
    assert(u.interruptOutputs() == Imx1Uart::rxOutput && get(u, urxd + 0x3c) == 0x8041);
    assert(get(u, usr2) == 0x4088 && !u.interruptOutputs() && get(u, urxd) == 0x0041);
    assert(put(u, usr2, 0x80) == A::ok && get(u, usr2) == 0x4008 && put(u, urxd, 0) == A::sideEffect);
    for (uint32_t n = 0; n < 16; ++n) {
        assert(!(get(u, usr1) & 0x200));
        assert(u.receive(uint8_t(n)));
    }
    assert((get(u, usr1) & 0x200) && u.ctsLow());
    for (uint32_t n = 16; n < 32; ++n) assert(u.receive(uint8_t(n)));
    // CTSTL 24, a full FIFO, and the 33rd character is lost.
    assert(!u.ctsLow() && (get(u, uts) & 8) && !(get(u, usr2) & 2));
    assert(u.receive(0xee) && (get(u, usr2) & 2) && !(u.interruptOutputs() & Imx1Uart::controlOutput));
    assert(put(u, ucr4, 0x6043) == A::ok && (u.interruptOutputs() & Imx1Uart::controlOutput));
    assert(put(u, usr2, 2) == A::ok && !(get(u, usr2) & 2));
    for (uint32_t n = 0; n < 31; ++n) assert(get(u, urxd) == (0x8000u | n));
    assert(get(u, urxd) == 0xe01f && get(u, urxd) == 0x601f && (get(u, uts) & 0x20));
    // Frame error, parity error only with parity on, BREAK.
    assert(u.receive(0x55, true, true) && get(u, usr1) == 0x6440 && get(u, urxd) == 0xd055);
    assert((u.interruptOutputs() & Imx1Uart::errorOutput) && put(u, usr1, 0x400) == A::ok && !(u.interruptOutputs() & Imx1Uart::errorOutput));
    assert(put(u, ucr2, 0x61a7) == A::ok && u.receive(0x55, false, true) && get(u, usr1) == 0xe040 && get(u, urxd) == 0xc455);
    assert(put(u, usr1, 0x8000) == A::ok && u.receive(0, false, false, true) && (get(u, usr2) & 4) && get(u, urxd) == 0xdc00);
    assert(get(u, usr1) == 0xe440 && put(u, usr1, 0xffff) == A::ok && put(u, usr2, 0xffff) == A::ok && get(u, usr1) == 0x6040);
    assert(put(u, ucr2, 0x60a7) == A::ok);
    // 7 bits clears bit 7; the serial infrared flag only in infrared mode.
    assert(put(u, ucr2, 0x6087) == A::ok && u.receive(0xc1) && get(u, urxd) == 0x8041 && !(get(u, usr2) & 0x100));
    assert(put(u, ucr2, 0x60a7) == A::ok && put(u, ucr1, 0x285) == A::ok && put(u, usr2, 0x80) == A::ok);
    assert(u.receive(0xc1) && get(u, usr2) == 0x4189 && !(u.interruptOutputs() & Imx1Uart::controlOutput));
    assert(put(u, ucr4, 0x6141) == A::ok && u.interruptOutputs() == (Imx1Uart::controlOutput | Imx1Uart::rxOutput));
    assert(put(u, usr2, 0x180) == A::ok && u.interruptOutputs() == Imx1Uart::rxOutput && get(u, urxd) == 0x80c1);
    // IDLE after more than 4 frames of 10 bits with a character waiting, then 8, 16 and 32.
    for (uint32_t icd = 0; icd < 4; ++icd) {
        assert(put(u, ucr1, 0x1285 | icd << 10) == A::ok && put(u, usr2, 0x1000) == A::ok);
        assert(u.receive(0x11) && u.busy() && !(get(u, usr2) & 0x1000));
        const uint64_t quiet = u.idleQuietCycles();
        assert(quiet >= (40u << icd) * 3333 && quiet <= (41u << icd) * 3334);
        u.advance(quiet - 1); assert(!(get(u, usr2) & 0x1000));
        u.advance(1); assert((get(u, usr2) & 0x1000) && !u.busy() && (u.interruptOutputs() & Imx1Uart::rxOutput));
        assert(get(u, urxd) == 0x8011);
    }
    // No character waiting, no idle condition. RXDS follows the receiver enable.
    assert(put(u, usr2, 0x1000) == A::ok && u.receive(0x12) && get(u, urxd) == 0x8012 && !u.busy());
    assert(put(u, ucr3, 0x1840) == A::ok && (u.interruptOutputs() & Imx1Uart::rxOutput));
    assert(put(u, ucr2, 0x60a5) == A::ok && !(get(u, usr1) & 0x40) && !u.receive(0x13) && !(u.interruptOutputs() & Imx1Uart::rxOutput));
}

void modemPins() {
    Imx1Uart u(true), one(false);
    Sent sent; u.setPeer(collect, &sent);
    // The transmitter needs RTS unless IRTS is set.
    open(u);
    assert(put(u, ucr2, 0x20a7) == A::ok && put(u, utxd, 7) == A::externalInput);
    assert(put(u, ucr2, 0x60a7) == A::ok && put(u, utxd, 7) == A::ok && put(u, ucr2, 0x20a7) == A::externalInput);
    assert(put(u, ucr2, 0x60a6) == A::ok);
    u.advance(4);
    u.setRts(false);
    assert(put(u, ucr2, 0x20a7) == A::ok && put(u, utxd, 7) == A::ok && !u.busy());
    u.advance(100000); assert(sent.bytes.empty());
    u.setRts(true);
    assert(get(u, usr1) == 0x7040 && u.busy());
    while (u.busy()) u.advance(u.idleQuietCycles());
    assert(sent.bytes.size() == 1);
    // RTSD on any change, RTSF on the edge RTEC picks: rising, falling, either.
    assert(put(u, ucr1, 0x225) == A::ok && u.interruptOutputs() == Imx1Uart::rtsOutput && put(u, usr1, 0x1000) == A::ok);
    assert(!u.interruptOutputs() && put(u, ucr1, 0x205) == A::ok);
    const struct { uint32_t edge; bool low, flag; } edges[] = {{0, false, true}, {0, true, false}, {1, false, false},
        {1, true, true}, {2, false, true}, {2, true, true}, {3, false, true}};
    for (const auto& e : edges) {
        u.setRts(!e.low);
        assert(put(u, ucr2, 0x20b7 | e.edge << 9) == A::ok && put(u, usr2, 0x10) == A::ok && put(u, usr1, 0x1000) == A::ok);
        u.setRts(e.low);
        assert(bool(get(u, usr2) & 0x10) == e.flag && (get(u, usr1) & 0x1000));
        assert(bool(u.interruptOutputs() & Imx1Uart::rtsOutput) == e.flag);
    }
    // CTS by hand, DTR on UART2 only.
    assert(put(u, ucr2, 0x00a7 | 0x4000) == A::ok && !u.ctsLow() && put(u, ucr2, 0x50a7) == A::ok && u.ctsLow());
    assert(put(u, ucr3, 0x6700) == A::ok && get(u, ucr3) == 0x6700 && put(one, ucr3, 0x2000) == A::reservedBits);
    u.setDtr(false); u.setDtr(true);
    assert(get(u, usr2) & 0x2000 && (u.interruptOutputs() & Imx1Uart::dtrOutput));
    assert(put(u, usr2, 0x2000) == A::ok && !(u.interruptOutputs() & Imx1Uart::dtrOutput));
    u.setDtr(false);
    assert(!(get(u, usr2) & 0x2000));
    one.setDtr(false); one.setDtr(true);
    assert(!(get(one, usr2) & 0x2000) && put(one, ucr3, 0x187f) == A::ok);
}

void refusals() {
    Imx1Uart u(true);
    open(u);
    for (uint32_t bit : {0x4000u, 0x100u, 0x10u, 8u}) assert(put(u, ucr1, 0x205 | bit) == A::sideEffect);
    assert(put(u, ucr1, 0x207) == A::sideEffect && put(u, ucr1, 0x10000) == A::reservedBits);
    assert(put(u, ucr2, 0x68a7) == A::sideEffect && put(u, ucr2, 0x60af) == A::reservedBits);
    assert(put(u, ucr3, 0x80) == A::reservedBits && put(u, ucr4, 0x6051) == A::reservedBits);
    assert(put(u, ucr4, 0x8440) == A::reservedBits && put(u, ucr4, 0x8040) == A::ok);
    for (uint32_t value : {0x0290u, 0x0690u, 0x8690u, 0x12d0u, 0x1291u + 32, 0x1390u})
        assert(put(u, ufcr, value) == A::reservedBits);
    assert(put(u, ufcr, 0x8020) == A::ok && get(u, ufcr) == 0x8020);
    assert(put(u, uesc, 0x100) == A::reservedBits && put(u, uesc, 0x7e) == A::ok && get(u, uesc) == 0x7e);
    assert(put(u, utim, 0x1000) == A::reservedBits && put(u, utim, 0xfff) == A::ok && get(u, utim) == 0xfff);
    assert(put(u, ubrc, 0) == A::sideEffect && put(u, uts, 0x1000) == A::sideEffect && put(u, uts, 0x2000) == A::sideEffect);
    assert(put(u, uts, 0x400) == A::sideEffect);
    assert(put(u, uts, 0x69) == A::ok && get(u, uts) == 0x60);
    // UBMR alone leaves the rate as it was, and UBIR after it is out of order.
    assert(get(u, ucr1) == 0x205 && get(u, ucr2) == 0x60a7);
    assert(put(u, ubmr, 0x1387) == A::ok && get(u, ubmr) == 0x1387 && put(u, utxd, 1) == A::ok);
    assert(u.idleQuietCycles() == 20000 && put(u, ubir, 0x2f) == A::sideEffect);
    // A refused rate change leaves UBMR as it was.
    {
        Imx1Uart v(true);
        open(v);
        assert(put(v, utxd, 1) == A::ok && put(v, ubir, 0x2f) == A::ok);
        assert(put(v, ubmr, 0x1387) == A::sideEffect && get(v, ubmr) == 0x270f && v.idleQuietCycles() == 3334);
        // An idle condition or a frame shorter than the idle time already counted is refused.
        Imx1Uart w(true);
        open(w);
        assert(put(w, ucr1, 0xe05) == A::ok && w.receive(0x11));
        w.advance(100 * 3334);
        assert(put(w, ucr1, 0x205) == A::sideEffect && get(w, ucr1) == 0xe05);
        assert(put(w, ucr1, 0x605) == A::sideEffect && put(w, ucr1, 0xa05) == A::ok);
        assert(put(w, ucr2, 0x6087) == A::ok && put(w, ucr2, 0x60a7) == A::ok && !(get(w, usr2) & 0x1000));
        const uint64_t quiet = w.idleQuietCycles();
        assert(quiet >= 59 * 3333 && quiet <= 61 * 3334);
        w.advance(quiet - 1); assert(!(get(w, usr2) & 0x1000));
        w.advance(1); assert((get(w, usr2) & 0x1000) && !w.busy());
    }
    // Disabled: nothing moves, and the FIFO keeps its character.
    assert(put(u, ucr1, 0x204) == A::ok && !u.busy());
    u.advance(1000000);
    assert(get(u, usr2) == 0 && put(u, ucr1, 0x200) == A::ok && put(u, utxd, 1) == A::sideEffect);
}

void state() {
    Imx1Uart u(true);
    Sent sent; u.setPeer(collect, &sent);
    open(u, 0x1285); u.setRts(true); u.setDtr(false);
    for (uint32_t n = 0; n < 3; ++n) assert(put(u, utxd, 0x70 + n) == A::ok);
    assert(u.receive(0x21) && u.receive(0x22, true) && get(u, urxd) == 0x8021);
    assert(put(u, bipr1, 0x55aa) == A::ok && put(u, ubir, 0x10) == A::ok);
    u.advance(20000);
    std::vector<uint8_t> first, second;
    { StateWriter w(first); u.state(w); }
    Imx1Uart v(true);
    Sent sentV; v.setPeer(collect, &sentV);
    { StateReader r(first.data(), first.size()); v.state(r); assert(r.complete()); }
    { StateWriter w(second); v.state(w); }
    assert(first == second && u.idleQuietCycles() == v.idleQuietCycles());
    for (auto* x : {&u, &v}) while (x->busy()) x->advance(x->idleQuietCycles());
    assert(sent.bytes == sentV.bytes && sent.bytes.size() == 3 && get(u, usr2) == get(v, usr2) && (get(v, usr2) & 0x1000));
    assert(get(v, usr1) == get(u, usr1) && get(v, urxd) == 0xd022 && get(u, urxd) == 0xd022 && get(v, bipr1) == 0x55aa);
    first.clear(); second.clear();
    { StateWriter w(first); u.state(w); } { StateWriter w(second); v.state(w); }
    assert(first == second);
    // UART1 doesn't take UART2's state, nor a FIFO count past the end.
    Imx1Uart one(false);
    { StateReader r(first.data(), first.size()); one.state(r); assert(!r.ok()); }
}

constexpr uint32_t uart1 = 0x00206000, uart2 = 0x00207000, cgm = 0x0021b000, aitc = 0x00223000;
void write(Imx1Bootstrap& soc, uint32_t address, uint32_t value, Imx1BootstrapAccess expected = Imx1BootstrapAccess::ok) {
    const auto actual = soc.access(address, 4, true, value);
    if (actual != expected) std::fprintf(stderr, "%08x=%08x: %s expected %s\n", address, value,
        imx1BootstrapAccessName(actual), imx1BootstrapAccessName(expected));
    assert(actual == expected);
}
uint32_t read(Imx1Bootstrap& soc, uint32_t address) {
    uint32_t value = 0xdeadbeef;
    assert(soc.access(address, 4, false, value) == Imx1BootstrapAccess::ok);
    return value;
}
void bootstrap() {
    using B = Imx1BootstrapAccess;
    Imx1Bootstrap soc;
    write(soc, cgm, 0x57000003); write(soc, cgm + 32, 0);
    uint32_t value = 0;
    for (uint32_t base : {uart1, uart2}) {
        assert(read(soc, base + ucr1) == 4 && read(soc, base + ucr2) == 1 && read(soc, base + ufcr) == 0x0881);
        assert(soc.access(base + usr1, 4, false, value) == B::externalInput);
        assert(soc.access(base + uts + 4, 4, false, value) == B::unmapped);
        assert(soc.access(base + ucr1, 2, false, value) == B::width);
        write(soc, base + ucr1, 0x10000, B::reservedBits); write(soc, base + ucr1, 0x4004, B::sideEffect);
        assert(!soc.instructionWord(base + ucr1, value));
    }
    assert(std::string(Imx1Bootstrap::registerName(uart1 + urxd + 4)) == "UART1.URXD");
    assert(std::string(Imx1Bootstrap::registerName(uart2 + uts)) == "UART2.UTS");
    assert(std::string(Imx1Bootstrap::registerName(uart2 + ucr2)) == "UART2.UCR2" && !Imx1Bootstrap::registerName(uart2 + 0xd4));
    // The two are separate, and UART1 has no DTR bits.
    write(soc, uart1 + ucr3, 0x2000, B::reservedBits); write(soc, uart2 + ucr3, 0x2000);
    write(soc, uart2 + ucr1, 0x205);
    assert(read(soc, uart1 + ucr1) == 4 && read(soc, uart2 + ucr1) == 0x205);
    // Sources 19 to 24 for UART2 and 25 to 30 for UART1; an asserted line needs the CPU connected.
    write(soc, uart2 + ucr2, 0x4027); write(soc, uart2 + ucr4, 0x8041);
    write(soc, aitc + 8, 24); write(soc, aitc + 8, 23); write(soc, aitc + 8, 30);
    assert(soc.interruptLines().irq == Imx1AitcLine::clear && !soc.idleWakePossible());
    write(soc, uart2 + ucr1, 0x2205, B::sideEffect);
    assert(read(soc, uart2 + ucr1) == 0x205);
    soc.connectCpuInterrupts();
    assert(soc.uart(2).receive(0x5a) && soc.interruptLines().irq == Imx1AitcLine::asserted);
    assert(soc.interruptPending().normal == uint64_t(1) << 24);
    assert(read(soc, uart2 + urxd) == 0x805a && soc.interruptLines().irq == Imx1AitcLine::clear);
    // Two characters at the reset rate, 80 PERCLK1 cycles a bit, PERCLK1 = HCLK / 1... counted in HCLK.
    write(soc, uart2 + ucr4, 0x8048);
    assert(soc.interruptPending().normal == uint64_t(1) << 23);
    write(soc, uart2 + utxd, 0x41); write(soc, uart2 + utxd + 0x3c, 0x42);
    assert(soc.interruptLines().irq == Imx1AitcLine::clear && soc.idleWakePossible());
    // PERCLK1 can't change under a character on its way out.
    write(soc, cgm + 32, 1, B::sideEffect);
    assert(read(soc, cgm + 32) == 0);
    uint64_t hclk = 0;
    while (soc.interruptLines().irq == Imx1AitcLine::clear) {
        const uint64_t quiet = soc.idleQuietHclk();
        assert(quiet && quiet < 100000);
        soc.advanceHclkCycles(uint32_t(quiet)); hclk += quiet;
    }
    assert(soc.uartState(2).transmittedCharacters() == 2 && soc.interruptPending().normal == uint64_t(1) << 23);
    std::printf("two characters took %llu HCLK\n", static_cast<unsigned long long>(hclk));
    write(soc, cgm + 32, 1); write(soc, cgm + 32, 0);
    write(soc, uart1 + ucr1, 5); write(soc, uart1 + ucr2, 0x4027); write(soc, uart1 + ucr4, 0x8041);
    assert(soc.uart(1).receive(1) && soc.interruptPending().normal == (uint64_t(1) << 23 | uint64_t(1) << 30));
}
}

int main() {
    resetValues();
    softwareReset();
    transmit();
    transmitInterrupts();
    receive();
    modemPins();
    refusals();
    state();
    bootstrap();
    std::puts("PASS: i.MX1 UART reset values, software reset, baud timing, FIFOs, flags, interrupt outputs, "
              "modem pins, refusals, state and both UARTs behind the bootstrap");
}
