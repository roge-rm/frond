#ifndef EmBusTiming_h
#define EmBusTiming_h

// Bus time the DragonBall's LCD DMA and DRAM refresh take from the 68000 core, worked out from
// the registers as the ROM programs them (MC68EZ328 and MC68VZ328 user's manuals). Everything
// is in system clocks. The CPU loop calls EmBusTiming::Charge after each instruction once the
// cycle count reaches EmBusTiming::next, so the cost when nothing is due is one compare.

#include <cstdint>

struct EmBusSteal {
    // System clocks per LCD line and the bus clocks the line's DMA takes; 0 with the DMA off.
    uint32_t lcdLineClocks{0};
    uint32_t lcdDmaClocks{0};
    // System clocks between DRAM refresh requests and the clocks one refresh holds the DRAM;
    // 0 without CAS-before-RAS refresh.
    uint32_t refreshPeriod{0};
    uint32_t refreshClocks{0};

    bool operator==(const EmBusSteal& o) const {
        return lcdLineClocks == o.lcdLineClocks && lcdDmaClocks == o.lcdDmaClocks &&
               refreshPeriod == o.refreshPeriod && refreshClocks == o.refreshClocks;
    }
};

// DragonBall clock dividers from a PLLCR SYSCLK SEL or LCDCLK SEL field: 2, 4, 8, 16, or 1.
uint32_t EmDragonballClockDivider(uint32_t select);

// MC68EZ328 LCD DMA (section 12) and DRAM controller (section 14) registers.
struct EmEzBusRegisters {
    uint16_t pllControl;       // PLLCR: LCDCLK SEL 13-11, SYSCLK SEL 10-8
    uint16_t lcdScreenWidth;   // LXMAX, pixels
    uint16_t lcdScreenHeight;  // LYMAX, lines minus one
    uint8_t lcdPanelControl;   // LPICF: GS 1-0
    uint8_t lcdPixelClock;     // LPXCD: PCD 5-0
    uint8_t lcdClockControl;   // LCKCON: LCDON 7, DWIDTH 6, DWS 3-0
    uint8_t lcdRefreshRateAdj; // LRRA
    uint8_t lcdPanningOffset;  // LPOSR
    bool lcdInDram;            // LSSA is in the DRAM chip select
    uint16_t dramConfig;       // DRAMMC: REF 3-0
    uint16_t dramControl;      // DRAMC: EN 15, RM 14, BC 13-12, CLK 11, EDO 10, MSW 5
    bool dramEnabled;          // CSD drives DRAM (CSD's DRAM bit)
    uint32_t systemClockHz;
};

EmBusSteal EmEzBusSteal(const EmEzBusRegisters& r);

// MC68VZ328 LCD (section 8), DRAM controller (section 7) registers.
struct EmVzBusRegisters {
    uint16_t pllControl;       // PLLCR: LCDCLK SEL 13-11, SYSCLK SEL 10-8
    uint16_t lcdScreenWidth;   // LXMAX, pixels
    uint16_t lcdScreenHeight;  // LYMAX, lines minus one
    uint8_t lcdPanelControl;   // LPICF: GS 1-0
    uint8_t lcdPixelClock;     // LPXCD: PCD 5-0
    uint8_t lcdClockControl;   // LCKCON: LCDEN 7, DWIDTH 6, DWS 3-0
    uint16_t lcdRefreshRateAdj; // LRRA, 10 bits
    uint8_t lcdPanningOffset;  // LPOSR
    uint8_t lcdDmaControl;     // DMACR: burst length 7-4
    bool lcdInDram;            // LSSA is in a DRAM chip select
    uint16_t dramConfig;       // DRAMMC: REF 3-0
    uint16_t dramControl;      // DRAMC: EN 15, RM 14, SLW 13 ... see EmVzBusSteal
    uint16_t sdramControl;     // SDCTRL
    bool dramEnabled;          // CSD or CSC drives DRAM
    bool sdram;                // that DRAM is SDRAM
    uint32_t systemClockHz;
};

EmBusSteal EmVzBusSteal(const EmVzBusRegisters& r);

// MC68VZ328 SDRAM page state for CPU accesses (VZ manual 7.3.3 and figures 19-15 to 19-19, CAS
// latency 1): a read waits 1 clock on a page hit and 3 on a miss, a write 0 and 1. CAS latency
// 2 adds a clock to reads. Without continuous page mode every access is a miss.
struct EmSdramTiming {
    bool continuousPage{false};
    bool casLatency2{false};
    uint8_t bankHigh{0xff}, bankLow{0xff};  // address bits picking the bank, 0xff for none
    uint8_t pageShift{9};                     // address bits within one row (page)
};

// SDCTRL and DRAMC PGSZ as the SDRAM timing.
EmSdramTiming EmVzSdramTiming(uint16_t sdramControl, uint16_t dramControl);

namespace EmSdram {
    void Set(const EmSdramTiming& timing);
    // Wait clocks for a CPU access of words 16-bit words from address.
    uint32_t Access(uint32_t address, bool write, uint32_t words);
    // A refresh closes every page.
    void PrechargeAll();
}  // namespace EmSdram

namespace EmBusTiming {
    struct State {
        EmBusSteal steal;
        uint32_t sdramConfig;
        uint64_t nextLcd, nextRefresh, count;
        uint32_t lcdFraction, refreshFraction, wait, cycles;
        uint32_t openRow[4];
    };

    State Capture();
    // Validates against the register-derived model before changing any running state.
    bool Restore(const State& state);
    void Reset(uint64_t now);
    // Clears the model and history when replacing the machine.
    void Clear();

    // Next cycle count at which Charge has something to do; ~0 when nothing is modelled.
    extern uint64_t next;

    // Sets the model, keeping the phase of events already scheduled where it can.
    void Set(const EmBusSteal& steal, uint64_t now);
    const EmBusSteal& Get();

    // Extra clocks for an instruction that ran from start to end (system cycles), of which
    // busClocks used the bus and dramClocks the DRAM. LCD DMA and refresh due in that span
    // delay it by their length times the instruction's share of bus or DRAM time, as the core
    // keeps working internally while it is off the bus. Events before start passed while the
    // CPU was stopped and cost nothing.
    uint32_t Charge(uint64_t start, uint64_t end, uint32_t cycles, uint32_t busClocks,
                    uint32_t dramClocks);

    // The CPU's bus use since the last charge: bus cycles in the low 16 bits and DRAM cycles
    // above (from EmAddressBank::busTag), the wait clocks of those cycles and all its clocks.
    extern uint64_t windowCount;
    extern uint32_t windowWait, windowCycles;

    // Charge for an instruction that ran from start to end, at the bus and DRAM shares of the
    // window, which it then clears. Taking the share over the window rather than from the one
    // instruction an event lands in keeps the charge from locking onto a loop's phase.
    uint32_t ChargeWindow(uint64_t start, uint64_t end);
}  // namespace EmBusTiming

#endif
