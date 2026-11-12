//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "pxa_PwrClk.h"
#include "pxa_I2S.h"
#include "pxa255_UDC.h"

#include <cstdlib>
#include <cstring>

#include "SoC.h"
#include "cputil.h"
#include "pxa_GPIO.h"
#include "pxa_RTC.h"
#include "pxa270_KPC.h"
#include "savestate/savestateAll.h"

#define PXA_CLOCK_MANAGER_BASE 0x41300000UL
#define PXA_CLOCK_MANAGER_SIZE 0x00001000UL

#define PXA_POWER_MANAGER_BASE 0x40F00000UL
#define PXA_POWER_MANAGER_SIZE 0x00000180UL

#define SAVESTATE_VERSION 3
// PXA25x saves stay at version 2; version 3 adds CLKCFG's HT and B on PXA27x.
#define SAVESTATE_VERSION_PXA25X 2

struct PxaPwrClk {
    PxaI2s *i2s;
    Pxa255Udc *udc;
    void *clockContext;
    void (*clockObserver)(void *);
    struct ArmCpu *cpu;
    class SoC *soc;
    uint32_t CCCR, CKEN, OSCR;  // clocks manager regs
    // power mgr common between 255 and 270
    uint32_t PMCR, PSSR, PSPR, PWER, PRER, PFER, PEDR, PCFR, PGSR[4], RCSR, PMFW;
    // power mgr 270 only
    uint32_t PSTR, PVCR, PUCR, PKWR, PKSR, PCMD[32];
    bool turbo, isPXA270;
    uint32_t clkcfgBits;  // CLKCFG HT (bit 2) and B (bit 3) from the last write, PXA27x only
    bool sleeping, wakePending;
    bool restoring;
    PxaGpio* gpio;
    PxaKpc* kpc;

    template <typename T>
    void DoSaveLoad(T &chunkHelper, uint32_t version) {
        chunkHelper.Do32(CCCR)
            .Do32(CKEN)
            .Do32(OSCR)
            .Do32(PMCR)
            .Do32(PSSR)
            .Do32(PSPR)
            .Do32(PWER)
            .Do32(PRER)
            .Do32(PFER)
            .Do32(PEDR)
            .Do32(PCFR)
            .DoBuffer32(PGSR, sizeof(PGSR) >> 2)
            .Do32(RCSR)
            .Do32(PMFW)
            .DoBool(turbo);
        if (version >= 1)
            chunkHelper.Do32(PSTR).Do32(PVCR).Do32(PUCR).Do32(PKWR).Do32(PKSR)
                .DoBuffer32(PCMD, 32);
        if (version >= 2) chunkHelper.DoBool(sleeping).DoBool(wakePending);
        if (version >= 3) chunkHelper.Do32(clkcfgBits);
    }
};

static void pxaPwrClkSyncUdc(PxaPwrClk* pc,bool refresh=true) {
    if(pc->udc)pxa255UdcSetClockEnabled(pc->udc,!pc->sleeping && (pc->CKEN&0x800),refresh);
}

static bool pxaPwrClkPrvCoproc7regXferFunc(struct ArmCpu *cpu, void *userData, bool two, bool read,
                                           uint8_t op1, uint8_t Rx, uint8_t CRn, uint8_t CRm,
                                           uint8_t op2) {
    // PSFS read?
    // real hardware ignors CRm if CRn is 4, so do we
    if (read && !two && !op1 && !op2 && CRn == 4) {
        cpuSetReg(cpu, Rx, 0);

        return true;
    }

    return false;
}

static bool pxaPwrClkPrvCoproc14regXferFunc(struct ArmCpu *cpu, void *userData, bool two, bool read,
                                            uint8_t op1, uint8_t Rx, uint8_t CRn, uint8_t CRm,
                                            uint8_t op2) {
    struct PxaPwrClk *pc = (struct PxaPwrClk *)userData;
    uint32_t val = 0;

    if (!read) val = cpuGetRegExternal(cpu, Rx);

    if (CRm == 0 && op1 == 0 && !two) {
        switch (CRn) {
            case 6:
                if (op2 != 0) break;
                if (read)  // CLKCFG: PXA27x reads back T, HT and B; F reads 0
                    val = pc->isPXA270 ? (pc->turbo ? 1 : 0) | pc->clkcfgBits : 0;
                else {
                    pc->turbo = (val & 1) != 0;
                    if (pc->isPXA270) pc->clkcfgBits = val & 0xc;
                    if (val & 2)
                        fprintf(stderr, "Set speed mode (CCCR + cp14 reg6) to 0x%08lx 0x%08lx\n",
                                (unsigned long)pc->CCCR, (unsigned long)val);
                }
                goto success;

            case 7:
                if (read)
                    val = 0;
                else if (op2 != 0) {
                } else if (pc->isPXA270 && val == 8) {
                    // PXA27x VC: the sequencer sends PCMD over PWR_I2C to the PMIC. There is no
                    // PMIC to answer, so the change is done at once.
                } else if (val == 1) {
                    pc->soc->Sleep();
                } else if (val == 3 || (pc->isPXA270 && val == 7)) {  // 7 is PXA27x deep sleep
                    if (pc->gpio) {
                        // Sleep ignores ordinary IRQs; only programmed PM sources exit it.
                        pc->sleeping = true;
                        pxaPwrClkSyncUdc(pc);
                        pc->wakePending = false;
                        // SSS, PH, RDH; PXA27x adds OTGPH.
                        pc->PSSR |= pc->isPXA270 ? 0x71 : 0x31;
                        pxaGpioHoldSleepOutputs(pc->gpio, pc->PGSR);
                        pc->soc->Sleep();
                    } else {
                        pc->RCSR |= 4;
                        cpuReset(pc->cpu, 0);
                    }
                } else {
                    fprintf(stderr,
                            "Someone tried to set processor power mode (cp14 reg7) to 0x%08lx\n",
                            (unsigned long)val);
                }
                goto success;

            case 10:  // notifies debugger about shit. if no debugger we just continue
                if (op2 != 0) break;
                if (read) val = 0;
                goto success;
        }
    }

    return false;

success:

    if (read) cpuSetReg(cpu, Rx, val);

    return true;
}

static bool pxaPwrClkPrvClockMgrMemAccessF(void *userData, uint32_t pa, uint8_t size, bool write,
                                           void *buf) {
    struct PxaPwrClk *pc = (struct PxaPwrClk *)userData;
    uint32_t val = 0;

    if (size != 4) {
        fprintf(stderr, "%s: Unexpected %s of %u bytes to 0x%08lx\n", __func__,
                write ? "write" : "read", size, (unsigned long)pa);
        return false;
    }

    pa = (pa - PXA_CLOCK_MANAGER_BASE) >> 2;

    if (write) val = *(uint32_t *)buf;

    switch (pa) {
        case 0:  // CCCR
            if (write)
                pc->CCCR = val;
            else
                val = pc->CCCR;
            break;

        case 1:  // CKEN
            if (write)
                { pc->CKEN = val;
                  if (pc->clockObserver) pc->clockObserver(pc->clockContext);
                  pxaPwrClkSyncUdc(pc);
                  if (pc->i2s) pxaI2sSetClockEnabled(pc->i2s, !!(val & 0x100)); }
            else
                val = pc->CKEN;
            break;

        case 2:  // OSCR
            if (!write) val = pc->OSCR;
            // no writing to this register
            break;

        case 3:  // PXA27x CCSR: PLLs locked, running the L and 2N that CCCR asks for
            if (!write && pc->isPXA270) val = 0x30000000u | (pc->CCCR & 0x79fu);
            break;
    }

    if (!write) *(uint32_t *)buf = val;

    return true;
}

static bool pxaPwrClkPrvPowerMgrMemAccessF(void *userData, uint32_t pa, uint_fast8_t size,
                                           bool write, void *buf) {
    struct PxaPwrClk *pc = (struct PxaPwrClk *)userData;
    uint32_t val = 0;

    if (size != 4) {
        fprintf(stderr, "%s: Unexpected %s of %u bytes to 0x%08lx\n", __func__,
                write ? "write" : "read", size, (unsigned long)pa);
        return false;
    }

    pa = (pa - PXA_POWER_MANAGER_BASE) >> 2;

    if (write) val = *(uint32_t *)buf;

    switch (pa) {
        case 0x00 / 4:  // PMCR
            if (write)
                pc->PMCR = val & (pc->isPXA270 ? 0x3f : 0x01);
            else
                val = pc->PMCR;
            break;

        case 0x04 / 4:  // PSSR
            if (write) {
                pc->PSSR &= ~val;
                if ((val & 0x10) && pc->gpio) pxaGpioReleaseSleepOutputs(pc->gpio);
            } else
                val = pc->PSSR;
            break;

        case 0x08 / 4:  // PSPR
            if (write)
                pc->PSPR = val;
            else
                val = pc->PSPR;
            break;

        case 0x0c / 4:  // PWER
            if (write)
                pc->PWER = val & (pc->isPXA270 ? 0xdf9ffe1b : 0xf000ffff);
            else
                val = pc->PWER;
            break;

        case 0x10 / 4:  // PRER
            if (write)
                pc->PRER = val & (pc->isPXA270 ? 0x0100fe1b : 0x0000ffff);
            else
                val = pc->PRER;
            break;

        case 0x14 / 4:  // PFER
            if (write)
                pc->PFER = val & (pc->isPXA270 ? 0x0100fe1b : 0x0000ffff);
            else
                val = pc->PFER;
            break;

        case 0x18 / 4:  // PEDR
            if (write)
                pc->PEDR &= ~val;
            else
                val = pc->PEDR;
            break;

        case 0x1c / 4:  // PCFR
            if (write)
                pc->PCFR = val & (pc->isPXA270 ? 0xdcd7 : 0x0007);
            else
                val = pc->PCFR;
            break;

        case 0x2c / 4:  // PGSR[3]
            if (!pc->isPXA270) return false;
            val &= 0x01ffffff;
            // fallthrough
        case 0x28 / 4:  // PGSR[2]
        case 0x24 / 4:  // PGSR[1]
        case 0x20 / 4:  // PGSR[0]
            if (write)
                pc->PGSR[pa - 0x20 / 4] = val;
            else
                val = pc->PGSR[pa - 0x20 / 4];
            break;

        case 0x30 / 4:  // RCSR
            if (write)
                pc->RCSR &= ~val;
            else
                val = pc->RCSR;
            break;

        case 0x34 / 4:  // PMFW (pxa255) / PSLR (pxa270)
            if (write)
                pc->PMFW = val & (pc->isPXA270 ? 0xffd00f0c : 0x00000002);
            else
                val = pc->PMFW;
            break;

        case 0x38 / 4:  // PSTR
            if (!pc->isPXA270) return false;
            if (write)
                pc->PSTR = val & 0x00000f0c;
            else
                val = pc->PSTR;
            break;

        case 0x40 / 4:  // PVCR (we do not emulate the actual PM i2c bus)
            if (!pc->isPXA270) return false;
            if (write)
                pc->PVCR = val & 0x01f04fff;
            else
                val = pc->PVCR;
            break;

        case 0x4c / 4:  // PUCR
            if (!pc->isPXA270) return false;
            if (write)
                pc->PUCR = val & 0x0000002d;
            else
                val = pc->PUCR;
            break;

        case 0x50 / 4:  // PKWR
            if (!pc->isPXA270) return false;
            if (write)
                pc->PKWR = val & 0x000fffff;
            else
                val = pc->PKWR;
            break;

        case 0x54 / 4:  // PKSR
            if (!pc->isPXA270) return false;
            if (write)
                pc->PKSR &= ~val;
            else
                val = pc->PKSR;
            break;

        case 0x80 / 4 ... 0xfc / 4:
            if (!pc->isPXA270) return false;
            if (write)
                pc->PCMD[pa - 0x80 / 4] = val & 0x00001fff;
            else
                val = pc->PCMD[pa - 0x80 / 4];
            break;

        default:
            return false;
    }

    if (!write) *(uint32_t *)buf = val;

    return true;
}

struct PxaPwrClk *pxaPwrClkInit(struct ArmCpu *cpu, struct ArmMem *physMem, class SoC *soc,
                                bool isPXA270) {
    struct PxaPwrClk *pc = (struct PxaPwrClk *)malloc(sizeof(*pc));
    struct ArmCoprocessor cp14 = {
        .regXfer = pxaPwrClkPrvCoproc14regXferFunc,
        .dataProcessing = nullptr,
        .memAccess = nullptr,
        .twoRegF = nullptr,
        .userData = pc,
    };
    struct ArmCoprocessor cp7 = {
        .regXfer = pxaPwrClkPrvCoproc7regXferFunc,
        .dataProcessing = nullptr,
        .memAccess = nullptr,
        .twoRegF = nullptr,
        .userData = pc,
    };

    if (!pc) ERR("cannot alloc PWRCLKMGR");

    memset(pc, 0, sizeof(*pc));

    pc->cpu = cpu;
    pc->isPXA270 = isPXA270;
    pc->soc = soc;

    pxaPwrClkReset(pc);

    cpuCoprocessorRegister(cpu, 14, &cp14);
    cpuCoprocessorRegister(cpu, 7, &cp7);

    if (!memRegionAdd(physMem, PXA_CLOCK_MANAGER_BASE, PXA_CLOCK_MANAGER_SIZE,
                      pxaPwrClkPrvClockMgrMemAccessF, pc))
        ERR("cannot add CLKMGR to MEM\n");

    if (!memRegionAdd(physMem, PXA_POWER_MANAGER_BASE, PXA_POWER_MANAGER_SIZE,
                      pxaPwrClkPrvPowerMgrMemAccessF, pc))
        ERR("cannot add PWRMGR to MEM\n");

    return pc;
}

static void pxaPwrClkGpioEdge(void* context, uint32_t pin, bool oldLevel, bool newLevel) {
    auto* pc = static_cast<PxaPwrClk*>(context);
    if (!pc->sleeping || pc->restoring || oldLevel == newLevel || !pxaGpioIsInput(pc->gpio, pin))
        return;

    // PXA25x wakes on GPIO0-15 (Table 3-12); PXA27x on a subset plus GPIO35 and muxed pins.
    uint32_t source = !pc->isPXA270 ? (pin < 16 ? 1u << pin : 0)
        : pin < 16 ? ((1u << pin) & 0xfe1b) : pin == 35 ? 0x01000000 : 0;
    if (source && (pc->PWER & source) && ((newLevel ? pc->PRER : pc->PFER) & source))
        pc->PEDR |= source, pc->wakePending = true;
    if (!pc->isPXA270) return;

    const unsigned mux2[] = {121, 38, 53, 40, 36, 121, 121, 121};
    const unsigned mux3[] = {121, 31, 113, 121};
    if (pin == mux2[(pc->PWER >> 16) & 7])
        pc->PEDR |= 1u << 17, pc->wakePending = true;
    if (pin == mux3[(pc->PWER >> 19) & 3])
        pc->PEDR |= 1u << 20, pc->wakePending = true;
}

static void pxaPwrClkRtcAlarm(void* context) {
    auto* pc = static_cast<PxaPwrClk*>(context);
    if (pc->sleeping && !pc->restoring && (pc->PWER & 0x80000000)) {
        pc->PEDR |= 0x80000000;
        pc->wakePending = true;
    }
}

void pxaPwrClkConnectWakeSources(PxaPwrClk* pc, PxaGpio* gpio, PxaRtc* rtc, PxaKpc* kpc) {
    pc->gpio = gpio;
    pc->kpc = kpc;
    pxaGpioSetInputObserver(gpio, pxaPwrClkGpioEdge, pc);
    pxaRtcSetWakeCallback(rtc, pxaPwrClkRtcAlarm, pc);
}

void pxaPwrClkSetResetCause(PxaPwrClk* pc, uint32_t rcsr) { pc->RCSR = rcsr; }
bool pxaPwrClkSleeping(const PxaPwrClk* pc) { return pc && pc->sleeping; }
void pxaPwrClkRestoreInProgress(PxaPwrClk* pc, bool restoring) { pc->restoring = restoring; }
uint32_t pxaPwrClkSleepRetention(const PxaPwrClk* pc) { return pc->PMFW; }
bool pxaPwrClkRetainPowerI2c(const PxaPwrClk* pc) { return (pc->PCFR & 0x40) != 0; }

bool pxaPwrClkWakePending(PxaPwrClk* pc) {
    if (!pc->sleeping || pc->restoring) return false;
    if (!pc->isPXA270) return pc->wakePending;  // PXA25x has no keypad wake (PKWR).
    // PKWR/PKSR bit order and GAFR input muxes: Tables 3-28, 3-29, 24-2.
    static const struct { unsigned pin, matrixAf, row, directAf, key; } pads[] = {
        {13,3,7,2,7}, {16,1,5,0,0}, {17,1,6,0,0}, {34,2,3,0,0},
        {36,3,7,0,0}, {37,3,3,0,0}, {38,2,4,0,0}, {39,1,4,0,0},
        {90,1,5,0,0}, {91,1,6,0,0}, {93,0,0,1,0}, {94,0,0,1,1},
        {95,3,6,1,2}, {96,0,0,1,3}, {97,3,3,1,4}, {98,3,4,1,5},
        {99,3,5,1,6}, {100,1,0,0,0}, {101,1,1,0,0}, {102,1,2,0,0}
    };
    const uint8_t rows = pxaKpcMatrixWakeLevels(pc->kpc);
    const uint8_t keys = pxaKpcDirectWakeLevels(pc->kpc);
    for (unsigned bit = 0; bit < 20; ++bit) {
        const auto& pad = pads[bit];
        if (!(pc->PKWR & (1u << bit)) || !pxaGpioIsInput(pc->gpio, pad.pin)) continue;
        const unsigned af = pxaGpioAlternateFunction(pc->gpio, pad.pin);
        const bool matrix = pad.matrixAf && af == pad.matrixAf;
        const bool direct = pad.directAf && af == pad.directAf;
        if ((matrix || direct) &&
            (pxaGpioInputLevel(pc->gpio, pad.pin) ||
             (matrix && (rows & (1u << pad.row))) || (direct && (keys & (1u << pad.key))))) {
            pc->PKSR |= 1u << bit;
            pc->wakePending = true;
        }
    }
    return pc->wakePending;
}

void pxaPwrClkFinishSleep(PxaPwrClk* pc) {
    // Table 3-2: retain wake configuration/status, scratchpad, RTC and clocks.
    memset(pc->PGSR, 0, sizeof(pc->PGSR));
    if (!pxaPwrClkRetainPowerI2c(pc)) {
        pc->PVCR = 0;
        memset(pc->PCMD, 0, sizeof(pc->PCMD));
    }
    pc->RCSR |= 4;  // SMR: sleep-exit reset, never set at sleep entry.
    pc->turbo = false;
    pc->clkcfgBits = 0;
    pc->sleeping = pc->wakePending = false;
    pxaPwrClkSyncUdc(pc);
    if (pc->PCFR & 0x4000) {
        pc->PSSR &= ~0x10u;
        pxaGpioReleaseSleepOutputs(pc->gpio);
    }
}

void pxaPwrClkReset(struct PxaPwrClk *pc) {
    pc->sleeping = pc->wakePending = false;
    pc->clkcfgBits = 0;
    // PXA25x: almost the default (we use mult 32 not 27). PXA27x: L 16, 2N 3, the 208 MHz run
    // and 312 MHz turbo these devices run at.
    pc->CCCR = pc->isPXA270 ? 0x00000190UL : 0x00000122UL;
    pc->CKEN = 0x000179EFUL;  // set CKEN to default value
    pc->OSCR = 0x00000003UL;  // 32KHz oscillator on and stable
    pc->PMCR = 0;
    pc->PSSR = 0x20;
    pc->PWER = 0x03;
    pc->PRER = 0x03;
    pc->PFER = 0x03;
    pc->PEDR = 0;
    pc->PCFR = 0;
    for (size_t i = 0; i < 4; i++) pc->PGSR[i] = 0;
    pc->RCSR = 0;
    pc->PMFW = pc->isPXA270 ? 0xcc000000 : 0x00000000;
    pc->PSTR = 0;
    pc->PVCR = 0;
    pc->PUCR = 0;
    pc->PKWR = 0;
    pc->PKSR = 0;
    for (size_t i = 0; i < 32; i++) pc->PCMD[i] = 0;

    // pretend we just power-no-resetted
    pc->RCSR |= 1;
    pxaPwrClkSyncUdc(pc);
}

template <typename T>
void pxaPwrClkSave(struct PxaPwrClk *pc, T &savestate) {
    // PXA25x moved to version 2 with power sleep; older versions still load.
    const uint32_t version = pc->isPXA270 ? SAVESTATE_VERSION : SAVESTATE_VERSION_PXA25X;
    auto chunk = savestate.GetChunk(ChunkType::pxaPwrClk, version);
    if (!chunk) ERR("unable to allocate chunk");

    SaveChunkHelper helper(*chunk);
    pc->DoSaveLoad(helper, version);
}

template <typename T>
void pxaPwrClkLoad(struct PxaPwrClk *pc, T &loader) {
    uint32_t version;
    auto chunk = loader.GetChunkOrFail(ChunkType::pxaPwrClk, SAVESTATE_VERSION,
                                       "pxa power / clock CP", version);
    if (!chunk) return;

    LoadChunkHelper helper(*chunk);
    pc->sleeping = pc->wakePending = false;  // v0/v1 never represented power sleep.
    // Before v3 a PXA27x takes B as set, as Palm OS sets it (CLKCFG 0xb) while it boots.
    pc->clkcfgBits = pc->isPXA270 ? 0x8 : 0;
    if (version == 0) {
        pc->PSTR = pc->PVCR = pc->PUCR = pc->PKWR = pc->PKSR = 0;
        memset(pc->PCMD, 0, sizeof(pc->PCMD));
    }
    pc->DoSaveLoad(helper, version);
    if (pc->wakePending && !pc->sleeping) loader.NotifyError();
    // The later UDC load refreshes requests from its restored FIFO state.
    pxaPwrClkSyncUdc(pc,false);
}

template void pxaPwrClkSave<Savestate<ChunkType>>(PxaPwrClk *pc, Savestate<ChunkType> &savestate);
template void pxaPwrClkSave<SavestateProbe<ChunkType>>(PxaPwrClk *pc,
                                                       SavestateProbe<ChunkType> &savestate);
template void pxaPwrClkLoad<SavestateLoader<ChunkType>>(PxaPwrClk *pc,
                                                        SavestateLoader<ChunkType> &loader);

bool pxaPwrClkI2sClock(const PxaPwrClk *pc) { return pc->CKEN & 0x100; }
void pxaPwrClkConnectI2s(PxaPwrClk *pc, PxaI2s *i2s) {
    pc->i2s = i2s;
    pxaI2sSetClockEnabled(i2s, pxaPwrClkI2sClock(pc));
}

uint32_t pxaPwrClkClockMask(const PxaPwrClk *p) { return p->CKEN; }
void pxaPwrClkSetClockObserver(PxaPwrClk *p, void *c, void (*f)(void *)) { p->clockContext=c; p->clockObserver=f; }

void pxaPwrClkConnectUdc(PxaPwrClk* pc,Pxa255Udc* udc) {
    pc->udc=udc;
    pxaPwrClkSyncUdc(pc);
}
