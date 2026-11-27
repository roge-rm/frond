#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "mem.h"
#include "pxa_GPIO.h"
#include "savestate/savestateAll.h"
#include "spitz_cf_ethernet.h"
#include "spitz_pcmcia.h"

namespace {
constexpr uint32_t io = 0x20000000, attribute = 0x28000000, scoop = 0x10800000;

struct Board {
    ArmMem* mem = memInit();
    PxaIc* ic = pxaIcInit(nullptr, mem, nullptr, 1);
    PxaGpio* gpio = pxaGpioInit(mem, ic, 2);
    SpitzPcmcia* slot = spitzPcmciaInit(mem, gpio);
    std::vector<uint8_t> sent;

    Board() {
        assert(slot);
        spitzPcmciaEthernetTransmit(slot, [](void* context, const uint8_t* frame, size_t size) {
            static_cast<Board*>(context)->sent.assign(frame, frame + size);
        }, this);
    }

    ~Board() {
        spitzPcmciaDeinit(slot);
        memDeinit(mem);
        std::free(mem);
        std::free(gpio);
        std::free(ic);
    }

    void Write(uint32_t address, uint16_t value, unsigned size = 1) {
        uint8_t bytes[] = {uint8_t(value), uint8_t(value >> 8)};
        assert(memAccess(mem, address, size, true, bytes));
    }

    uint16_t Read(uint32_t address, unsigned size = 1) {
        uint8_t bytes[2]{};
        assert(memAccess(mem, address, size, false, bytes));
        return uint16_t(bytes[0] | (bytes[1] << 8));
    }

    void Save(Savestate<ChunkType>& state) { spitzPcmciaSave(slot, state); }
    void Save(SavestateProbe<ChunkType>& state) { spitzPcmciaSave(slot, state); }
    void Load(SavestateLoader<ChunkType>& loader) { spitzPcmciaLoad(slot, loader, false); }

    void Dma(uint16_t address, uint16_t size, bool write) {
        Write(io, 0x22);
        Write(io + 8, address);
        Write(io + 9, address >> 8);
        Write(io + 0xa, size);
        Write(io + 0xb, size >> 8);
        Write(io, write ? 0x12 : 0x0a);
    }
};

void testEthernet() {
    Board board;
    assert(pxaGpioInputLevel(board.gpio, 94));
    assert(spitzPcmciaInsertEthernet(board.slot));
    assert(!pxaGpioInputLevel(board.gpio, 94));
    assert(!spitzPcmciaCompactFlashInserted(board.slot));
    assert(!spitzPcmciaInsertEthernet(board.slot));
    assert(board.Read(attribute) == 0xff);
    board.Write(scoop + 0xc, 1);
    assert(board.Read(attribute) == 1 && board.Read(attribute + 1) == 0xff);
    assert(board.Read(attribute + 2 * 5) == 0x15);
    assert(board.Read(attribute + 2 * 6) == 26);
    assert(board.Read(attribute + 2 * 35) == 6);
    assert(board.Read(io) == 0xff);
    assert(board.Read(0x38000000) == 0xff);

    board.Write(scoop, 0x10, 2);
    board.Write(attribute + 0x200, 0x41);
    assert(board.Read(io + 0x1f) == 0 && board.Read(io + 7) == 0x80);
    board.Write(io + 0xe, 0x48);
    board.Write(io + 7, 0xff);
    board.Write(io + 0xf, 0x43);
    board.Dma(0, 32, false);
    for (unsigned i = 0; i < 32; ++i) {
        const unsigned index = i / 2;
        const uint8_t expected = index < SpitzCfEthernet::mac.size() ? SpitzCfEthernet::mac[index] :
                                 index >= 14 ? 0x57 : 0;
        assert(board.Read(io + 0x10) == expected);
    }
    assert(board.Read(io + 7) == 0x40 && !pxaGpioInputLevel(board.gpio, 105));
    board.Write(io + 7, 0x40);
    assert(pxaGpioInputLevel(board.gpio, 105));

    board.Write(io + 0xe, 0x49);
    std::array<uint8_t, 60> tx{};
    for (unsigned i = 0; i < tx.size(); ++i) tx[i] = i;
    board.Dma(0x4000, tx.size(), true);
    for (unsigned i = 0; i < tx.size(); i += 2)
        board.Write(io + 0x10, tx[i] | (uint16_t(tx[i + 1]) << 8), 2);
    board.Write(io + 7, 0x40);
    board.Write(io + 4, 0x40);
    board.Write(io + 5, tx.size());
    board.Write(io + 6, 0);
    board.Write(io, 0x26);
    assert(board.sent == std::vector<uint8_t>(tx.begin(), tx.end()));
    assert(board.Read(io + 7) == 2 && !pxaGpioInputLevel(board.gpio, 105));
    board.Write(io + 7, 2);

    board.Write(io + 1, 0x46);
    board.Write(io + 2, 0x50);
    board.Write(io + 3, 0x4d);
    board.Write(io + 0xc, 4);
    board.Write(io, 0x62);
    for (unsigned i = 0; i < SpitzCfEthernet::mac.size(); ++i)
        board.Write(io + i + 1, SpitzCfEthernet::mac[i]);
    board.Write(io + 7, 0x4f);
    board.Write(io, 0x22);

    std::array<uint8_t, 300> rx{};
    rx.front() = 4;
    assert(!spitzPcmciaEthernetReceive(board.slot, rx.data(), rx.size()));
    rx.front() = 0xff;
    std::fill_n(rx.begin(), 6, 0xff);
    for (unsigned i = 6; i < rx.size(); ++i) rx[i] = uint8_t(i);
    assert(spitzPcmciaEthernetReceive(board.slot, rx.data(), rx.size()));
    assert(!pxaGpioInputLevel(board.gpio, 105));
    board.Dma(0x4f00, 304, false);
    assert(board.Read(io + 0x10, 2) == 0x4721);
    assert(board.Read(io + 0x10, 2) == 304);
    for (unsigned i = 0; i < 246; i += 2)
        assert(board.Read(io + 0x10, 2) == (rx[i] | (uint16_t(rx[i + 1]) << 8)));

    Savestate<ChunkType> saved;
    assert(saved.Save(board));
    Board resumed;
    SavestateLoader<ChunkType> loader;
    assert(loader.Load(saved.GetBuffer(), saved.GetSize(), resumed));
    assert(spitzPcmciaEthernetInserted(resumed.slot));
    assert(!pxaGpioInputLevel(resumed.gpio, 94) && !pxaGpioInputLevel(resumed.gpio, 105));
    for (unsigned i = 246; i < rx.size(); i += 2)
        assert(resumed.Read(io + 0x10, 2) == (rx[i] | (uint16_t(rx[i + 1]) << 8)));
    assert(resumed.Read(io + 7) == 0x41);
    resumed.Write(io + 7, 0x41);
    assert(pxaGpioInputLevel(resumed.gpio, 105));
    spitzPcmciaEjectEthernet(resumed.slot);
    assert(pxaGpioInputLevel(resumed.gpio, 94));
    assert(resumed.Read(io) == 0xff);
}
}  // namespace

int main() {
    testEthernet();
    std::puts("PASS: Spitz CF Ethernet, PROM probe, 16-bit DMA, transmit, receive ring, IRQ and restore");
}
