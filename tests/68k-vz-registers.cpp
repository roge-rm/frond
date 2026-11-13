#include <cassert>
#include <cstring>
#include <cstdio>
#include "EmRegsVZ.h"
#include "EmSession.h"
#include "EmBankMapped.h"
#include "EmSystemState.h"
#include "Frame.h"
#include "Lcd.h"
#include "EmSPISlaveADS784x.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"
#include "savestate/SavestateProbe.h"
#include <vector>

class Board : public EmRegsVZ {
 public:
    Board() { fPortDDataCount = 0xffffffff; SetSubBankHandlers(); }
    Bool GetLCDScreenOn() override { return true; }
    Bool GetLCDBacklightOn() override { return false; }
    void GetKeyInfo(int* r, int* c, uint16*, Bool*) override { *r = *c = 0; }
    uint8 GetPortInputValue(int) override { return pins; }
    uint8 GetPortInternalValue(int) override { return pins; }
    bool PortDPolarityInverts() const override { return inverts; }
    void Put(emuptr addr, int size, uint32 value) { StdWrite(addr, size, value); }
    uint8 ReadD() { return GetByte(0xfffff419); }
    void UpdateD() { UpdatePortDInterrupts(); }
    int32 Level() { return GetInterruptLevel(); }
    void ApplySdctl() override {}
    void TimerOrigin(double cycle) {
        tmr1LastProcessedSystemCycles = cycle;
        tmr2LastProcessedSystemCycles = cycle;
        systemCycles = 0;
    }
    uint64 NextTimer() const { return nextTimerEventAfterCycle; }
    std::vector<double> TimerState() {
        return {double(GetWord(0xfffff608)), double(GetWord(0xfffff60a)),
                double(GetWord(0xfffff312)), double(GetWord(0xfffff30e)),
                double(nextTimerEventAfterCycle), tmr1LastProcessedSystemCycles,
                tmr2LastProcessedSystemCycles};
    }
    uint8 pins = 0;
    bool inverts = true;
};

class Display : public EmHALHandler {
 public:
    Bool GetAsleep() override { return false; }
    Bool GetLCDBacklightOn() override { return false; }
    uint16 GetLCD2bitMapping() override { return 0xfa50; }
    bool CopyLCDFrame(Frame& f, bool) override {
        f.lineWidth = width; f.lines = 160; f.scaleX = f.scaleY = scale;
        f.bpp = depth; f.margin = margin; f.bytesPerLine = stride;
        memset(f.GetBuffer(), 0, f.GetBufferSize());
        f.GetBuffer()[159 * 71 + 70] = 0x02;
        if (scale == 2) f.GetBuffer()[0] = 0x0f;  // 4 bpp: pixel 1 of row 0 at full ink
        return true;
    }
    unsigned width = 560, depth = 1, margin = 7, stride = 71, scale = 1;
};

static void CheckDisplay() {
    Display display;
    Lcd68k wide(560,160);
    assert(wide.Update());
    const auto* p = wide.Pixels();
    // The last pixel after seven panning bits must decode on the final row.
    assert(memcmp(p, p + (560 * 160 - 1) * 4, 4) != 0);
    assert(p[(560 * 160 - 1) * 4 + 3] == 255);
    display.stride = 70;
    assert(!wide.Update());
    display.stride = 71; display.depth = 8;
    assert(!wide.Update());
    display.depth = 1; display.width = 160; display.margin = 0; display.stride = 20;
    Lcd68k classic;
    assert(classic.Update());
    assert(!wide.Update());
    // The T415's 160x160 mode is pixel-doubled on its 320x320 panel; other scales are refused.
    display.depth = 4; display.stride = 80; display.scale = 2;
    Lcd68k t415(320, 320, false, Lcd68k::kT415Panel);
    assert(t415.Update() && !classic.Update());
    const auto* q = t415.Pixels();
    auto px = [&](int x, int y) { return q + (y * 320 + x) * 4; };
    assert(memcmp(px(0, 0), px(1, 1), 4) == 0 && memcmp(px(2, 0), px(3, 1), 4) == 0);
    assert(memcmp(px(1, 0), px(2, 0), 4) != 0 && memcmp(px(2, 2), px(4, 4), 4) == 0);
    assert(px(2, 0)[0] < px(0, 0)[0]);
    display.scale = 3;
    assert(!t415.Update());
}

static void CheckAdcState() {
    // Use the existing generic undocked channel to exercise a nonzero shift reply.
    // This fixture does not establish Dana's channel wiring or voltages.
    for (unsigned split = 1; split < 8; ++split) {
        EmSPISlaveADS784x adc(kChannelSet3);
        adc.DoExchange(split - 1, 0xe4 >> (8 - split));
        Savestate<ChunkType> partial;
        assert(partial.Save(adc));
        adc.DoExchange(7 - split, 0xe4);
        assert(adc.DoExchange(15, 0) == 0x7ff8);
        SavestateLoader<ChunkType> loader;
        assert(loader.Load(partial.GetBuffer(), partial.GetSize(), adc));
        adc.DoExchange(7 - split, 0xe4);
        Savestate<ChunkType> conversion;
        assert(conversion.Save(adc));
        assert(adc.DoExchange(7, 0) == 0x7f);
        Savestate<ChunkType> reply;
        assert(reply.Save(adc));
        assert(adc.DoExchange(7, 0) == 0xf8);
        assert(loader.Load(reply.GetBuffer(), reply.GetSize(), adc));
        assert(adc.DoExchange(7, 0) == 0xf8);
        assert(loader.Load(conversion.GetBuffer(), conversion.GetSize(), adc));
        assert(adc.DoExchange(15, 0) == 0x7ff8);
        // Corrupt the two counters in the serialized ADC chunk.
        auto* raw = static_cast<uint32_t*>(partial.GetBuffer());
        assert(raw[0] == 1 && raw[1] == uint32_t(ChunkType::spiSlaveADS784));
        for (unsigned field : {5u, 6u}) {
            auto saved = raw[field];
            for (uint32_t bad : {(split + 1) % 8, 8u, 32u, 0xffffffffu}) {
                raw[field] = bad;
                assert(!loader.Load(partial.GetBuffer(), partial.GetSize(), adc));
            }
            raw[field] = saved;
        }
        assert(loader.Load(partial.GetBuffer(), partial.GetSize(), adc));
    }
}

static void CheckTimerLoadClock() {
    Board board;
    // The session normally subscribes during Initialize; this fixture has no ROM.
    auto clock = EmHAL::onSystemClockChange.AddHandler([] { gSession->SetClockFactor(1); });
    gSession->SetClockFactor(1);
    board.Initialize();
    board.Put(0xfffff200, 2, 0x2483);
    board.Put(0xfffff202, 2, 0x0347);
    gSession->SetClockFactor(1);
    assert(gSession->GetClocksPerSecond() == 33161216);
    board.Put(0xfffff600, 2, 0x13);
    board.Put(0xfffff602, 2, 5);
    board.Put(0xfffff604, 2, 1000);
    board.Put(0xfffff608, 2, 7);
    board.Put(0xfffff306, 2, 0);
    board.TimerOrigin(-600);
    Savestate<ChunkType> lazy;
    assert(lazy.Save(board));
    // Read at the saved cycle, then continue across the next timer interrupt.
    assert(board.GetWord(0xfffff608) == 107);
    assert(board.NextTimer() == 5358);
    std::vector<std::vector<double>> reference;
    for (uint64 cycle : {5357u, 5358u, 5364u}) {
        board.Cycle(cycle, false);
        board.GetWord(0xfffff608);
        assert((board.Level() == 6) == (cycle == 5364));
        reference.push_back(board.TimerState());
    }
    // Rebuild starts with the reset PLL. Loading must restore the clock before ticking.
    board.Put(0xfffff200, 2, 0x24b3);
    board.Put(0xfffff202, 2, 0x0123);
    gSession->SetClockFactor(1);
    assert(gSession->GetClocksPerSecond() == 8290304);
    SavestateLoader<ChunkType> loader;
    assert(loader.Load(lazy.GetBuffer(), lazy.GetSize(), board));
    assert(board.GetWord(0xfffff608) == 107);
    assert(gSession->GetClocksPerSecond() == 33161216);
    assert(board.NextTimer() == 5358);
    unsigned i = 0;
    for (uint64 cycle : {5357u, 5358u, 5364u}) {
        board.Cycle(cycle, false);
        board.GetWord(0xfffff608);
        assert(board.TimerState() == reference[i]);
        ++i;
    }
    board.Dispose();
    EmHAL::onSystemClockChange.RemoveHandler(clock);
    puts("VZ load clock, lazy counter and next interrupt continuation passed");
}

int main() {
    CheckTimerLoadClock();
    // Drive the VZ registers without running guest firmware.
    Board board;
    board.Put(0xfffff314, 2, 0);
    board.Put(0xfffff30c, 4, 0);
    assert(board.Level() == -1);
    board.Put(0xfffff314, 2, 0x6533);
    board.Put(0xfffff418, 1, 0);
    board.Put(0xfffff41b, 1, 0xff);
    for (unsigned pins = 0; pins < 256; ++pins) {
        board.pins = pins;
        for (unsigned polarity = 0; polarity < 256; ++polarity) {
            board.Put(0xfffff41c, 1, polarity);
            assert(board.ReadD() == (pins ^ (polarity & 15)));
        }
    }
    board.Put(0xfffff418, 1, 0xff);
    board.Put(0xfffff419, 1, 0xa5);
    board.Put(0xfffff41c, 1, 0xff);
    assert(board.ReadD() == 0xa5);
    board.pins = 0x5a;
    for (unsigned dir = 0; dir < 256; ++dir) {
        board.Put(0xfffff418, 1, dir);
        assert(board.ReadD() == ((0xa5 & dir) | ((0x5a ^ 15) & ~dir & 255)));
    }
    board.Put(0xfffff418, 1, 0);
    board.Put(0xfffff41d, 1, 15);
    board.Put(0xfffff41e, 1, 0);
    board.Put(0xfffff41f, 1, 0);
    board.Put(0xfffff306, 2, 0);
    for (unsigned pins = 0; pins < 16; ++pins) {
        board.pins = pins;
        for (unsigned pol = 0; pol < 16; ++pol) {
            board.SetByte(0xfffff41c, pol);
            unsigned expected = ((pins ^ pol) & 15) << 8;
            assert((board.GetWord(0xfffff312) & 0x0f00) == expected);
            assert((board.GetWord(0xfffff30e) & 0x0f00) == expected);
        }
    }
    // Cloudpilot's Palm boards (m125, m500) report keys active-high: reads ignore PDPOL and a
    // level IRQ is pending when the input matches it.
    board.inverts = false;
    for (unsigned pins = 0; pins < 16; ++pins) {
        board.pins = pins;
        for (unsigned pol = 0; pol < 16; ++pol) {
            board.SetByte(0xfffff41c, pol);
            assert(board.ReadD() == pins);
            assert((board.GetWord(0xfffff312) & 0x0f00) == ((~(pins ^ pol) & 15) << 8));
        }
    }
    board.inverts = true;
    // Output pins and disabled requests cannot leave a level IRQ pending.
    board.SetByte(0xfffff418, 15);
    assert(!(board.GetWord(0xfffff312) & 0x0f00));
    board.SetByte(0xfffff418, 0); board.SetByte(0xfffff41d, 0);
    assert(!(board.GetWord(0xfffff312) & 0x0f00));
    board.Put(0xfffff41d, 1, 15); board.Put(0xfffff41c, 1, 0);
    board.Put(0xfffff306, 2, 0x0f00); board.UpdateD();
    assert((board.GetWord(0xfffff312) & 0x0f00) == 0x0f00);
    assert(!(board.GetWord(0xfffff30e) & 0x0f00));
    EmBankMapped::Initialize();
    EmBankMapped::SetBankHandlers();
    uint8 pixels[11200];
    memset(pixels, 0x5a, sizeof pixels);
    EmBankMapped::MapPhysicalMemory(pixels, sizeof pixels);
    const emuptr base = EmBankMapped::GetEmulatedAddress(pixels);
    board.Put(0xfffffa00, 4, base);
    board.Put(0xfffffa05, 1, 35);
    board.Put(0xfffffa08, 2, 560);
    board.Put(0xfffffa0a, 2, 159);
    board.Put(0xfffffa20, 1, 0);
    board.Put(0xfffffa2d, 1, 0);
    Frame frame(sizeof pixels);
    memset(frame.GetBuffer(), 0xcc, frame.GetBufferSize());
    gSystemState.MarkScreenClean();
    gSystemState.MarkScreenDirty(base + 70 * 159, base + sizeof pixels - 1);
    // A final dirty row must still fit in the entire destination.
    Frame small(sizeof pixels - 1);
    memset(small.GetBuffer(), 0xcc, small.GetBufferSize());
    assert(!board.CopyLCDFrame(small, false));
    for (size_t i = 0; i < small.GetBufferSize(); ++i) assert(small.GetBuffer()[i] == 0xcc);
    board.Put(0xfffffa05, 1, 0);
    assert(!board.CopyLCDFrame(frame, false));
    for (size_t i = 0; i < frame.GetBufferSize(); ++i) assert(frame.GetBuffer()[i] == 0xcc);
    board.Put(0xfffffa05, 1, 35);
    assert(board.CopyLCDFrame(frame, false));
    assert(frame.firstDirtyLine == 159 && frame.lastDirtyLine == 159);
    for (size_t i = 0; i < frame.GetBufferSize(); ++i)
        assert(frame.GetBuffer()[i] == (i < 70 * 159 ? 0xcc : 0x5a));
    EmBankMapped::Dispose();
    CheckDisplay();
    CheckAdcState();
    puts("VZ polarity, frame register and LCD geometry cases passed");
}
