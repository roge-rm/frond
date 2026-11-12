//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "cp15mmu.h"

#include <cstdlib>
#include <cstring>

#include "CPU.h"
#include "MMU.h"
#include "arm920_state.h"
#include "cputil.h"
#include "icache.h"
#include "savestate/savestateAll.h"

#define SAVESTATE_VERSION 1

struct ArmCP15MMU {
    struct ArmCpu* cpu;
    struct ArmMmu* mmu;
    struct icache* ic;

    uint32_t control;
    uint32_t ttb;
    uint32_t FSR;  // fault sttaus register
    uint32_t FAR;  // fault address register

    union {
        struct {
            uint32_t CPAR;  // coprocessor access register
            uint32_t ACP;   // auxilary control reg for xscale
        };  // xscale
        struct {
            uint8_t cfg, iMin, iMax;
            uint16_t tid;
        };  // omap
    };
    uint32_t mmuSwitchCy;

    uint32_t cpuid;
    uint32_t cacheId;

    bool xscale, omap;
    bool arm920Bootstrap;
    bool arm920IcacheIndexCompatibility;
    Arm920DataCache dataCache;

    template <typename T>
    void DoSaveLoad(T& chunkHelper);
};

void cp15MMUCycle(struct ArmCP15MMU* cp15)  // mmu on/off lags by a cycle
{
    if (cp15->mmuSwitchCy) {
        if (!--cp15->mmuSwitchCy) {
            mmuSetTTP(cp15->mmu, (cp15->control & 0x00000001UL) ? cp15->ttb : MMU_DISABLED_TTP);

            cpuClearSlowPath(cp15->cpu, SLOW_PATH_REASON_CP15);
        }
    }
}

static bool cp15prvCoprocRegXferFunc(struct ArmCpu* cpu, void* userData, bool two, bool read,
                                     uint8_t op1, uint8_t Rx, uint8_t CRn, uint8_t CRm,
                                     uint8_t op2) {
    struct ArmCP15MMU* cp15 = (struct ArmCP15MMU*)userData;
    uint32_t val = 0, tmp;

    if (!read) val = cpuGetRegExternal(cpu, Rx);

    if (op1 != 0 || two) goto fail;  // CP15 only accessed with MCR/MRC with op1 == 0

    if (cp15->arm920Bootstrap) {
        // DDI0151C 2.3: reject unmodelled forms before they can change CPU state.
        if (Rx == 15) return false;
        // Control, TTB, domain and TLB writes can change any translation the Fast engine holds.
        if (!read && (CRn == 1 || CRn == 2 || CRn == 3 || CRn == 8)) cpuArm920LeanFlush(cpu);
        if (CRn == 0 && CRm == 0 && op2 == 1 && read) {
            cpuSetReg(cpu, Rx, 0x0d172172);  // Table 2-6, two 16KiB, 64-way caches.
            return true;
        }
        if (CRn == 1 && CRm == 0 && op2 == 0) {
            if (read) {
                cpuSetReg(cpu, Rx, cp15->control);
                return true;
            }
            // C requires translation-table memory attributes. Big endian remains unsupported.
            if ((val & 0x78) != 0x78 || (val & ~0xc000737fu) ||
                (val & 0xc0000000) == 0x80000000 || ((val & 4) && !(val & 1))) return false;
            if ((val ^ cp15->control) & 1)
                icacheArm920PrepareMmuSwitch(cp15->ic, cpuGetRegisters(cpu)[15],
                    (cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR) & ARM_SR_M) != ARM_SR_MODE_USR);
            cp15->control = val;
            cpuSetVectorAddr(cpu, val & 0x2000 ? 0xffff0000 : 0);
            mmuSetS(cp15->mmu, (val & 0x100) != 0);
            mmuSetR(cp15->mmu, (val & 0x200) != 0);
            icacheArm920Control(cp15->ic, (val & 0x1000) != 0, (val & 0x4000) != 0);
            mmuSetTTPArm920(cp15->mmu, val & 1 ? cp15->ttb : MMU_DISABLED_TTP);
            return true;
        }
        if (CRm == 0 && op2 == 0) {
            if (CRn == 2) {
                if (read) cpuSetReg(cpu, Rx, cp15->ttb);
                else {
                    if (val & 0x3fff) return false;
                    cp15->ttb = val;
                    if (cp15->control & 1) mmuSetTTPArm920(cp15->mmu, val);
                }
                return true;
            }
            if (CRn == 3) {
                if (read) cpuSetReg(cpu, Rx, mmuGetDomainCfg(cp15->mmu));
                else mmuSetDomainCfg(cp15->mmu, val);
                return true;
            }
            if (CRn == 5 || CRn == 6) {
                auto& reg = CRn == 5 ? cp15->FSR : cp15->FAR;
                if (read) cpuSetReg(cpu, Rx, reg);
                else reg = val;
                return true;
            }
            if (CRn == 13) {
                if (read) cpuSetReg(cpu, Rx, cpuGetPid(cpu));
                else if (val == 0) cpuSetPid(cpu, 0);
                else return false;  // Nonzero FCSE needs both instruction and data paths.
                return true;
            }
        }
        if (read) return false;
        if (CRn == 7 && CRm == 5 && op2 == 2 && cp15->arm920IcacheIndexCompatibility) {
            // Undocumented on ARM920T: opt-in compatibility flushes the whole I-cache.
            icacheInvalGuest(cp15->ic);
            return true;
        }
        if (CRn == 7 && op2 == 2 && (CRm == 10 || CRm == 14)) {
            // DDI0151C 2.3.11: index bits 31:26, segment bits 7:5, all other bits SBZ.
            if (val & ~0xfc0000e0u) return false;
            return cp15->dataCache.maintain(val, true, true, CRm == 14);
        }
        if (CRn == 7 && op2 == 0 && (CRm == 5 || CRm == 6 || CRm == 7)) {
            if (val) return false;
            if (CRm != 5) cp15->dataCache.invalidate();
            if (CRm != 6) icacheInvalGuest(cp15->ic);
            return true;
        }
        if (CRn == 7 && op2 == 1 && (CRm == 6 || CRm == 10 || CRm == 14)) {
            if (val & 31) return false;
            return cp15->dataCache.maintain(val, false, CRm != 6, CRm != 10);
        }
        if (CRn == 7 && CRm == 5 && op2 == 1) {
            if (val & 31) return false;
            icacheInvalAddr(cp15->ic, val);
            return true;
        }
        if (CRn == 7 && CRm == 0 && op2 == 4) {
            // DDI0151C 4.9: drain before waiting; only an attached scheduler admits WFI.
            return !cp15->dataCache.guard.active && cpuArm920BeginWait(cpu);
        }
        if (CRn == 7 && CRm == 10 && op2 == 4) {
            // DDI0151C SBZ violations are UNPREDICTABLE; drain ignores the operand in this model.
            return !cp15->dataCache.guard.active;
        }
        if (CRn == 8 && op2 == 0 && (CRm == 5 || CRm == 6 || CRm == 7)) {
            mmuTlbFlush(cp15->mmu);
            return true;
        }
        return false;
    }

    switch (CRn) {
        case 0:  // ID codes

            if (!read) goto fail;     // cannot write to ID codes register
            if (CRm != 0) goto fail;  // CRm must be zero for this read
            if (op2 == 0) {           // main ID register

                val = cp15->cpuid;
                goto success;
            } else if (op2 == 1) {  // cache type register - we lie here

                val = cp15->cacheId;
                goto success;
            }
            break;

        case 1:  // control register

            if (op2 == 0 && CRm == 0) {
                if (read)
                    val = cp15->control;
                else {
                    uint32_t origVal = val;

                    // some bits ignore writes. pretend they were wirtten as proper
                    val |= 0x0070;
                    val &= ~0x0080;

                    tmp = val ^ cp15->control;  // see what changed and mask off then chack for what
                                                // we support changing of
                    if (tmp & 0x84F0UL) {
                        fprintf(
                            stderr,
                            "cp15: unknown bits changed (0x%08lx) 0x%08lx -> 0x%08lx, halting\n",
                            (unsigned long)(tmp & 0x84F0UL), (unsigned long)cp15->control,
                            (unsigned long)origVal);

                        ERR("CP15: unknown bits changed");
                    }

                    if (tmp & 0x00000002UL) {  // A bit
                        cp15->control ^= 0x00000002UL;
                    }
                    if (tmp & 0x00002000UL) {  // V bit

                        cpuSetVectorAddr(cp15->cpu,
                                         (val & 0x00002000UL) ? 0xFFFF0000UL : 0x00000000UL);
                        cp15->control ^= 0x00002000UL;
                    }
                    if (tmp & 0x00000200UL) {  // R bit

                        mmuSetR(cp15->mmu, (val & 0x00000200UL) != 0);
                        cp15->control ^= 0x00000200UL;
                    }
                    if (tmp & 0x00000100UL) {  // S bit

                        mmuSetS(cp15->mmu, (val & 0x00000100UL) != 0);
                        cp15->control ^= 0x00000100UL;
                    }
                    mmuControlWrite(cp15->mmu, (val & 0x00000004UL) != 0,
                                    (tmp & 0x00000001UL) && !(val & 0x00000001UL));
                    if (tmp & 0x00000001UL) {  // M bit

                        cp15->mmuSwitchCy = 2;
                        cp15->control ^= 0x00000001UL;

                        cpuSetSlowPath(cp15->cpu, SLOW_PATH_REASON_CP15);
                    }
                }
            } else if (CRm == 1) {
                fprintf(stderr, "sony cr1 bug?\n");
                if (read) val = 0;
            } else if (cp15->xscale && op2 == 1) {  // PXA-specific thing
                if (read)
                    val = cp15->ACP;
                else
                    cp15->ACP = val;
            } else
                break;
            goto success;

        case 2:  // translation tabler base
            if (read)
                val = cp15->ttb;
            else {
                if (cp15->control & 0x00000001UL) {  // mmu is on

                    mmuSetTTP(cp15->mmu, val);
                }
                cp15->ttb = val;
            }
            goto success;

        case 3:  // domain access control
            if (read)
                val = mmuGetDomainCfg(cp15->mmu);
            else {
                mmuSetDomainCfg(cp15->mmu, val);
                // dcacheFlushPermInfo(cp15->dc);
            }
            goto success;

        case 5:  // FSR
            if (read)
                val = cp15->FSR;
            else
                cp15->FSR = val;
            goto success;

        case 6:  // FAR
            if (read)
                val = cp15->FAR;
            else
                cp15->FAR = val;
            goto success;

        case 7:  // cache ops
            if ((CRm == 6 || CRm == 7) && op2 == 0) mmuDcacheInvalidate(cp15->mmu);
            if ((CRm == 5 || CRm == 7) && op2 == 0) {
                icacheInvalGuest(cp15->ic);  // invalidate entire {icache(5) or both i and dcache(7)}
                if (CRm == 7) {
                    // dcacheInval(cp15->dc);
                }
            } else if ((CRm == 5 || CRm == 7) && op2 == 1) {
                icacheInvalAddr(
                    cp15->ic,
                    val);  // invalidate {icache(5) or both i and dcache(7)} line, given VA
                if (CRm == 7) {
                    // dcacheInvalAddr(cp15->dc, val);
                }
            } else if ((CRm == 5 || CRm == 7) && op2 == 2) {
                icacheInvalGuest(
                    cp15->ic);  // invalidate {icache(5) or both i and dcache(7)} line, given
                                // set/index. i dont know how to do this, so flush the whole thing

                if (CRm == 7) {
                    // dcacheInvalSetWayRaw(cp15->dc, val);
                }
            } else if (CRm == 10 && op2 == 4) { /* drain write buffer = nothing */
            } else if (CRm == 10 && op2 == 1) {
                // dcacheCleanSetWayRaw(cp15->dc, val);
            } else if (CRm == 6 && op2 == 0) {
                // dcacheInval(cp15->dc);
            } else if (CRm == 6 && op2 == 1) {
                // dcacheInvalAddr(cp15->dc, val);
            } else if (CRm == 6 && op2 == 2) {
                // dcacheInvalSetWayRaw(cp15->dc, val);
            } else if (CRm == 2 && op2 == 5) {
                // dcacheAllocAddr(cp15->dc, val);
            } else if (CRm == 5 && op2 == 6) {  /* flush btb = nothing */
            } else if (CRm == 0 && op2 == 4) {  /* idle = nothing */
            } else if (CRm == 14 && op2 == 2) { /* clean and inval d-cache line */
            } else if (CRm == 10 && op2 == 0) { /* clean entire d-cache - omap can do this */
            } else if (CRm == 10 && op2 == 2) { /* clean d-cache line */
            } else
                break;
            goto success;

        case 8:  // TLB ops
            mmuTlbFlush(cp15->mmu);
            // dcacheFlushPermInfo(cp15->dc);
            goto success;

        case 9:  // cache lockdown
            if (CRm == 1 && op2 == 0)
                fprintf(stderr, "Attempt to lock 0x%08lx+32 in icache\r\n", (unsigned long)val);
            else if (CRm == 2 && op2 == 0)
                fprintf(stderr, "Dcache now %s lock mode\r\n", val ? "in" : "out of");
            else
                break;
            goto success;

        case 10:  // TLB lockdown
            if (!read && CRm == 0 && (op2 == 0 || op2 == 1)) {
                goto success;
            }
            break;

        case 13:  // FCSE
            if (read)
                val = cpuGetPid(cp15->cpu);
            else
                cpuSetPid(cp15->cpu, val & 0xfe000000ul);
            goto success;

        case 14:  // xscale debug
            if (cp15->xscale) {
                if (CRm == 8 && op2 == 0) {  // ICBR0

                    val = 0;
                    goto success;
                }
                if (CRm == 9 && op2 == 0) {  // ICBR1

                    val = 0;
                    goto success;
                }
                if (CRm == 0 && op2 == 0) {  // DBR0

                    val = 0;
                    goto success;
                }
                if (CRm == 3 && op2 == 0) {  // DBR1

                    val = 0;
                    goto success;
                }
                if (CRm == 4 && op2 == 0) {  // DBCON

                    val = 0;
                    goto success;
                }
            }
            break;

        case 15:
            if (cp15->xscale && op2 == 0 && CRm == 1) {  // CPAR on xscale
                if (read)
                    val = cpuGetCPAR(cp15->cpu);
                else
                    cpuSetCPAR(cp15->cpu, val & 0x3FFF);
                goto success;
            } else if (cp15->omap) {  // omap shit

                if (CRm == 1 && op2 == 0) {
                    if (read)
                        val = cp15->cfg;
                    else
                        cp15->cfg = val & 0x87;
                    goto success;
                } else if (CRm == 2 && op2 == 0) {
                    if (read)
                        val = cp15->iMax;
                    else
                        cp15->iMax = val;
                    goto success;
                } else if (CRm == 3 && op2 == 0) {
                    if (read)
                        val = cp15->iMin;
                    else
                        cp15->iMin = val;
                    goto success;
                } else if (CRm == 4 && op2 == 0) {
                    if (read)
                        val = cp15->tid;
                    else
                        cp15->tid = val;
                    goto success;
                } else if (CRm == 8 && op2 == 0 && read) {
                    val = 0;
                    goto success;
                } else if (CRm == 8 && op2 == 2 && !read) {
                    // WFI
                    goto success;
                }
            }
            break;
    }

fail:
    return false;

success:

    if (read) cpuSetReg(cpu, Rx, val);
    return true;
}

struct ArmCP15MMU* cp15MMUInit(struct ArmCpu* cpu, struct ArmMmu* mmu, struct icache* ic,
                               uint32_t cpuid, uint32_t cacheId, bool xscale, bool omap) {
    struct ArmCP15MMU* cp15 = (struct ArmCP15MMU*)malloc(sizeof(*cp15));
    struct ArmCoprocessor cp = {
        .regXfer = cp15prvCoprocRegXferFunc,
        .dataProcessing = nullptr,
        .memAccess = nullptr,
        .twoRegF = nullptr,
        .userData = cp15,
    };

    if (!cp15) ERR("cannot alloc CP15");

    memset(cp15, 0, sizeof(*cp15));

    cp15->cpu = cpu;
    cp15->mmu = mmu;
    cp15->ic = ic;
    icacheSetXscale(ic, xscale);
    cp15->control = 0x00004072UL;
    cp15->cpuid = cpuid;
    cp15->cacheId = cacheId;
    cp15->xscale = xscale;
    cp15->omap = omap;

    if (omap) cp15->iMax = 0xff;

    cpuCoprocessorRegister(cpu, 15, &cp);

    return cp15;
}

bool cp15MMUAlignmentEnabled(const struct ArmCP15MMU* cp15) {
    return (cp15->control & 2) != 0;
}

uint32_t cp15MMUClockMode(const ArmCP15MMU* cp15) { return cp15->control >> 30; }

void cp15MMUArm920SetIcacheIndexCompatibility(ArmCP15MMU* cp15, bool enabled) {
    cp15->arm920IcacheIndexCompatibility = enabled;
}

uint32_t cp15MMUArm920Control(const ArmCP15MMU* cp15) { return cp15->control; }
Arm920DataCache* cp15MMUArm920DataCache(ArmCP15MMU* cp15) { return &cp15->dataCache; }

void cp15MMUConfigureArm920Bootstrap(struct ArmCP15MMU* cp15) {
    cp15->arm920Bootstrap = true;
    cp15->control = 0x78;  // DDI0151C table 2-10, assuming VINITHI low.
    cp15->mmuSwitchCy = 0;
    cp15->ttb = 0;
    cp15->FSR = cp15->FAR = 0;
    cp15->dataCache.reset();
    cpuClearSlowPath(cp15->cpu, SLOW_PATH_REASON_CP15);
    icacheConfigureArm920(cp15->ic);
    cpuSetVectorAddr(cp15->cpu, 0);
    cpuSetPid(cp15->cpu, 0);
    mmuSetS(cp15->mmu, false);
    mmuSetR(cp15->mmu, false);
}

void cp15MMUSetFaultStatus(struct ArmCP15MMU* cp15, uint32_t addr, uint_fast8_t faultStatus) {
    cp15->FAR = addr;
    cp15->FSR = faultStatus;
}

template <typename T>
void cp15MMUSave(struct ArmCP15MMU* cp15, T& savestate) {
    auto chunk = savestate.GetChunk(ChunkType::cp15mmu, SAVESTATE_VERSION);
    if (!chunk) ERR("unable to allocate chunk");

    SaveChunkHelper helper(*chunk);
    cp15->DoSaveLoad(helper);
    uint32_t fetchState[ICACHE_FETCH_STATE_WORDS];
    icacheGetFetchState(cp15->ic, fetchState);
    helper.DoBuffer32(fetchState, ICACHE_FETCH_STATE_WORDS);
}

template <typename T>
void cp15MMULoad(struct ArmCP15MMU* cp15, T& loader) {
    uint32_t version;
    auto chunk = loader.GetChunkOrFail(ChunkType::cp15mmu, SAVESTATE_VERSION, "cp15mmu", version);
    if (!chunk) return;

    LoadChunkHelper helper(*chunk);
    cp15->DoSaveLoad(helper);
    uint32_t fetchState[ICACHE_FETCH_STATE_WORDS] = {};
    fetchState[1] = fetchState[10] = 0xffffffff;
    if (version >= 1) helper.DoBuffer32(fetchState, ICACHE_FETCH_STATE_WORDS);
    if (!icacheSetFetchState(cp15->ic, fetchState)) {
        loader.NotifyError();
        return;
    }

    if (cp15->arm920Bootstrap)
        icacheArm920Control(cp15->ic, (cp15->control & 0x1000) != 0, (cp15->control & 0x4000) != 0);

    if (cp15->mmuSwitchCy > 0)
        cpuSetSlowPath(cp15->cpu, SLOW_PATH_REASON_CP15);
    else
        cpuClearSlowPath(cp15->cpu, SLOW_PATH_REASON_CP15);
}

template <typename T>
void ArmCP15MMU::DoSaveLoad(T& chunkHelper) {
    chunkHelper.Do32(control).Do32(ttb).Do32(FSR).Do32(FAR).Do32(CPAR).Do32(ACP).Do32(mmuSwitchCy);
}

template void cp15MMUSave<Savestate<ChunkType>>(ArmCP15MMU* cp15, Savestate<ChunkType>& savestate);
template void cp15MMUSave<SavestateProbe<ChunkType>>(ArmCP15MMU* cp15,
                                                     SavestateProbe<ChunkType>& savestate);
template void cp15MMULoad<SavestateLoader<ChunkType>>(ArmCP15MMU* cp15,
                                                      SavestateLoader<ChunkType>& loader);

void cp15MMUArm920Capture(const ArmCP15MMU* cp, Arm920SavedState& s) {
    s.control = cp->control;
    s.ttb = cp->ttb;
    s.fsr = cp->FSR;
    s.far = cp->FAR;
    s.mmuSwitchCy = cp->mmuSwitchCy;
    s.icacheIndexCompatibility = cp->arm920IcacheIndexCompatibility;
    const auto& dc = cp->dataCache;
    for (unsigned segment = 0; segment < 8; ++segment)
        for (unsigned index = 0; index < 64; ++index) {
            const auto& from = dc.lines[segment][index];
            auto& to = s.dataLines[segment][index];
            to.va = from.va;
            to.pa = from.pa;
            memcpy(to.words, from.words, sizeof(to.words));
            to.dirty = from.dirty;
            to.valid = from.valid;
            to.bufferable = from.bufferable;
        }
    memcpy(s.dcacheVictim, dc.victim, sizeof(s.dcacheVictim));
    s.dcacheRandom = dc.random;
    s.guardAddress = dc.guard.address;
    s.guardSize = dc.guard.size;
    s.guardWrite = dc.guard.write;
    s.guardActive = dc.guard.active;
}

void cp15MMUArm920Apply(ArmCP15MMU* cp, const Arm920SavedState& s) {
    cp->control = s.control;
    cp->ttb = s.ttb;
    cp->FSR = s.fsr;
    cp->FAR = s.far;
    cp->mmuSwitchCy = s.mmuSwitchCy;
    auto& dc = cp->dataCache;
    for (unsigned segment = 0; segment < 8; ++segment)
        for (unsigned index = 0; index < 64; ++index) {
            const auto& from = s.dataLines[segment][index];
            auto& to = dc.lines[segment][index];
            to.va = from.va;
            to.pa = from.pa;
            memcpy(to.words, from.words, sizeof(to.words));
            to.dirty = from.dirty;
            to.valid = from.valid;
            to.bufferable = from.bufferable;
        }
    memcpy(dc.victim, s.dcacheVictim, sizeof(dc.victim));
    dc.random = s.dcacheRandom;
    dc.guard = {s.guardAddress, s.guardSize, s.guardWrite, s.guardActive};
}

void cp15MMUEmitArm920Diagnostic(const ArmCP15MMU* cp, const ArmDiagnosticSink& sink) {
    const uint32_t state[] = {cp->control,cp->ttb,cp->FSR,cp->FAR,cp->mmuSwitchCy,
        uint32_t(cp->arm920Bootstrap),uint32_t(cp->arm920IcacheIndexCompatibility)};
    sink.emit("cp15",state,7);
    const auto& dc = cp->dataCache;
    const uint32_t guard[] = {dc.guard.address,dc.guard.size,uint32_t(dc.guard.write),uint32_t(dc.guard.active)};
    sink.emit("dcache-guard",guard,4);
    const uint32_t replacement[] = {dc.random,dc.victim[0],dc.victim[1],dc.victim[2],dc.victim[3],
        dc.victim[4],dc.victim[5],dc.victim[6],dc.victim[7]};
    sink.emit("dcache-replacement",replacement,9);
    for (unsigned segment=0;segment<8;++segment) for (unsigned index=0;index<64;++index) {
        const auto& line = dc.lines[segment][index];
        if (!line.valid) continue;
        const uint32_t row[] = {segment,index,line.va,line.pa,line.dirty,uint32_t(line.bufferable),
            line.words[0],line.words[1],line.words[2],line.words[3],line.words[4],line.words[5],line.words[6],line.words[7]};
        sink.emit("dcache-line",row,14);
    }
    // Buffered misses drain synchronously in Arm920DataCache::access; no queued writes exist.
    const uint32_t pending = 0;
    sink.emit("pending-write-buffer-words",&pending,1);
}
