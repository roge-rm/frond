// The i.MX1 MMCSD with a card in the virtual slot: commands and their responses on the card clock,
// block reads and writes through a card DMA channel, card errors and the guarded edges.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "imx1_bootstrap.h"
#include "imx1_mmcsd.h"
#include "vSD.h"

static std::vector<uint8_t> disk(2048 * 512);
static uint32_t failSector = ~0u;
static bool readSector(uint32_t sector, void* buf) {
    if (sector == failSector || sector >= 2048) return false;
    std::memcpy(buf, disk.data() + sector * 512, 512);
    return true;
}
static bool writeSector(uint32_t sector, const void* buf) {
    if (sector == failSector || sector >= 2048) return false;
    std::memcpy(disk.data() + sector * 512, buf, 512);
    return true;
}

int main() {
    using A = Imx1BootstrapAccess;
    constexpr uint32_t mmc = 0x00214000, sram = 0x00300000, dma = 0x00209000;
    constexpr uint32_t fifo = mmc + 0x38;
    for (size_t i = 0; i < disk.size(); ++i) disk[i] = uint8_t(i * 7 + (i >> 9));
    VSD* card = vsdInit(readSector, writeSector, 2048);
    assert(card);
    Imx1Bootstrap soc;
    auto read = [&](uint32_t address) {
        uint32_t value = 0xdeadbeef;
        assert(soc.access(address,4,false,value) == A::ok);
        return value;
    };
    auto expect = [&](uint32_t address, uint32_t expected) {
        const uint32_t value = read(address);
        if (value != expected) fprintf(stderr, "%08x: %08x, expected %08x\n", address, value, expected);
        assert(value == expected);
    };
    auto write = [&](uint32_t address, uint32_t value, A expected = A::ok) {
        const auto got = soc.access(address,4,true,value);
        if (got != expected) fprintf(stderr, "write %08x: %d, expected %d\n", address, int(got), int(expected));
        assert(got == expected);
    };
    write(0x00210000,0xffffc400); write(0x00210004,0xfffffbff);
    soc.setMmcsdEmptySlot(true);
    soc.attachMmcsdCard(card);
    assert(soc.mmcsdCard() == card);
    write(mmc,8); write(mmc,0xd);
    for (unsigned i = 0; i < 8; ++i) write(mmc,5);
    // CLK_RATE 0: one card clock a HCLK here, as in imx1-mmcsd.
    write(mmc+8,0); write(mmc+0x10,0x40); write(mmc+0x14,0xffff); write(mmc+0x24,0x7f);
    // Runs a command and checks it ends exactly after clocks card clocks. Returns STATUS.
    auto command = [&](uint32_t index, uint32_t argument, uint32_t format, uint32_t clocks) {
        write(mmc+0x28,index); write(mmc+0x2c,argument >> 16); write(mmc+0x30,argument & 0xffff);
        write(mmc+0xc,format); write(mmc,6);
        soc.advanceHclkCycles(clocks - 1);
        expect(mmc+4,0x100);
        soc.advanceHclkCycles(1);
        return read(mmc+4);
    };
    auto stop = [&] { write(mmc,5); soc.advanceHclkCycles(8); expect(mmc+4,0); };
    auto halfwords = [&](unsigned count) {
        std::vector<uint32_t> out;
        for (unsigned i = 0; i < count; ++i) out.push_back(read(mmc+0x34));
        uint32_t value = 0;
        assert(soc.access(mmc+0x34,4,false,value) == A::sideEffect);
        return out;
    };
    // CMD0 with INIT: 80 extra clocks, no response.
    assert(command(0,0,0x80,48+80) == 0x2100);
    { uint32_t value = 0; assert(soc.access(mmc+0x34,4,false,value) == A::sideEffect); }
    stop();
    // ACMD41 until the card is ready; the R3 OCR comes first bits first, two bytes a halfword.
    bool ready = false;
    for (unsigned i = 0; i < 100 && !ready; ++i) {
        assert(command(55,0,1,48+2+48) == 0x2100);
        const auto r1 = halfwords(3);
        assert(r1[0] == 0x3700);
        stop();
        assert(command(41,0x00ff8000,3,48+2+48) == 0x2100);
        const auto r3 = halfwords(3);
        assert((r3[0] >> 8) == 0x3f);
        ready = r3[0] & 0x80;
        stop();
    }
    assert(ready);
    // CMD2: R2, 8 halfwords with the CRC byte dropped.
    assert(command(2,0,2,48+2+136) == 0x2100);
    const auto cid = halfwords(8);
    assert((cid[0] >> 8) == 0x3f);
    stop();
    assert(command(3,0,6,48+2+48) == 0x2100);
    const auto r6 = halfwords(3);
    assert((r6[0] >> 8) == 3);
    const uint32_t rca = (r6[0] & 0xff) << 8 | r6[1] >> 8;
    assert(rca);
    stop();
    assert(command(9,rca << 16,2,48+2+136) == 0x2100);
    halfwords(8);
    stop();
    // A response format the card doesn't answer with times out after RES_TO.
    assert(command(9,rca << 16,1,48+0x40) == 0x2102);
    halfwords(0);
    stop();
    assert(command(7,rca << 16,1,48+2+48) == 0x2100);
    halfwords(3);
    stop();
    assert(command(16,512,1,48+2+48) == 0x2100);
    stop();
    // Guarded edges: stream mode, R4/R5, no block length or count, oversized blocks.
    write(mmc+0x18,512); write(mmc+0x1c,1);
    write(mmc+0x28,17); write(mmc+0x2c,0); write(mmc+0x30,0);
    for (uint32_t format : {0x29u,4u,5u}) { write(mmc+0xc,format); write(mmc,6,A::sideEffect); }
    write(mmc+0x1c,0); write(mmc+0xc,9); write(mmc,6,A::sideEffect);
    write(mmc+0x1c,1); write(mmc+0x18,0); write(mmc+0xc,9); write(mmc,6,A::sideEffect);
    write(mmc+0x18,513,A::ok); write(mmc+0xc,9); write(mmc,6,A::sideEffect);
    write(mmc+0x18,512);
    // DMA: a card channel needs the card's request line, a FIFO end and memory the DMA can reach.
    write(dma,1);
    constexpr uint32_t ch9 = dma + 0x80 + 9*0x40, ch8 = dma + 0x80 + 8*0x40;
    auto channel = [&](uint32_t base, uint32_t source, uint32_t destination, uint32_t count) {
        write(base,source); write(base+4,destination); write(base+8,count);
        write(base+0x10,13); write(base+0x14,0);
    };
    channel(ch9,fifo,sram,512);
    write(ch9+0x10,12); write(ch9+0xc,0x829,A::sideEffect);
    write(ch9+0x10,13);
    write(ch9+4,0x00200000); write(ch9+0xc,0x829,A::memoryUnavailable);
    write(ch9+4,sram);
    // Single block read, 1-bit: the block's clocks follow the response, then the DMA takes it.
    std::memset(soc.plainPage(sram,true),0xa5,0x1000);
    write(dma+4,0x3ff);
    assert(command(17,5*512,9,48+2+48) == 0x2100);
    write(ch9+0xc,0x828); write(ch9+0xc,0x829);
    write(ch9,sram,A::sideEffect);
    const uint32_t blockClocks = 2 + 512*8 + 18;
    soc.advanceHclkCycles(blockClocks - 1);
    expect(mmc+4,0x2100);
    assert(soc.plainPage(sram,false)[0] == 0xa5);
    assert(!(read(dma+4) & (1u << 9)));
    soc.advanceHclkCycles(1);
    expect(mmc+4,0x2900);
    assert(!std::memcmp(soc.plainPage(sram,false), disk.data() + 5*512, 512));
    assert(soc.plainPage(sram,false)[512] == 0xa5);
    assert(read(dma+4) & (1u << 9));
    write(dma+4,1u << 9); assert(!(read(dma+4) & (1u << 9)));
    stop();
    // DATA_TRAN unmasked raises IRQ 35 at the data's end, not at the response.
    write(0x00223008,35); write(mmc+0x24,0x7e);
    assert(command(17,6*512,9,48+2+48) == 0x2100);
    assert(!(soc.interruptPending().normal & (uint64_t(1) << 35)));
    write(ch9+0xc,0x829);
    soc.advanceHclkCycles(blockClocks);
    expect(mmc+4,0x2900);
    assert(soc.interruptPending().normal & (uint64_t(1) << 35));
    assert(!std::memcmp(soc.plainPage(sram,false), disk.data() + 6*512, 512));
    write(mmc+0x24,0x7f);
    stop();
    // Two blocks on the 4-bit bus with the DMA enabled before the data: it waits for them.
    write(mmc+0x1c,2); write(ch9+8,1024); write(ch9+0xc,0x828);
    assert(command(18,10*512,0x209,48+2+48) == 0x2100);
    write(ch9+0xc,0x829);
    soc.advanceHclkCycles(2 + 2*(512*2 + 18) - 1);
    expect(mmc+4,0x2100);
    soc.advanceHclkCycles(1);
    expect(mmc+4,0x2900);
    assert(!std::memcmp(soc.plainPage(sram,false), disk.data() + 10*512, 1024));
    stop();
    assert(command(12,0,1,48+2+48) == 0x2100);
    stop();
    // Single block write from SRAM: DATA_TRANS_DONE and WRITE_OP_DONE, the card holds the block.
    write(mmc+0x1c,1);
    uint8_t* page = soc.plainPage(sram,true);
    for (unsigned i = 0; i < 512; ++i) page[0x800 + i] = uint8_t(i ^ 0x5a);
    channel(ch8,sram+0x800,fifo,512);
    assert(command(24,20*512,0x19,48+2+48) == 0x2100);
    write(ch8+0xc,0x2088); write(ch8+0xc,0x2089);
    soc.advanceHclkCycles(2 + 512*8 + 18 + 16 - 1);
    expect(mmc+4,0x2100);
    soc.advanceHclkCycles(1);
    expect(mmc+4,0x3900);
    assert(!std::memcmp(disk.data() + 20*512, page + 0x800, 512));
    assert(read(dma+4) & (1u << 8));
    stop();
    // A card that can't read the block ends the transfer with CRC_READ_ERR.
    failSector = 7; write(dma+4,0x3ff);
    write(ch9+8,512); write(ch9+0xc,0x828);
    assert(command(17,7*512,9,48+2+48) == 0x2100);
    write(ch9+0xc,0x829);
    soc.advanceHclkCycles(blockClocks);
    expect(mmc+4,0x2908);
    assert(!(read(dma+4) & (1u << 9)));
    write(ch9+0xc,0x828);
    stop();
    failSector = ~0u;
    // A stop while the data waits drops it; the DMA stays where it was.
    write(dma+4,0x3ff);
    assert(command(17,8*512,9,48+2+48) == 0x2100);
    soc.advanceHclkCycles(blockClocks);
    stop();
    write(ch9+0xc,0x829);
    assert(!(read(dma+4) & (1u << 9)));
    write(ch9+0xc,0x828);
    // Without a card the card channels are refused.
    soc.attachMmcsdCard(nullptr);
    assert(!soc.mmcsdCard());
    write(ch9+0xc,0x829,A::sideEffect);
    puts("i.MX1 MMCSD card commands, responses, block DMA and errors: PASS");
}
