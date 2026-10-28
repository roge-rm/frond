// Firmware-free capability probe. BLOCKED observations are deliberate assertions
// about today's model, not successful serial/USB/HotSync acceptance tests.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
#include "pxa_UART.h"
#include "pxa270_UDC.h"
#include "savestate/savestateAll.h"

struct Region { uint32_t base; ArmMemAccessF access; void* context; };
static std::vector<Region> regions;
static unsigned irqs, reschedules;
extern "C" void uarmAbort() { std::_Exit(86); } // fatal model trap; no core dumps
bool memRegionAdd(ArmMem*, uint32_t base, uint32_t, ArmMemAccessF access, void* context) {
    regions.push_back({base, access, context}); return true;
}
void pxaIcInt(PxaIc*, uint_fast8_t number, bool raised) {
    assert(number == PXA_I_FFUART || number == PXA_I_HWUART || number == PXA_I_USB); if (raised) ++irqs;
}
static uint32_t io(uint32_t base, unsigned offset, bool write, uint32_t value = 0, unsigned size = 4) {
    for (auto& r : regions) if (r.base == base) {
        assert(r.access(r.context, base + offset, size, write, &value)); return value;
    }
    std::abort();
}
static void write(unsigned reg, uint32_t value) { io(PXA_FFUART_BASE, reg * 4, true, value); }
static uint32_t read(unsigned reg) { return io(PXA_FFUART_BASE, reg * 4, false); }
struct Peer {
    std::deque<uint16_t> incoming;
    std::vector<uint16_t> outgoing;
    unsigned reads = 0;
};
static void attach(PxaUart* uart, Peer& peer) {
    pxaUartSetFuncs(uart, [](void* p) -> uint_fast16_t {
        auto& peer = *static_cast<Peer*>(p); ++peer.reads;
        if (peer.incoming.empty()) return UART_CHAR_NONE;
        auto value = peer.incoming.front(); peer.incoming.pop_front(); return value;
    }, [](uint_fast16_t c, void* p) { static_cast<Peer*>(p)->outgoing.push_back(c); },
    [](void* p) { return !static_cast<Peer*>(p)->incoming.empty(); }, nullptr, &peer);
}
// Receive is paced at the programmed baud: step until the peer has sent everything.
static void receive(PxaUart* uart, Peer& peer) {
    for (unsigned ticks = 0; ticks < 2000000 && !peer.incoming.empty(); ++ticks) pxaUartProcess(uart);
    assert(peer.incoming.empty());
}
static void transmit(PxaUart* uart, Peer& peer, unsigned count) {
    const auto goal = peer.outgoing.size() + count;
    for (unsigned ticks = 0; ticks < 2000000 && peer.outgoing.size() < goal; ++ticks)
        pxaUartProcess(uart);
    assert(peer.outgoing.size() == goal);
}
struct State {
    PxaUart* uart;
    Pxa270Udc* udc;
    template<class T> void Save(T& state) { pxaUartSave(uart, state); pxa270UdcSave(udc, state); }
    void Load(SavestateLoader<ChunkType>& state) { pxaUartLoad(uart, state); pxa270UdcLoad(udc, state); }
};
// Older versions have the same guest registers/FIFOs. Version 0 lacks the 32-bit
// timeout counter, 1 the TX frame time, 2 the RX frame time and DMA trailing flags.
static std::vector<uint32_t> legacyVersion(Savestate<ChunkType>& saved, uint32_t version) {
    const auto* data = static_cast<const uint32_t*>(saved.GetBuffer());
    std::vector<uint32_t> words(data, data + saved.GetSize() / 4);
    const auto count = le32toh(words[0]);
    size_t offset = 1 + 2 * count;
    for (unsigned i = 0; i < count; ++i) {
        const auto length = le32toh(words[2 + 2 * i]);
        if (le32toh(words[1 + 2 * i]) == static_cast<uint32_t>(ChunkType::pxaUart)) {
            assert(length > 4 && le32toh(words[offset]) == 3 && version < 3);
            words[offset] = htole32(version);
            const unsigned remove = 4 - version;
            words[2 + 2 * i] = htole32(length - remove);
            words.erase(words.begin() + offset + length - remove, words.begin() + offset + length);
            return words;
        }
        offset += length;
    }
    std::abort();
}
template<class F> static void fatal(F action) {
    std::fflush(nullptr);
    const auto pid = fork(); assert(pid >= 0);
    if (!pid) { action(); std::_Exit(0); }
    int status; assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 86);
}
int main() {
    auto* uart = pxaUartInit(nullptr, {[](void*, uint32_t task) {
        assert(task == RESCHEDULE_TASK_UART); ++reschedules;
    }, nullptr}, nullptr, PXA_FFUART_BASE, PXA_I_FFUART, true);
    auto* udc = pxa270UdcInit(nullptr, nullptr, nullptr);
    Peer peer; attach(uart, peer);
    write(3, 0x83); write(0, 96); write(1, 0); write(3, 3); // 9600 8N1
    write(4, 8); write(1, 0x41); write(2, 7);
    const std::vector<uint16_t> binary{0, 0x7f, 0x80, 0xff, 0x46};
    peer.incoming.assign(binary.begin(), binary.end());
    for (auto c : binary) write(0, c);
    receive(uart, peer);
    transmit(uart, peer, binary.size() - peer.outgoing.size());
    assert(peer.outgoing == binary && read(9) == binary.size() && irqs && reschedules);
    for (auto c : binary) assert(read(0) == c);
    assert(read(9) == 0);
    puts("PASS: FFUART binary callbacks and guest register/IRQ receive path (synthetic)");

    // Core chunks preserve guest queues; a live external peer is not rolled back.
    peer.incoming = {0x12, 0x34}; receive(uart, peer);
    write(0, 0xab); write(0, 0xcd);
    io(0x40600000, 0x404, true, 0x12345);
    State state{uart, udc}; Savestate<ChunkType> saved; assert(saved.Save(state));
    assert(read(0) == 0x12); transmit(uart, peer, 1);
    assert(peer.outgoing.back() == 0xab);
    io(0x40600000, 0x404, true, 0);
    SavestateLoader<ChunkType> loader;
    assert(loader.Load(saved.GetBuffer(), saved.GetSize(), state));
    assert(read(0) == 0x12 && read(0) == 0x34);
    assert(io(0x40600000, 0x404, false) == 0x12345);
    transmit(uart, peer, 2);
    assert((std::vector<uint16_t>(peer.outgoing.end()-3, peer.outgoing.end()) ==
            std::vector<uint16_t>{0xab, 0xab, 0xcd}));
    puts("PASS: UART/UDC chunk roundtrip; external TX replay demonstrates checkpoint hazard");

    // Palm RS232 actually uses these packed word writes with FCR=e1.
    peer.outgoing.clear(); write(2, 0xe7);
    write(0, 0x72537246); write(0, 0xff807f00);
    transmit(uart, peer, 8);
    assert((peer.outgoing == std::vector<uint16_t>{0x46,0x72,0x53,0x72,0,0x7f,0x80,0xff}));
    // Halfword/byte trailing transfers use exactly their requested lanes.
    io(PXA_FFUART_BASE, 0, true, 0xdead1280, 2);
    io(PXA_FFUART_BASE, 0, true, 0xffffff34, 1);
    transmit(uart, peer, 3);
    assert((std::vector<uint16_t>(peer.outgoing.end()-3, peer.outgoing.end()) ==
            std::vector<uint16_t>{0x80,0x12,0x34}));
    write(2, 0x27); // BUS, FIFO, resets; trigger one byte for IRQ checks
    peer.incoming = {0x246,0x72,0x53,0x72,0,0x7f,0x80,0xff};
    receive(uart, peer);
    assert(read(9) == 8 && (read(2) & 0x0f) == 4);
    assert(read(0) == 0x72537246 && read(9) == 4); // error flags are not data lanes
    assert(io(PXA_FFUART_BASE, 0, false, 0, 2) == 0x7f00 && read(9) == 2);
    assert(io(PXA_FFUART_BASE, 0, false, 0, 1) == 0x80 && read(9) == 1);
    assert(read(0) == 0xff && read(9) == 0 && !(read(5) & 1)); // unused lanes zero, no phantom bytes
    assert((read(2) & 0x0f) == 1);
    peer.incoming = {0xa5}; receive(uart, peer);
    assert(read(0) == 0xa5 && read(9) == 0); // FIFO still usable after over-read
    puts("PASS: PXA27x packed 1/2/4-byte TX/RX, trailing bytes, occupancy and IRQ clearing");

    // No new state fields: retain BUS, partial RX and queued packed TX on restore.
    peer.incoming = {0x46,0x72,0x53,0x72,0,0x7f,0x80,0xff};
    receive(uart, peer);
    assert(read(0) == 0x72537246);
    write(0, 0xff807f00); assert(saved.Save(state));
    auto legacy = legacyVersion(saved, 0);
    auto legacyOne = legacyVersion(saved, 1);
    auto legacyTwo = legacyVersion(saved, 2);
    write(2, 7); // clear BUS and both FIFOs
    assert(loader.Load(saved.GetBuffer(), saved.GetSize(), state));
    assert(read(0) == 0xff807f00 && read(9) == 0);
    peer.outgoing.clear();
    transmit(uart, peer, 4);
    assert((peer.outgoing == std::vector<uint16_t>{0,0x7f,0x80,0xff}));
    assert(loader.Load(legacyTwo.data(), legacyTwo.size() * 4, state));
    assert(read(0) == 0xff807f00 && read(9) == 0);
    transmit(uart, peer, 4);
    assert(loader.Load(legacyOne.data(), legacyOne.size() * 4, state));
    assert(read(0) == 0xff807f00 && read(9) == 0);
    transmit(uart, peer, 4);
    assert(loader.Load(legacy.data(), legacy.size() * 4, state));
    assert(read(0) == 0xff807f00 && read(9) == 0);
    peer.outgoing.clear();
    transmit(uart, peer, 4);
    assert((peer.outgoing == std::vector<uint16_t>{0,0x7f,0x80,0xff}));
    // FIFO byte mode still ignores upper bus lanes; DLAB still selects one divisor byte.
    write(2, 7); peer.outgoing.clear(); write(0, 0xdeadbeef); transmit(uart, peer, 1);
    assert((peer.outgoing == std::vector<uint16_t>{0xef}));
    write(2, 0x27); write(3, 0x83); write(0, 0xdeadbe60);
    assert(read(0) == 0x60); write(3, 3);
    puts("PASS: version-0/1/2 packed FIFO restore and unchanged byte/divisor accesses");
    write(1, 0); peer.incoming = {0x55};
    const auto before = peer.reads; pxaUartProcess(uart);
    assert(peer.reads == before && peer.incoming.size() == 1 && !(read(5) & 1));
    write(1, 0x41); receive(uart, peer); assert(read(0) == 0x55);
    puts("PASS: disabled UART retains pending callback RX until re-enabled");
    write(1, 0xc0); assert(read(1) == 0xc0); write(1, 0x41);
    puts("PASS: PXA27x UART DMA enable is accepted (see uart-dma tests)");
    io(0x40600000, 0, true, 1);
    assert(io(0x40600000, 0x300, false) == 0); // no host has supplied a packet
    for (unsigned i = 0; i < 4; ++i) io(0x40600000, 0x300, true, 0x1234);
    assert(io(0x40600000, 0x100, false) == 2); // locally staged, never transmitted
    assert(io(0x40600000, 0x0c, false) == 0);
    for (auto& r : regions) if (r.base == 0x40600000) {
        uint32_t value = 0;
        assert(r.access(r.context, r.base + 0x300, 4, true, &value)); // full EP0: dropped
        assert(r.access(r.context, r.base + 0x304, 4, true, &value)); // EP A is OUT here: write dropped
        assert(r.access(r.context, r.base + 0x308, 4, true, &value)); // EP B not enabled: ignored
        value = 0xffffffff;
        assert(r.access(r.context, r.base + 0x308, 4, false, &value) && value == 0);
    }
    puts("PASS: UDC EP0 staging; writes to a full EP0 or an OUT FIFO are dropped, disabled FIFOs read 0 and ignore writes");
    puts("NOTE: the USB host and HotSync bridge are covered by test-usb-enumeration, test-usb-bridge and test-tx-usb-rom");
    free(uart); free(udc); regions.clear();
    // Earlier FFUART and the separate PXA25x HWUART retain byte-mode transfers,
    // even if guest software writes the BUS bit. No new older-board behavior.
    for (bool hw : {false, true}) {
        const uint32_t base = hw ? PXA_HWUART_BASE : PXA_FFUART_BASE;
        uart = pxaUartInit(nullptr, {[](void*, uint32_t) {}, nullptr}, nullptr,
                           base, hw ? PXA_I_HWUART : PXA_I_FFUART, hw);
        peer = {}; attach(uart, peer);
        io(base, 8, true, 0x27); io(base, 0, true, 0xff807f46);
        pxaUartProcess(uart); pxaUartProcess(uart);
        assert((peer.outgoing == std::vector<uint16_t>{0x46}));
        peer.incoming = {0x12,0x34}; pxaUartProcess(uart); pxaUartProcess(uart);
        assert(io(base, 0, false) == 0x12 && io(base, 0, false) == 0x34);
        fatal([&] { io(base, 4, true, 0xc0); });
        free(uart); regions.clear();
    }
    puts("PASS: earlier FFUART and PXA25x HWUART retain byte-mode bus behavior; their DMA enable stays fatal");
}
