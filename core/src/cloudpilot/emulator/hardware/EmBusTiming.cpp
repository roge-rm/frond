#include "EmBusTiming.h"

#include <algorithm>
#include <iterator>

namespace {
    // A CAS-before-RAS refresh holds the DRAM for about 240 ns, four clocks at 16.58 MHz (EZ
    // manual 14.1.2 and figure 18-8: a colliding CPU access waits 1-4 clocks).
    constexpr uint32_t kRefreshClocks = 4;

    // The EZ's LCD controller cycle lasts at most eight clocks (EZ manual 14.1.4), so a burst
    // carries as many words as fit in eight clocks.
    constexpr uint32_t kEzLcdCycleClocks = 8;

    uint32_t BitsPerPixel(uint8_t panelControl) {
        switch (panelControl & 3) {
            case 1:
                return 2;
            case 2:
                return 4;
            default:
                return 1;
        }
    }

    // 16-bit (or 8-bit with DWIDTH) DMA transfers per line, with one more when the line is
    // panned off a word boundary.
    uint32_t LineTransfers(uint16_t width, uint8_t panelControl, uint8_t clockControl,
                           uint8_t panning) {
        const uint32_t bits = uint32_t(width) * BitsPerPixel(panelControl);
        const uint32_t unit = (clockControl & 0x40) ? 8 : 16;
        return (bits + unit - 1) / unit + ((panning & 0x0f) ? 1 : 0);
    }

    // Clocks for a line's DMA split into bursts of up to burstWords, where each burst takes
    // first clocks for its first word and next for each further word.
    uint32_t BurstClocks(uint32_t transfers, uint32_t burstWords, uint32_t first, uint32_t next) {
        burstWords = std::max<uint32_t>(burstWords, 1);
        const uint32_t bursts = (transfers + burstWords - 1) / burstWords;
        return bursts * first + (transfers - bursts) * next;
    }

    // System clocks per LCD clock, from the PLL's two dividers of DMACLK.
    double LcdClockInSystemClocks(uint16_t pllControl) {
        return double(EmDragonballClockDivider((pllControl >> 11) & 7)) /
               EmDragonballClockDivider((pllControl >> 8) & 7);
    }

    // DRAMMC REF with DRAMC CLK (both chips' manuals, DRAMMC description).
    uint32_t RefreshPeriod(uint16_t dramConfig, uint16_t dramControl, uint32_t refMask,
                           uint32_t systemClockHz) {
        const uint32_t ref = dramConfig & refMask;
        if (dramControl & 0x0800) return 32 * (ref + 1);
        const double rate = ref == 0 ? 2 * 32768. : ref == 1 ? 32768. : 32768. / (ref + 1);
        return uint32_t(systemClockHz / rate + 0.5);
    }

    EmBusSteal gSteal;
    uint64_t gNextLcd = ~uint64_t(0);
    uint64_t gNextRefresh = ~uint64_t(0);

    // Moves an event time to the first one at or after start, keeping its phase.
    uint64_t CatchUp(uint64_t next, uint32_t period, uint64_t start) {
        if (next >= start) return next;
        return next + ((start - next + period - 1) / period) * uint64_t(period);
    }

    // Charges every event of one kind due in [start, end]; share is fixed point, 1 << 16 whole.
    uint64_t Due(uint64_t& next, uint32_t period, uint32_t length, uint64_t start, uint64_t end,
                 uint32_t share, uint32_t& fraction) {
        next = CatchUp(next, period, start);
        uint64_t total = 0;
        while (next <= end) {
            total += uint64_t(length) * share;
            next += period;
        }
        total += fraction;
        fraction = uint32_t(total & 0xffff);
        return total >> 16;
    }

    uint32_t gLcdFraction, gRefreshFraction;

    EmSdramTiming gSdram;
    uint32_t gOpenRow[4] = {~0u, ~0u, ~0u, ~0u};
    uint32_t gLcdBank = ~0u;

    uint32_t SdramBank(uint32_t address) {
        return (gSdram.bankHigh == 0xff ? 0 : ((address >> gSdram.bankHigh) & 1) << 1) |
               (gSdram.bankLow == 0xff ? 0 : (address >> gSdram.bankLow) & 1);
    }
}  // namespace

uint32_t EmDragonballClockDivider(uint32_t select) {
    return (select & 4) ? 1 : 2u << (select & 3);
}

EmBusSteal EmEzBusSteal(const EmEzBusRegisters& r) {
    EmBusSteal steal;

    // LCD: LRRA's frame period (EZ manual 12.2.14) over the screen's lines, and the line's DMA
    // from DRAM (DRAMC: first word 4 clocks, one more with MSW, then 1 with EDO or BC + 2 in
    // fast page mode) or from SRAM/ROM (DWS + 1 clocks a transfer).
    if ((r.lcdClockControl & 0x80) && r.lcdScreenWidth) {
        const double line = (6. + r.lcdRefreshRateAdj + r.lcdScreenWidth) *
                            ((r.lcdPixelClock & 0x3f) + 1) * LcdClockInSystemClocks(r.pllControl);
        const uint32_t transfers = LineTransfers(r.lcdScreenWidth, r.lcdPanelControl,
                                                 r.lcdClockControl, r.lcdPanningOffset);
        uint32_t first, next;
        if (r.lcdInDram && r.dramEnabled) {
            first = 4 + ((r.dramControl >> 5) & 1);
            next = (r.dramControl & 0x0400) ? 1 : ((r.dramControl >> 12) & 3) + 2;
        } else {
            first = next = (r.lcdClockControl & 0x0f) + 1;
        }
        const uint32_t burst = first >= kEzLcdCycleClocks ? 1 : 1 + (kEzLcdCycleClocks - first) / next;
        steal.lcdLineClocks = std::max<uint32_t>(uint32_t(line + 0.5), 1);
        steal.lcdDmaClocks = BurstClocks(transfers, burst, first, next);
    }

    // CAS-before-RAS refresh while the DRAM controller runs and isn't in self refresh.
    if (r.dramEnabled && (r.dramControl & 0x8000) && !(r.dramControl & 0x4000)) {
        steal.refreshPeriod = std::max<uint32_t>(
            RefreshPeriod(r.dramConfig, r.dramControl, 0x0f, r.systemClockHz), 1);
        steal.refreshClocks = kRefreshClocks;
    }

    return steal;
}

EmBusSteal EmVzBusSteal(const EmVzBusRegisters& r) {
    EmBusSteal steal;

    // LCD: LRRA's frame period (VZ manual 8.3.14), DMACR's burst length. SDRAM bursts are
    // taken like EDO: a page miss for the first word, then one clock a word.
    if ((r.lcdClockControl & 0x80) && r.lcdScreenWidth) {
        const double line = (12. + (r.lcdRefreshRateAdj & 0x3ff) + r.lcdScreenWidth) *
                            ((r.lcdPixelClock & 0x3f) + 1) * LcdClockInSystemClocks(r.pllControl);
        const uint32_t transfers = LineTransfers(r.lcdScreenWidth, r.lcdPanelControl,
                                                 r.lcdClockControl, r.lcdPanningOffset);
        uint32_t first, next;
        if (r.lcdInDram && r.dramEnabled && r.sdram) {
            first = 4 + ((r.sdramControl >> 1) & 1);
            next = 1;
        } else if (r.lcdInDram && r.dramEnabled) {
            first = 4 + ((r.dramControl >> 5) & 1);
            next = (r.dramControl & 0x0400) ? 1 : ((r.dramControl >> 12) & 3) + 2;
        } else {
            first = next = (r.lcdClockControl & 0x0f) + 1;
        }
        const uint32_t burst = (r.lcdDmaControl >> 4) & 0x0f;
        steal.lcdLineClocks = std::max<uint32_t>(uint32_t(line + 0.5), 1);
        steal.lcdDmaClocks = BurstClocks(transfers, burst ? burst : 1, first, next);
    }

    // DRAM refresh as on the EZ; SDRAM auto refresh when SDCTRL RE is set, holding the SDRAM
    // for the refresh to active latency (RACL: 3 or 6 clocks).
    if (r.dramEnabled && r.sdram) {
        if (r.sdramControl & 0x1000) {
            steal.refreshPeriod = std::max<uint32_t>(
                RefreshPeriod(r.dramConfig, r.dramControl, 0x1f, r.systemClockHz), 1);
            steal.refreshClocks = (r.sdramControl & 1) ? 6 : 3;
        }
    } else if (r.dramEnabled && (r.dramControl & 0x8000) && !(r.dramControl & 0x4000)) {
        steal.refreshPeriod = std::max<uint32_t>(
            RefreshPeriod(r.dramConfig, r.dramControl, 0x1f, r.systemClockHz), 1);
        steal.refreshClocks = kRefreshClocks;
    }

    return steal;
}

EmSdramTiming EmVzSdramTiming(uint16_t sdramControl, uint16_t dramControl) {
    static constexpr uint8_t kHigh[] = {20, 22, 24, 0xff}, kLow[] = {19, 21, 23, 0xff};
    EmSdramTiming t;
    t.continuousPage = sdramControl & 0x4000;
    t.casLatency2 = sdramControl & 0x0002;
    t.bankHigh = kHigh[(sdramControl >> 4) & 3];
    t.bankLow = kLow[(sdramControl >> 2) & 3];
    // PGSZ: 256 to 2048 words, so 9 to 12 address bits.
    t.pageShift = 9 + ((dramControl >> 8) & 3);
    return t;
}

namespace EmSdram {
    void Set(const EmSdramTiming& timing) {
        gSdram = timing;
        PrechargeAll();
    }

    uint32_t Access(uint32_t address, bool write, uint32_t words) {
        const uint32_t hit = write ? 0 : 1 + gSdram.casLatency2;
        const uint32_t miss = write ? 1 : 3 + gSdram.casLatency2;
        if (!gSdram.continuousPage) return words * miss;
        const uint32_t bank = SdramBank(address);
        const uint32_t row = address >> gSdram.pageShift;
        const uint32_t first = gOpenRow[bank] == row ? hit : miss;
        gOpenRow[bank] = row;
        return first + (words - 1) * hit;
    }

    const EmSdramTiming& Get() { return gSdram; }

    uint32_t ReadFirst(uint32_t writeWait, uint32_t words) {
        const uint32_t hit = 1 + gSdram.casLatency2, miss = 3 + gSdram.casLatency2;
        // Without continuous page mode every access is a miss, a write one clock.
        if (!gSdram.continuousPage) return words * (miss + 1);
        return (writeWait ? miss : hit) + (words - 1) * hit;
    }

    void SetLcdAddress(uint32_t address) { gLcdBank = address == ~0u ? ~0u : SdramBank(address); }

    void PrechargeAll() { std::fill(std::begin(gOpenRow), std::end(gOpenRow), ~0u); }
}  // namespace EmSdram

extern "C" uint32_t EmSdramAccess(uint32_t address, int write, int words) {
    return EmSdram::Access(address, write, words);
}

extern "C" uint32_t EmSdramReadFirst(uint32_t writeWait, int words) {
    return EmSdram::ReadFirst(writeWait, words);
}

namespace EmBusTiming {
    uint64_t next = ~uint64_t(0);
    uint64_t windowCount;
    uint32_t windowWait, windowCycles;

    namespace {
        void ClearWindow() {
            windowCount = 0;
            windowWait = windowCycles = 0;
        }
    }  // namespace

    void Set(const EmBusSteal& steal, uint64_t now) {
        // Keeps an event's phase while its period stays and it is due within one period.
        const bool lcdKept = steal.lcdLineClocks == gSteal.lcdLineClocks &&
                             gNextLcd != ~uint64_t(0) && gNextLcd <= now + steal.lcdLineClocks;
        const bool refreshKept = steal.refreshPeriod == gSteal.refreshPeriod &&
                                 gNextRefresh != ~uint64_t(0) &&
                                 gNextRefresh <= now + steal.refreshPeriod;
        gSteal = steal;
        if (!steal.lcdLineClocks || !steal.lcdDmaClocks)
            gNextLcd = ~uint64_t(0);
        else if (!lcdKept)
            gNextLcd = now + steal.lcdLineClocks;
        if (!steal.refreshPeriod || !steal.refreshClocks)
            gNextRefresh = ~uint64_t(0);
        else if (!refreshKept)
            gNextRefresh = now + steal.refreshPeriod;
        next = std::min(gNextLcd, gNextRefresh);
        ClearWindow();
    }

    const EmBusSteal& Get() { return gSteal; }

    namespace {
        uint32_t SdramConfig() {
            return uint32_t(gSdram.continuousPage) | (uint32_t(gSdram.casLatency2) << 1) |
                   (uint32_t(gSdram.bankHigh) << 8) | (uint32_t(gSdram.bankLow) << 16) |
                   (uint32_t(gSdram.pageShift) << 24);
        }
    }

    State Capture() {
        State state{gSteal, SdramConfig(), gNextLcd, gNextRefresh, windowCount,
                    gLcdFraction, gRefreshFraction, windowWait, windowCycles, {}};
        std::copy(std::begin(gOpenRow), std::end(gOpenRow), state.openRow);
        return state;
    }

    bool Restore(const State& state) {
        const auto validDeadline = [](uint64_t deadline, bool enabled) {
            return (deadline != ~uint64_t(0)) == enabled;
        };
        if (!(state.steal == gSteal) || state.sdramConfig != SdramConfig() ||
            state.lcdFraction > 0xffff || state.refreshFraction > 0xffff ||
            state.wait > state.cycles ||
            !validDeadline(state.nextLcd, gSteal.lcdLineClocks && gSteal.lcdDmaClocks) ||
            !validDeadline(state.nextRefresh, gSteal.refreshPeriod && gSteal.refreshClocks))
            return false;
        for (const auto row : state.openRow)
            if (row != ~0u && (!gSdram.continuousPage || row > (~0u >> gSdram.pageShift)))
                return false;

        gNextLcd = state.nextLcd;
        gNextRefresh = state.nextRefresh;
        gLcdFraction = state.lcdFraction;
        gRefreshFraction = state.refreshFraction;
        windowCount = state.count;
        windowWait = state.wait;
        windowCycles = state.cycles;
        std::copy(std::begin(state.openRow), std::end(state.openRow), gOpenRow);
        next = std::min(gNextLcd, gNextRefresh);
        return true;
    }

    void Reset(uint64_t now) {
        gNextLcd = gNextRefresh = ~uint64_t(0);
        gLcdFraction = gRefreshFraction = 0;
        EmSdram::PrechargeAll();
        Set(gSteal, now);
    }


    void Clear() {
        gSdram = {};
        gLcdBank = ~0u;
        gSteal = {};
        Reset(0);
    }

    uint32_t Charge(uint64_t start, uint64_t end, uint32_t cycles, uint32_t busClocks,
                    uint32_t dramClocks) {
        if (!cycles) return 0;
        const uint32_t busShare = uint32_t((uint64_t(std::min(busClocks, cycles)) << 16) / cycles);
        const uint32_t dramShare = uint32_t((uint64_t(std::min(dramClocks, cycles)) << 16) / cycles);
        uint64_t extra = 0;
        if (gNextLcd != ~uint64_t(0) && gNextLcd <= end) {
            extra += Due(gNextLcd, gSteal.lcdLineClocks, gSteal.lcdDmaClocks, start, end,
                         busShare, gLcdFraction);
            if (gLcdBank != ~0u && gSdram.continuousPage) gOpenRow[gLcdBank] = ~0u;
        }
        if (gNextRefresh != ~uint64_t(0) && gNextRefresh <= end) {
            extra += Due(gNextRefresh, gSteal.refreshPeriod, gSteal.refreshClocks, start, end,
                         dramShare, gRefreshFraction);
            if (gSdram.continuousPage) EmSdram::PrechargeAll();
        }
        next = std::min(gNextLcd, gNextRefresh);
        return uint32_t(extra);
    }

    uint32_t ChargeWindow(uint64_t start, uint64_t end) {
        const uint32_t busCycles = uint32_t(windowCount & 0xffff);
        const uint32_t dramCycles = uint32_t((windowCount >> 16) & 0xffff);
        const uint32_t busClocks = 4 * busCycles + windowWait;
        const uint32_t dramClocks =
            busCycles ? uint32_t(uint64_t(dramCycles) * busClocks / busCycles) : 0;
        const uint32_t extra = Charge(start, end, windowCycles, busClocks, dramClocks);
        ClearWindow();
        return extra;
    }
}  // namespace EmBusTiming
