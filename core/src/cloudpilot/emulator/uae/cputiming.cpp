// 68000 instruction timing, built from UAE's decoded opcode table and the Motorola MC68000
// user's manual, section 8 (the DragonBall's 68EC000 core has the same counts). Counts are in
// clocks with no wait states; the bus adds those per access.

#include "cputiming.h"

#include <cstdlib>

#include "readcpu.h"

extern "C" {
uae_u16 gCpuTiming[65536];
uae_u32 gUaeAccessWait;
uae_u32 gUaeTimingExtra;
uae_u32 gUaeBusCount;
uae_u32 gUaeSdramWriteWords;
}

namespace {
    struct Timing {
        int clocks;
        int programReads;
        int kind;
    };

    bool IsMemory(int mode) { return mode != Dreg && mode != Areg; }

    bool IsRegisterOrImmediate(int mode) {
        return mode == Dreg || mode == Areg || mode == imm || mode == imm0 || mode == imm1 ||
               mode == imm2;
    }

    // Extension words an operand adds to the instruction.
    int ExtensionWords(int mode, int size) {
        switch (mode) {
            case Ad16:
            case Ad8r:
            case absw:
            case PC16:
            case PC8r:
            case imm0:
            case imm1:
                return 1;
            case absl:
            case imm2:
                return 2;
            case imm:
                return size == sz_long ? 2 : 1;
            default:
                return 0;
        }
    }

    // Table 8-1: effective address time, including extension words and the operand fetch.
    int EaTime(int mode, int size) {
        const bool l = size == sz_long;
        switch (mode) {
            case Aind:
            case Aipi:
                return l ? 8 : 4;
            case Apdi:
                return l ? 10 : 6;
            case Ad16:
            case absw:
            case PC16:
                return l ? 12 : 8;
            case Ad8r:
            case PC8r:
                return l ? 14 : 10;
            case absl:
                return l ? 16 : 12;
            case imm:
            case imm0:
            case imm1:
            case imm2:
                return l ? 8 : 4;
            default:
                return 0;
        }
    }

    // Tables 8-2 and 8-3: MOVE destination time, added to the source's effective address time.
    int MoveDestination(int mode, int size) {
        const bool l = size == sz_long;
        switch (mode) {
            case Dreg:
            case Areg:
                return 4;
            case Aind:
            case Aipi:
            case Apdi:
                return l ? 12 : 8;
            case Ad16:
            case absw:
                return l ? 16 : 12;
            case Ad8r:
                return l ? 18 : 14;
            case absl:
                return l ? 20 : 16;
            default:
                return 4;
        }
    }

    // Table 8-10 columns: (An), (An)+, -(An), (d16,An), (d8,An,Xn), (xxx).W, (xxx).L, (d16,PC),
    // (d8,PC,Xn).
    int ControlColumn(int mode) {
        switch (mode) {
            case Aind:
                return 0;
            case Aipi:
                return 1;
            case Apdi:
                return 2;
            case Ad16:
                return 3;
            case Ad8r:
                return 4;
            case absw:
                return 5;
            case absl:
                return 6;
            case PC16:
                return 7;
            default:
                return 8;
        }
    }

    Timing Compute(uae_u32 opcode, const instr& in) {
        const int size = in.size;
        const bool l = size == sz_long;
        const int sm = in.smode, dm = in.dmode;
        const int length = 1 + ExtensionWords(sm, size) + ExtensionWords(dm, size);
        Timing t{0, length, kTimingPlain};

        switch (in.mnemo) {
            case i_MOVE:
            case i_MOVEA:
                t.clocks = EaTime(sm, size) + MoveDestination(dm, size);
                break;

            case i_ADD:
            case i_SUB:
            case i_AND:
            case i_OR:
            case i_EOR:
            case i_CMP: {
                const bool cmp = in.mnemo == i_CMP;
                if ((opcode >> 12) == 0) {
                    // Immediate forms (Table 8-5): ORI, ANDI, SUBI, ADDI, EORI, CMPI.
                    const bool andOrCmp = in.mnemo == i_AND || cmp;
                    if (dm == Dreg)
                        t.clocks = l ? (andOrCmp ? 14 : 16) : 8;
                    else
                        t.clocks = (cmp ? (l ? 12 : 8) : (l ? 20 : 12)) + EaTime(dm, size);
                } else if ((opcode >> 12) == 5) {
                    // ADDQ and SUBQ.
                    if (dm == Dreg)
                        t.clocks = l ? 8 : 4;
                    else if (dm == Areg)
                        t.clocks = 8;
                    else
                        t.clocks = (l ? 12 : 8) + EaTime(dm, size);
                } else if (in.mnemo == i_EOR || IsMemory(dm)) {
                    // op Dn,<ea>; EOR always has its data register as source.
                    if (dm == Dreg)
                        t.clocks = l ? 8 : 4;
                    else
                        t.clocks = (l ? 12 : 8) + EaTime(dm, size);
                } else {
                    // op <ea>,Dn.
                    if (cmp)
                        t.clocks = (l ? 6 : 4) + EaTime(sm, size);
                    else
                        t.clocks = (l ? (IsRegisterOrImmediate(sm) ? 8 : 6) : 4) + EaTime(sm, size);
                }
                break;
            }

            case i_ADDA:
            case i_SUBA:
                if ((opcode >> 12) == 5)
                    t.clocks = 8;  // ADDQ/SUBQ to An
                else
                    t.clocks = (l ? (IsRegisterOrImmediate(sm) ? 8 : 6) : 8) + EaTime(sm, size);
                break;

            case i_CMPA:
                t.clocks = 6 + EaTime(sm, size);
                break;

            case i_ORSR:
            case i_ANDSR:
            case i_EORSR:
                t.clocks = 20;
                t.programReads = 3;
                break;

            case i_ADDX:
            case i_SUBX:
                t.clocks = sm == Dreg ? (l ? 8 : 4) : (l ? 30 : 18);
                break;

            case i_ABCD:
            case i_SBCD:
                t.clocks = sm == Dreg ? 6 : 18;
                break;

            case i_CMPM:
                t.clocks = l ? 20 : 12;
                break;

            case i_CLR:
            case i_NEG:
            case i_NEGX:
            case i_NOT:
                t.clocks = sm == Dreg ? (l ? 6 : 4) : (l ? 12 : 8) + EaTime(sm, size);
                if (in.mnemo == i_CLR && sm != Dreg) t.kind = kTimingReadFirst;
                break;

            case i_NBCD:
                t.clocks = sm == Dreg ? 6 : 8 + EaTime(sm, size);
                break;

            case i_TST:
                t.clocks = 4 + EaTime(sm, size);
                break;

            case i_TAS:
                t.clocks = sm == Dreg ? 4 : 14 + EaTime(sm, size);
                break;

            case i_Scc:
                if (sm == Dreg) {
                    t.clocks = 4;
                    t.kind = kTimingSccReg;
                } else {
                    t.clocks = 8 + EaTime(sm, size);
                    t.kind = kTimingReadFirst;
                }
                break;

            case i_ASR:
            case i_ASL:
            case i_LSR:
            case i_LSL:
            case i_ROL:
            case i_ROR:
            case i_ROXL:
            case i_ROXR:
                t.clocks = l ? 8 : 6;
                if (sm == Dreg) {
                    t.kind = kTimingShiftReg;
                } else {
                    const int count = (opcode >> 9) & 7;
                    t.clocks += 2 * (count ? count : 8);
                }
                break;

            case i_ASRW:
            case i_ASLW:
            case i_LSRW:
            case i_LSLW:
            case i_ROLW:
            case i_RORW:
            case i_ROXLW:
            case i_ROXRW:
                t.clocks = 8 + EaTime(sm, sz_word);
                break;

            case i_BTST:
            case i_BCHG:
            case i_BCLR:
            case i_BSET: {
                const bool dynamic = sm == Dreg;
                const bool test = in.mnemo == i_BTST;
                if (dm == Dreg) {
                    if (test) {
                        t.clocks = dynamic ? 6 : 10;
                    } else {
                        t.clocks = (dynamic ? 6 : 10) + (in.mnemo == i_BCLR ? 2 : 0);
                        t.kind = dynamic ? kTimingBitReg : kTimingBitImm;
                    }
                } else {
                    t.clocks = (dynamic ? (test ? 4 : 8) : (test ? 8 : 12)) + EaTime(dm, sz_byte);
                }
                break;
            }

            case i_Bcc:
                if ((opcode & 0xff) == 0) {
                    t.clocks = 12;
                    t.programReads = 2;
                    t.kind = kTimingBccWord;
                } else {
                    t.clocks = 8;
                    t.programReads = 1;
                    t.kind = kTimingBccShort;
                }
                break;

            case i_BSR:
                t.clocks = 18;
                t.programReads = 2;
                break;

            case i_DBcc:
                t.clocks = 10;
                t.programReads = 2;
                t.kind = kTimingDBcc;
                break;

            case i_JMP: {
                static const int clocks[] = {8, 0, 0, 10, 14, 10, 12, 10, 14};
                const int c = ControlColumn(sm);
                t.clocks = clocks[c];
                t.programReads = (c == 4 || c == 8) ? 3 : (length < 2 ? 2 : length);
                break;
            }

            case i_JSR: {
                static const int clocks[] = {16, 0, 0, 18, 22, 18, 20, 18, 22};
                t.clocks = clocks[ControlColumn(sm)];
                t.programReads = length < 2 ? 2 : length;
                break;
            }

            case i_LEA: {
                static const int clocks[] = {4, 0, 0, 8, 12, 8, 12, 8, 12};
                t.clocks = clocks[ControlColumn(sm)];
                break;
            }

            case i_PEA: {
                static const int clocks[] = {12, 0, 0, 16, 20, 16, 20, 16, 20};
                t.clocks = clocks[ControlColumn(sm)];
                break;
            }

            case i_MVMEL: {
                // Memory to registers; the 68000 reads one extra word past the last register.
                static const int clocks[] = {12, 12, 0, 16, 18, 16, 20, 16, 18};
                t.clocks = clocks[ControlColumn(dm)];
                t.programReads = length + 1;
                t.kind = l ? kTimingMovemLong : kTimingMovemWord;
                break;
            }

            case i_MVMLE: {
                static const int clocks[] = {8, 0, 8, 12, 14, 12, 16, 0, 0};
                t.clocks = clocks[ControlColumn(dm)];
                t.kind = l ? kTimingMovemLong : kTimingMovemWord;
                break;
            }

            case i_MULU:
            case i_MULS:
            case i_DIVU:
            case i_DIVS:
                // The handler adds the data-dependent part (gUaeTimingExtra).
                t.clocks = EaTime(sm, sz_word);
                break;

            case i_CHK:
                t.clocks = 10 + EaTime(sm, sz_word);
                break;

            case i_MVSR2:
                t.clocks = sm == Dreg ? 6 : 8 + EaTime(sm, sz_word);
                if (sm != Dreg) t.kind = kTimingReadFirst;
                break;

            case i_MV2SR:
                t.clocks = 12 + EaTime(sm, sz_word);
                if (size == sz_word && length < 2) t.programReads = 2;
                break;

            case i_EXG:
                t.clocks = 6;
                break;

            case i_EXT:
            case i_SWAP:
            case i_NOP:
            case i_MVR2USP:
            case i_MVUSP2R:
            case i_TRAPV:
                t.clocks = 4;
                break;

            case i_LINK:
                t.clocks = 16;
                break;

            case i_UNLK:
                t.clocks = 12;
                break;

            case i_RESET:
                t.clocks = 132;
                break;

            case i_RTE:
            case i_RTR:
                t.clocks = 20;
                t.programReads = 2;
                break;

            case i_RTS:
                t.clocks = 16;
                t.programReads = 2;
                break;

            case i_STOP:
                t.clocks = 4;
                t.programReads = 0;
                break;

            case i_MVPRM:
            case i_MVPMR:
                t.clocks = l ? 24 : 16;
                break;

            case i_TRAP:
            default:
                // TRAP and illegal opcodes: the exception carries the whole cost.
                t.clocks = 0;
                t.programReads = 0;
                break;
        }

        return t;
    }
}  // namespace

extern "C" void BuildCpuTiming(void) {
    for (uae_u32 opcode = 0; opcode < 65536; opcode++) {
        const instr& in = table68k[opcode];
        Timing t{0, 0, kTimingPlain};
        if (in.mnemo != i_ILLG && in.clev == 0) t = Compute(opcode, in);
        if (t.clocks < 0 || t.clocks > 510 || (t.clocks & 1) || t.programReads > 15) abort();
        gCpuTiming[opcode] = uae_u16((t.clocks >> 1) | (t.programReads << 8) | (t.kind << 12));
    }
}

extern "C" uae_u32 TimingMulu(uae_u16 source) {
    return 38 + 2 * __builtin_popcount(source);
}

// MULS counts the 01 and 10 pairs in the source with a zero appended below it.
extern "C" uae_u32 TimingMuls(uae_u16 source) {
    const uae_u32 bits = uae_u32(source) << 1;
    return 38 + 2 * __builtin_popcount((bits ^ (bits >> 1)) & 0xffff);
}

// DIVU and DIVS follow the 68000's microcode step by step (as measured by Jorge Cwik). A zero
// divisor costs nothing here; the exception adds its time.
extern "C" uae_u32 TimingDivu(uae_u32 dividend, uae_u16 divisor) {
    if (divisor == 0) return 0;
    if ((dividend >> 16) >= divisor) return 10;

    uae_u32 mcycles = 38;
    const uae_u32 hdivisor = uae_u32(divisor) << 16;
    for (int i = 0; i < 15; i++) {
        const uae_u32 before = dividend;
        dividend <<= 1;
        if (before & 0x80000000) {
            dividend -= hdivisor;
        } else {
            mcycles += 2;
            if (dividend >= hdivisor) {
                dividend -= hdivisor;
                mcycles--;
            }
        }
    }
    return mcycles * 2;
}

extern "C" uae_u32 TimingDivs(uae_s32 dividend, uae_s16 divisor) {
    if (divisor == 0) return 0;

    uae_u32 mcycles = 6;
    if (dividend < 0) mcycles++;

    const uae_u32 absDividend = dividend < 0 ? uae_u32(0) - uae_u32(dividend) : uae_u32(dividend);
    const uae_u32 absDivisor = divisor < 0 ? uae_u32(-int(divisor)) : uae_u32(divisor);
    if ((absDividend >> 16) >= absDivisor) return (mcycles + 2) * 2;

    uae_u32 quotient = absDividend / absDivisor;
    mcycles += 55;
    if (divisor >= 0) {
        if (dividend >= 0)
            mcycles--;
        else
            mcycles++;
    }
    for (int i = 0; i < 15; i++) {
        if (!(quotient & 0x8000)) mcycles++;
        quotient <<= 1;
    }
    return mcycles * 2;
}
