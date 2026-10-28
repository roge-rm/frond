// Firmware-free tests for PXA27x UDC enumeration and bulk endpoints: requests the UDC
// answers itself, configuration and SMAC, endpoint configuration checks, A-X FIFOs,
// savestate v3, and the frame-scheduled host against a scripted XLLP-style guest.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <type_traits>
#include <vector>
#include "usb-host-fixture.h"
#include "usb-frame-host.h"

static ArmMemAccessF mmio;
static bool irq;
extern "C" void uarmAbort() { std::abort(); }
bool memRegionAdd(ArmMem*, uint32_t, uint32_t, ArmMemAccessF f, void*) { mmio = f; return true; }
void pxaIcInt(PxaIc*, uint_fast8_t n, bool level) { assert(n == PXA_I_USB); irq = level; }

using R = Pxa270UdcHostResult;
static Pxa270Udc* udc;
static void wr(unsigned offset, uint32_t value) { assert(mmio(udc, 0x40600000 + offset, 4, true, &value)); }
static uint32_t rd(unsigned offset) { uint32_t v = 0; assert(mmio(udc, 0x40600000 + offset, 4, false, &v)); return v; }
static void wr8(unsigned offset, uint8_t value) { assert(mmio(udc, 0x40600000 + offset, 1, true, &value)); }

// UDCCRx: enable, double buffer, MPS, IN, type, endpoint number, alternate, interface, configuration.
static uint32_t ccr(bool in, unsigned type, unsigned number, unsigned mps, bool db = false, unsigned cfg = 1,
                    unsigned intf = 0, unsigned alt = 0) {
    return 1u | (db ? 2u : 0u) | mps << 2 | (in ? 1u << 12 : 0u) | type << 13 | number << 15 | alt << 19 |
           intf << 22 | cfg << 25;
}
static void setupPacket(uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t length, uint8_t out[8]) {
    const uint8_t r[8] = {type, req, uint8_t(value), uint8_t(value >> 8), uint8_t(index), uint8_t(index >> 8),
                          uint8_t(length), uint8_t(length >> 8)};
    std::memcpy(out, r, 8);
}
// Raw control transfer through the entry points, no guest: SETUP, IN data, status.
static std::vector<uint8_t> hwControl(uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t length,
                                      R* status = nullptr) {
    uint8_t s[8]; setupPacket(type, req, value, index, length, s);
    assert(pxa270UdcHostSetup(udc, s) == R::Ack);
    std::vector<uint8_t> data;
    uint8_t buf[16]; unsigned size = 0;
    if ((type & 0x80) && length) {
        R r = pxa270UdcHostIn(udc, buf, &size);
        if (status) *status = r;
        if (r != R::Ack) return data;
        data.assign(buf, buf + size);
        assert(pxa270UdcHostOut(udc, nullptr, 0) == R::Ack);
    } else {
        R r = pxa270UdcHostIn(udc, buf, &size);
        if (status) *status = r;
        assert(r != R::Ack || !size);
    }
    return data;
}
static void freshUdc() {
    if (udc) free(udc);
    udc = pxa270UdcInit(nullptr, nullptr, nullptr);
    irq = false;
}

static void testHardwareRequests() {
    freshUdc();
    wr(0x404, ccr(true, 2, 1, 64)); wr(0x408, ccr(false, 2, 2, 64));
    wr(0x40c, ccr(true, 2, 3, 64, false, 1, 0, 1)); // alternate setting 1
    wr(0, 1); wr(4, 3); wr(8, (1u << 31) | (1u << 27));
    assert(pxa270UdcHostReset(udc) && irq); wr(0x10, 1u << 27); assert(!irq);

    // SET_ADDRESS: no OPC, no IR0, UDA until the status IN, address after it.
    uint8_t s[8]; setupPacket(0, 5, 0x2a, 0, 0, s);
    assert(pxa270UdcHostSetup(udc, s) == R::Ack && !irq && !rd(0x100) && !rd(0x0c) && (rd(0) & 2));
    assert(pxa270UdcHostAddress(udc) == 0);
    uint8_t buf[64]; unsigned size = 9;
    assert(pxa270UdcHostIn(udc, buf, &size) == R::Ack && !size && pxa270UdcHostAddress(udc) == 0x2a && !(rd(0) & 2));
    puts("PASS: SET_ADDRESS answered by the UDC, applied after its status stage");

    // GET_STATUS / GET_CONFIGURATION unconfigured; GET_INTERFACE stalls.
    assert(hwControl(0x80, 0, 0, 0, 2) == (std::vector<uint8_t>{0, 0}));
    assert(hwControl(0x80, 8, 0, 0, 1) == (std::vector<uint8_t>{0}));
    R r; hwControl(0x81, 10, 0, 0, 1, &r); assert(r == R::Stall && !(rd(0) & 2));
    hwControl(0x82, 0, 0, 0x81, 2, &r); assert(r == R::Stall); // endpoint not in the active configuration
    assert(!irq && !rd(0x0c));
    // Bulk tokens before configuration get no handshake.
    assert(pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Timeout);
    assert(pxa270UdcHostEpOut(udc, 2, buf, 4) == R::Timeout);

    // Remote wakeup and OTG features.
    hwControl(0x00, 3, 1, 0, 0, &r); assert(r == R::Ack && (rd(0) & (1u << 16)));
    assert(hwControl(0x80, 0, 0, 0, 2) == (std::vector<uint8_t>{2, 0}));
    hwControl(0x00, 1, 1, 0, 0, &r); assert(r == R::Ack && !(rd(0) & (1u << 16)));
    hwControl(0x00, 3, 3, 0, 0, &r); assert(r == R::Stall && !(rd(0) & (1u << 28)));
    puts("PASS: GET_STATUS, GET_CONFIGURATION, GET_INTERFACE stall, remote wakeup, OTG STALL without OEN");

    // SET_CONFIGURATION with ACM=0: IRCC, ACN, immediate status ACK, NAK until SMAC.
    setupPacket(0, 9, 1, 0, 0, s);
    assert(pxa270UdcHostSetup(udc, s) == R::Ack && irq && rd(0x10) == (1u << 31) && ((rd(0) >> 11) & 3) == 1);
    assert(!rd(0x100) && !rd(0x0c));
    assert(pxa270UdcHostIn(udc, buf, &size) == R::Ack && !size);
    wr(0x10, 1u << 31); assert(!irq);
    assert(pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Nak && pxa270UdcHostEpOut(udc, 2, buf, 4) == R::Nak);
    assert(pxa270UdcHostEpIn(udc, 3, buf, &size) == R::Timeout); // alternate setting 1 not selected
    wr(0, 0x11); assert(!(rd(0) & 0x10) && udc->memSwitched);
    assert(pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Nak && (rd(0x104) & 4)); // TRN
    assert(hwControl(0x80, 8, 0, 0, 1) == (std::vector<uint8_t>{1}));
    assert(hwControl(0x81, 10, 0, 0, 1) == (std::vector<uint8_t>{0}));
    assert(hwControl(0x82, 0, 0, 0x81, 2) == (std::vector<uint8_t>{0, 0}));
    hwControl(0x82, 0, 0, 0x01, 2, &r); assert(r == R::Stall); // EP1 OUT does not exist
    // SET_INTERFACE alternate 1: endpoint 3 becomes active, AIN/AAISN follow.
    setupPacket(1, 11, 1, 0, 0, s);
    assert(pxa270UdcHostSetup(udc, s) == R::Ack && rd(0x10) == (1u << 31) && ((rd(0) >> 5) & 7) == 1 && ((rd(0) >> 8) & 7) == 0);
    assert(pxa270UdcHostIn(udc, buf, &size) == R::Ack);
    wr(0x10, 1u << 31); wr(0, 0x11);
    assert(pxa270UdcHostEpIn(udc, 3, buf, &size) == R::Nak && pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Timeout);
    assert(hwControl(0x81, 10, 0, 0, 1) == (std::vector<uint8_t>{1}));
    puts("PASS: SET_CONFIGURATION/SET_INTERFACE: ACN/AIN/AAISN, IRCC, NAK until SMAC, alternate settings");

    // ACM=1: the status IN NAKs until AREN; the next SETUP clears AREN.
    wr(0x100, 0x200);
    setupPacket(0, 9, 1, 0, 0, s);
    assert(pxa270UdcHostSetup(udc, s) == R::Ack);
    assert(pxa270UdcHostIn(udc, buf, &size) == R::Nak && pxa270UdcHostIn(udc, buf, &size) == R::Nak);
    wr(0x100, 0x300); assert(rd(0x100) == 0x300);
    assert(pxa270UdcHostIn(udc, buf, &size) == R::Ack && !size && !(rd(0) & 2));
    setupPacket(0x80, 6, 0x100, 0, 18, s);
    assert(pxa270UdcHostSetup(udc, s) == R::Ack && rd(0x100) == 0x2c1); // forwarded: SA|RNE|OPC, ACM kept, AREN cleared
    wr(0x100, 0x281); wr(0x0c, 1); wr(0x100, 0);
    wr(0x10, ~0u);
    puts("PASS: ACM=1 holds the SET_CONFIGURATION status until AREN; GET_DESCRIPTOR still forwarded");

    // Endpoint halt by the host, and by FST.
    wr(0, 0x11);
    hwControl(0x02, 3, 0, 0x81, 0, &r); assert(r == R::Ack);
    assert(hwControl(0x82, 0, 0, 0x81, 2) == (std::vector<uint8_t>{1, 0}));
    assert(pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Stall);
    hwControl(0x02, 1, 0, 0x81, 0, &r); assert(r == R::Ack);
    assert(pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Nak);
    wr(0x108, 0x20); // FST on EP B (OUT)
    assert(pxa270UdcHostEpOut(udc, 2, buf, 4) == R::Stall && (rd(0x108) & 0x30) == 0x10);
    assert(pxa270UdcHostEpOut(udc, 2, buf, 4) == R::Stall);
    assert(hwControl(0x82, 0, 0, 0x02, 2) == (std::vector<uint8_t>{1, 0}));
    wr(0x108, 0x10); hwControl(0x02, 1, 0, 0x02, 0, &r);
    assert(r == R::Ack && !rd(0x108) && pxa270UdcHostEpOut(udc, 2, buf, 4) == R::Ack);
    puts("PASS: ENDPOINT_HALT by SET_FEATURE and by FST (SST set), GET_STATUS shows it, CLEAR_FEATURE clears");

    // SET_INTERFACE clears the halt of that interface's endpoints, set by the host or by FST.
    hwControl(0x02, 3, 0, 0x81, 0, &r); assert(r == R::Ack);
    wr(0x108, 0x20); assert(pxa270UdcHostEpOut(udc, 2, buf, 4) == R::Stall);
    assert(hwControl(0x82, 0, 0, 0x81, 2) == (std::vector<uint8_t>{1, 0}));
    hwControl(0x01, 11, 0, 0, 0, &r); assert(r == R::Ack);
    wr(0x10, 1u << 31); wr(0, 0x11);
    assert(hwControl(0x82, 0, 0, 0x81, 2) == (std::vector<uint8_t>{0, 0}));
    assert(hwControl(0x82, 0, 0, 0x02, 2) == (std::vector<uint8_t>{0, 0}) && !(rd(0x108) & 0x30));
    assert(pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Nak);
    puts("PASS: SET_INTERFACE clears halts of that interface's endpoints");

    // Bus reset drops address, configuration, halts and FIFOs.
    hwControl(0x02, 3, 0, 0x81, 0, &r);
    assert(pxa270UdcHostReset(udc) && !pxa270UdcHostAddress(udc) && !((rd(0) >> 5) & 0xff) && !udc->halted &&
           !udc->memSwitched && !udc->fifo[2].count && !rd(0x108));
    assert(pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Timeout);
    puts("PASS: bus reset clears address, configuration, halts and A-X FIFOs");

    // SOF: UDCFNR counts frames (11 bits) and IRSOF latches only when enabled.
    wr(8, 0); wr(0x10, ~0u);
    assert(pxa270UdcHostFrame(udc) && rd(0x14) == 1 && !rd(0x10) && !irq);
    wr(8, 1u << 30);
    for (unsigned i = 1; i < 2048; ++i) pxa270UdcHostFrame(udc);
    assert(rd(0x14) == 0 && rd(0x10) == (1u << 30) && irq);
    wr(0x10, 1u << 30); wr(8, 0); assert(!irq);
    puts("PASS: SOF advances UDCFNR modulo 2048 and latches IRSOF when enabled");
}

static void configure(bool doubleIn, bool doubleOut) {
    freshUdc();
    wr(0x404, ccr(true, 2, 1, 64, doubleIn)); wr(0x408, ccr(false, 2, 2, 64, doubleOut));
    wr(0x44c, ccr(true, 3, 5, 8)); // endpoint S (index 19): interrupt IN, its bits are in UDCISR1
    wr(0, 1); wr(4, 0x3f); wr(8, 1u << 6);
    pxa270UdcHostReset(udc);
    uint8_t s[8]; setupPacket(0, 9, 1, 0, 0, s);
    pxa270UdcHostSetup(udc, s); uint8_t buf[16]; unsigned size; pxa270UdcHostIn(udc, buf, &size);
    wr(0, 0x11); wr(0x0c, ~0u); wr(0x10, ~0u);
}

static void testBulkFifos() {
    uint8_t buf[64]; unsigned size;
    configure(false, false);
    // IN: empty FIFO NAKs with TRN, FS/BNF set.
    assert(rd(0x104) == 0x041 && pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Nak && rd(0x104) == 0x045);
    wr(0x104, 4); assert(rd(0x104) == 0x041);
    // A full packet commits itself; further writes are dropped while the single buffer is full.
    for (unsigned i = 0; i < 16; ++i) wr(0x304, 0x03020100u + 0x04040404u * i);
    assert(rd(0x104) == 0 && pxa270UdcHostPeekIn(udc, 1, &size) == R::Ack && size == 64);
    wr(0x304, 0xffffffff);
    assert(!irq && pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Ack && size == 64);
    for (unsigned i = 0; i < 64; ++i) assert(buf[i] == i);
    assert(rd(0x104) == 0x043 && irq && rd(0x0c) == 4); // PC, packet-complete IRQ for A
    wr(0x0c, 4); wr(0x104, 2); assert(!irq && rd(0x104) == 0x041);
    // Short packet: words, then bytes, then SP. ZLP: SP alone.
    wr(0x304, 0x44332211); wr8(0x304, 0x55); wr8(0x304, 0x66);
    assert(rd(0x104) == 0x041); wr(0x104, 0x80); assert(rd(0x104) == 0x080);
    assert(pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Ack && size == 6 && buf[4] == 0x55 && buf[5] == 0x66);
    wr(0x104, 0x80); assert(pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Ack && size == 0);
    // FEF flushes a loaded packet.
    wr(0x304, 1); wr(0x104, 0x80); wr(0x104, 0x100); assert(pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Nak);
    // Writing SST=1 while SST is clear changes nothing; clearing a set SST flushes the IN FIFO.
    wr(0x304, 1); wr(0x104, 0x80); wr(0x104, 0x10);
    assert(pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Ack && size == 4 && buf[0] == 1);
    wr(0x0c, ~0u); wr(0x104, 6);
    wr(0x104, 0x20); assert(pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Stall && (rd(0x104) & 0x30) == 0x10);
    wr(0x304, 2); wr(0x104, 0x80); wr(0x104, 0x10); assert(!(rd(0x104) & 0x10));
    { R r; hwControl(0x02, 1, 0, 0x81, 0, &r); assert(r == R::Ack); }
    assert(pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Nak);
    wr(0x0c, ~0u); wr(0x104, 6);
    puts("PASS: bulk IN: NAK/TRN when empty, full and short packets, byte tails, ZLP, FEF, SST flush only when set, PC IRQ");

    // OUT: packet complete, BNE, SP for short, byte count, reads, NAK while full (single buffer).
    uint8_t out[64]; for (unsigned i = 0; i < 64; ++i) out[i] = uint8_t(0xa0 + i);
    assert(rd(0x108) == 0 && pxa270UdcHostEpOut(udc, 2, out, 13) == R::Ack);
    assert(rd(0x108) == 0x0c3 && rd(0x208) == 13 && irq && rd(0x0c) == 0x10);
    assert(pxa270UdcHostEpOut(udc, 2, out, 4) == R::Nak && (rd(0x108) & 4));
    assert(rd(0x308) == 0xa3a2a1a0 && rd(0x208) == 9);
    rd(0x308); rd(0x308); assert(rd(0x208) == 1 && rd(0x308) == 0xac && !rd(0x208) && rd(0x108) == 0x087);
    wr(0x108, 6); assert(!rd(0x108) && !rd(0x208));
    assert(pxa270UdcHostEpOut(udc, 2, out, 65) == R::Timeout); // longer than the maximum: no handshake
    assert(pxa270UdcHostEpOut(udc, 2, out, 64) == R::Ack && rd(0x108) == 0x043 && rd(0x208) == 64);
    wr(0x108, 2); assert(!rd(0x108)); // clearing PC drops the unread packet
    assert(pxa270UdcHostEpOut(udc, 2, nullptr, 0) == R::Ack && rd(0x108) == 0x083 && !rd(0x208)); // ZLP: SP, no BNE
    wr(0x108, 2); wr(0x0c, ~0u);
    puts("PASS: bulk OUT: PC/FS/BNE/SP, byte count, NAK with TRN while full, PC clear drops, ZLP, oversize gets no handshake");

    // Double buffering both ways.
    configure(true, true);
    for (unsigned p = 0; p < 2; ++p) {
        assert(rd(0x104) & 1);
        for (unsigned i = 0; i < 16; ++i) wr(0x304, p);
    }
    assert(rd(0x104) == 0); wr(0x304, 9); // dropped: both buffers full
    assert(pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Ack && size == 64 && buf[0] == 0 && (rd(0x104) & 0x41) == 0x41);
    wr(0x304, 7); wr(0x104, 0x80); // short packet behind the queued full one
    assert(pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Ack && size == 64 && buf[0] == 1);
    assert(pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Ack && size == 4 && buf[0] == 7);
    out[0] = 1; assert(pxa270UdcHostEpOut(udc, 2, out, 64) == R::Ack);
    out[0] = 2; assert(pxa270UdcHostEpOut(udc, 2, out, 5) == R::Ack);
    assert(pxa270UdcHostEpOut(udc, 2, out, 5) == R::Nak);
    assert(rd(0x208) == 64 && (rd(0x308) & 0xff) == 1);
    wr(0x108, 2); assert(rd(0x108) == 0x0c7 && rd(0x208) == 5 && (rd(0x308) & 0xff) == 2); // next packet, PC again
    wr(0x108, 6); assert(!rd(0x108));
    // Interrupt endpoint S (index 19) reports in UDCISR1 bit 6.
    wr(0x14c, 0x80); wr(0x0c, ~0u); wr(0x10, ~0u); assert(!irq);
    assert(pxa270UdcHostEpIn(udc, 5, buf, &size) == R::Ack && !size && rd(0x10) == (1u << 6) && irq);
    puts("PASS: double-buffered IN and OUT queue two packets; endpoint S packet-complete in UDCISR1");

    // DME on an A-X endpoint is not modelled: the host is told so.
    wr(0x108, 8); assert(pxa270UdcHostEpOut(udc, 2, out, 4) == R::Unsupported); wr(0x108, 0);
}

static void testEndpointMemory() {
    freshUdc();
    wr(0x404, ccr(true, 2, 1, 48)); wr(0, 1); assert(rd(0) == 8); // bulk MPS must be 8/16/32/64
    wr(0x404, ccr(true, 1, 1, 1023, true)); wr(0x408, ccr(false, 1, 2, 1023, true)); wr(0, 9); assert(rd(0) == 8);
    wr(0x408, ccr(false, 1, 2, 1023, true, 2)); wr(0, 9); assert(rd(0) == 1); // other configuration: fits
    wr(0, 0); wr(0x404, ccr(true, 3, 1, 65)); wr(0, 1); assert(rd(0) == 8);
    wr(0x404, 0); wr(0, 9); assert(rd(0) == 1);
    puts("PASS: UDE checks packet sizes and 4064 bytes per configuration/interface/alternate setting (EMCE)");
}

template<class F> struct Saver { F& f; template<class T> void Save(T& s) { f(s); } };
template<class F> static std::vector<uint8_t> saveWith(F&& save) {
    Saver<std::remove_reference_t<F>> saver{save};
    Savestate<ChunkType> state; assert(state.Save(saver));
    auto* b = static_cast<uint8_t*>(state.GetBuffer());
    return {b, b + state.GetSize()};
}
static bool loadBytes(std::vector<uint8_t>& bytes) {
    struct Loader { void Load(SavestateLoader<ChunkType>& l) { pxa270UdcLoad(udc, l); } } loader;
    SavestateLoader<ChunkType> l; return l.Load(bytes.data(), bytes.size(), loader);
}

static void testSavestate() {
    uint8_t buf[64]; unsigned size;
    configure(true, false);
    uint8_t s[8]; setupPacket(0, 5, 3, 0, 0, s); pxa270UdcHostSetup(udc, s); pxa270UdcHostIn(udc, buf, &size);
    wr(0x304, 0x11223344); wr(0x104, 0x80);
    wr(0x304, 0x55);  // partial load of the second buffer
    uint8_t out[7] = {1, 2, 3, 4, 5, 6, 7};
    assert(pxa270UdcHostEpOut(udc, 2, out, 7) == R::Ack); rd(0x308);
    setupPacket(0x80, 0, 0, 0, 2, s); pxa270UdcHostSetup(udc, s); // GET_STATUS answered by the UDC, data pending
    auto saved = saveWith([](auto& st) { pxa270UdcSave(udc, st); });
    Pxa270Udc copy = *udc;
    configure(false, false);
    assert(loadBytes(saved));
    assert(!std::memcmp(&copy.fifo, &udc->fifo, sizeof(copy.fifo)) && pxa270UdcHostAddress(udc) == 3 && udc->memSwitched);
    assert(pxa270UdcHostIn(udc, buf, &size) == R::Ack && size == 2); // the UDC's own reply survives
    assert(pxa270UdcHostOut(udc, nullptr, 0) == R::Ack);
    assert(rd(0x208) == 3 && pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Ack && size == 4 && buf[0] == 0x44);
    wr(0x304, 0x66); wr(0x104, 0x80);
    assert(pxa270UdcHostEpIn(udc, 1, buf, &size) == R::Ack && size == 8 && buf[4] == 0x66);
    // Invalid v3 state is refused without changing anything.
    auto bad = saved; Pxa270Udc before = *udc;
    udc->fifo[1].count = 3; bad = saveWith([](auto& st) { pxa270UdcSave(udc, st); }); *udc = before;
    assert(!loadBytes(bad) && !std::memcmp(udc, &before, sizeof(before)));
    udc->hw0 = 7; bad = saveWith([](auto& st) { pxa270UdcSave(udc, st); }); *udc = before;
    assert(!loadBytes(bad));
    // A v2 chunk: A-X CSRs keep only stored bits, no address, FIFOs empty.
    configure(false, false);
    udc->ep[1].csr = 0x0c1; // as an old model might have saved (SP/BNF/FS bits)
    auto v2 = saveWith([](auto& st) {
        auto* c = st.GetChunk(ChunkType::pxa270udc, 2); assert(c);
        SaveChunkHelper h(*c); udc->DoSaveLoad(h, 2);
    });
    configure(true, true); wr(0x304, 1); wr(0x104, 0x80);
    assert(loadBytes(v2) && !udc->fifo[1].count && !pxa270UdcHostAddress(udc) && rd(0x104) == 0x041 && udc->memSwitched);
    puts("PASS: savestate v3 keeps FIFOs, address, configuration and UDC replies; bad v3 refused; v2 loads");
}

// Scripted guest in the style of the TX ROM's XLLP driver: the EP0 UDCISR0 W1C comes only
// after the handler, so an EP0 event raised while it runs is lost. A-X events are
// acknowledged first. Steps in 1 us.
struct FakeGuest {
    static constexpr uint64_t kLatencyNs = 4000, kHandlerNs = 30000;
    uint64_t now = 0, w1cAt = 0;
    bool w1cPending = false;
    unsigned state0 = 0, left = 0; const uint8_t* ptr = nullptr;
    std::vector<uint8_t> descriptor, received;
    std::deque<uint8_t> toSend; bool sendZlpAtEnd = false;
    unsigned zlps = 0;
    uint64_t irqSince = UINT64_MAX;
    std::vector<uint64_t> setupSeen;
    FakeGuest() { for (unsigned i = 0; i < 40; ++i) descriptor.push_back(uint8_t(i == 0 ? 40 : i * 3)); }
    void send0() {
        const unsigned n = std::min(16u, left);
        unsigned i = 0;
        for (; i + 4 <= n; i += 4) { uint32_t w; std::memcpy(&w, ptr + i, 4); wr(0x300, w); }
        for (; i < n; ++i) wr8(0x300, ptr[i]);
        ptr += n; left -= n;
        if (n < 16) { wr(0x100, 2); state0 = 3; } else state0 = 2;
    }
    void ep0() {
        const uint32_t csr = rd(0x100);
        if (csr & 1) {
            const unsigned count = rd(0x200) & 0x3ff;
            uint8_t req[16] = {};
            for (unsigned i = 0; i < count; i += 4) { uint32_t w = rd(0x300); std::memcpy(req + i, &w, 4); }
            wr(0x100, 0x81);
            state0 = 0;
            if (count < 8) return; // status OUT
            setupSeen.push_back(now);
            const unsigned length = req[6] | req[7] << 8;
            if (req[1] == 6) { ptr = descriptor.data(); left = std::min<unsigned>(length, descriptor.size()); send0(); }
            return;
        }
        if (state0 == 2) send0();
    }
    void fillIn() {
        while ((rd(0x104) & 1) && (!toSend.empty() || sendZlpAtEnd)) {
            unsigned n = 0;
            while (n < 64 && !toSend.empty()) { wr8(0x304, toSend.front()); toSend.pop_front(); ++n; }
            if (n < 64) { wr(0x104, 0x80); sendZlpAtEnd = false; }
            else if (toSend.empty() && !sendZlpAtEnd) break;
        }
    }
    void epOut() {
        for (uint32_t csr; (csr = rd(0x108)) & 3;) {
            if ((csr & 0xc0) == 0x80) ++zlps;
            const unsigned count = rd(0x208);
            for (unsigned i = 0; i < count; i += 4) {
                const uint32_t w = rd(0x308);
                for (unsigned b = 0; b < 4 && i + b < count; ++b) received.push_back(uint8_t(w >> (8 * b)));
            }
            wr(0x108, 2);
        }
    }
    // The handler reads UDCISR0 on entry, does its work kHandlerNs later and, for EP0,
    // writes the W1C 1 us after that, as XLLP does right after loading the next packet.
    uint32_t isr0 = 0; uint64_t workAt = 0; bool working = false;
    void tick() {
        if (w1cPending && now >= w1cAt) { wr(0x0c, 1); wr(0x100, 4); w1cPending = false; }
        if (working && now >= workAt) {
            working = false;
            if (isr0 & 0xc) fillIn();
            if (isr0 & 0x30) epOut();
            if (isr0 & 1) { ep0(); w1cAt = now + 1000; w1cPending = true; }
        }
        if (working || w1cPending || !irq) { irqSince = UINT64_MAX; return; }
        if (irqSince == UINT64_MAX) irqSince = now;
        if (now < irqSince + kLatencyNs) return;
        irqSince = UINT64_MAX;
        isr0 = rd(0x0c);
        const uint32_t isr1 = rd(0x10);
        wr(0x10, isr1 & 0xf8000000u);
        if (isr1 & (1u << 31)) { wr(0, rd(0) | 0x10); wr(4, 0x3f); } // configuration change: SMAC, enable A and B
        if (isr0 & 0xc) { wr(0x0c, isr0 & 0xc); wr(0x104, 4); }
        if (isr0 & 0x30) wr(0x0c, isr0 & 0x30);
        if (isr0 & 2) wr(0x0c, 2);
        working = true; workAt = now + kHandlerNs;
    }
    void runUntil(uint64_t t) { for (; now < t; now += 1000) tick(); }
};

static void testFrameHost() {
    freshUdc();
    wr(0x404, ccr(true, 2, 1, 64, true)); wr(0x408, ccr(false, 2, 2, 64));
    wr(0, 1); wr(4, 3); wr(8, (1u << 31) | (1u << 27));
    FakeGuest guest;
    std::vector<std::pair<uint64_t, uint64_t>> log;
    UsbFrameHost bus([] { return udc; }, [&](uint64_t t) { guest.runUntil(t); });
    bus.log = &log;
    bus.reset();
    using S = UsbFrameHost::Status;
    auto d = bus.getDescriptor(1, 0, 64);
    if (d.status != S::Ok || d.data != guest.descriptor) std::printf("descriptor: status=%d error=%s size=%zu packets=%u naks=%u setups=%zu csr0=%x isr0=%x icr0=%x\n", int(d.status), d.error.c_str(), d.data.size(), d.packets, d.naks, guest.setupSeen.size(), rd(0x100), rd(0x0c), rd(4));
    assert(d.status == S::Ok && d.data == guest.descriptor && d.packets == 4 && guest.setupSeen.size() == 1);
    // Exactly 32 bytes: ends on a full packet, the guest's armed ZLP is dropped by the status OUT.
    auto d32 = bus.getDescriptor(1, 0, 32);
    assert(d32.status == S::Ok && d32.data.size() == 32 && !udc->tx0Count);
    assert(bus.setAddress(5).status == S::Ok && bus.address == 5);
    uint64_t lastEnd = 0;
    for (unsigned i = 0; i < 20; ++i) {
        auto r = bus.getDescriptor(1, 0, uint16_t(18 + i));
        assert(r.status == S::Ok && r.data.size() == 18 + i);
        assert(!lastEnd || r.startNs >= lastEnd + UsbFrameHost::kFrameNs);
        assert(r.startNs % UsbFrameHost::kFrameNs < 10000); // starts right after a SOF
        lastEnd = r.endNs;
    }
    assert(guest.setupSeen.size() == 22);
    assert(udc->udcfnr == (bus.frames & 0x7ff));
    puts("PASS: frame host: control transfers start after SOF, 1 ms after the previous one; no SETUP lost to the late W1C");

    // SET_CONFIGURATION: the guest takes IRCC, sets SMAC and enables A/B interrupts.
    auto sc = bus.request(0, 9, 1, 0, 0);
    if (sc.status != S::Ok) std::printf("set_configuration: status=%d error=%s naks=%u csr0=%x\n", int(sc.status), sc.error.c_str(), sc.naks, rd(0x100));
    assert(sc.status == S::Ok);
    bus.advance(2 * UsbFrameHost::kFrameNs);
    assert(udc->memSwitched && rd(4) == 0x3f);
    std::vector<uint8_t> payload(300);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 13);
    auto o = bus.bulkOut(2, payload, 64, false);
    bus.advance(UsbFrameHost::kFrameNs);
    assert(o.status == S::Ok && o.packets == 5 && guest.received == payload);
    guest.received.clear();
    std::vector<uint8_t> exact(payload.begin(), payload.begin() + 128);
    o = bus.bulkOut(2, exact, 64, true);
    bus.advance(UsbFrameHost::kFrameNs);
    assert(o.status == S::Ok && o.packets == 3 && o.zlp && guest.received == exact && guest.zlps == 1);
    guest.toSend.assign(payload.begin(), payload.end());
    guest.fillIn();
    auto in = bus.bulkIn(1, 1000, 64);
    assert(in.status == S::Ok && in.data == payload && in.packets == 5);
    guest.toSend.assign(exact.begin(), exact.end()); guest.sendZlpAtEnd = true; guest.fillIn();
    in = bus.bulkIn(1, 1000, 64);
    assert(in.status == S::Ok && in.data == exact && in.zlp && in.packets == 3);
    in = bus.bulkIn(1, 64, 64, 5 * UsbFrameHost::kFrameNs);
    assert(in.status == S::Timeout && in.naks > 100);
    // No transaction straddles a frame boundary.
    for (auto& t : log) assert(t.first / UsbFrameHost::kFrameNs == (t.second - 1) / UsbFrameHost::kFrameNs);
    std::printf("PASS: frame host bulk OUT 300/128+ZLP and IN 300/128+ZLP through the guest; %zu transactions inside frames\n",
                log.size());
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    testHardwareRequests();
    testBulkFifos();
    testEndpointMemory();
    testSavestate();
    testFrameHost();
    free(udc);
}
