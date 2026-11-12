//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

// Intel Wireless MMX (iWMMXt) on coprocessors 0 and 1 of the PXA27x.
// Encodings follow the iWMMXt opcode tables in GNU binutils. Lanes are taken from the 64-bit
// value, lane 0 in the low bits, so the code doesn't depend on the host's byte order.

#include "pxa270_WMMX.h"

#include <cstdlib>
#include <cstring>

#include "CPEndian.h"
#include "cputil.h"
#include "savestate/savestateAll.h"

union REG64 {
    uint64_t v64;
    int64_t s64;
    uint32_t v32[2];
    int32_t s32[2];
    uint16_t v16[4];
    int16_t s16[4];
    uint8_t v8[8];
    int8_t s8[8];
};

struct Pxa270wmmx {
    union REG64 wR[16];
    uint32_t wCGR[4], wCASF;  // wCASF: an NZCV nibble per lane
    uint8_t wCon, wCSSF;

    template <typename T>
    void DoSaveLoad(T &h) {
        for (auto &r : wR) h.Do64(r.v64);
        h.DoBuffer32(wCGR, 4).Do32(wCASF).Do8(wCon).Do8(wCSSF);
    }
};

#if __BYTE_ORDER == __LITTLE_ENDIAN
    #define ACCESS_REG_32(_idx) (_idx)
#else
    #define ACCESS_REG_32(_idx) (1 - (_idx))
#endif

namespace {

constexpr uint32_t kFlagN = 8, kFlagZ = 4, kFlagC = 2, kFlagV = 1;

// wCon: MUP (bit 1) after a data register write, CUP (bit 0) after a control register write.
void dataChanged(Pxa270wmmx *w) { w->wCon |= 2; }
void controlChanged(Pxa270wmmx *w) { w->wCon |= 1; }

uint64_t laneMask(unsigned bits) { return bits == 64 ? ~0ull : (1ull << bits) - 1; }

uint64_t lane(uint64_t v, unsigned bits, unsigned i) {
    return bits == 64 ? v : (v >> (i * bits)) & laneMask(bits);
}

int64_t slane(uint64_t v, unsigned bits, unsigned i) {
    uint64_t x = lane(v, bits, i);
    if (bits < 64 && (x >> (bits - 1))) x |= ~laneMask(bits);
    return (int64_t)x;
}

void setLane(uint64_t &v, unsigned bits, unsigned i, uint64_t x) {
    if (bits == 64) {
        v = x;
        return;
    }
    v &= ~(laneMask(bits) << (i * bits));
    v |= (x & laneMask(bits)) << (i * bits);
}

// The NZCV nibble for lane i of a `bits` wide element sits at the top of that lane's share of
// the 32-bit wCASF: bits 4i (byte), 8i+4 (halfword), 16i+12 (word), 28 (doubleword).
unsigned flagShift(unsigned bits, unsigned i) { return (i + 1) * (bits / 2) - 4; }

uint32_t laneNZ(uint64_t v, unsigned bits, unsigned i) {
    uint64_t x = lane(v, bits, i);
    return ((x >> (bits - 1)) & 1 ? kFlagN : 0) | (x ? 0 : kFlagZ);
}

void setFlagsNZ(Pxa270wmmx *w, uint64_t v, unsigned bits) {
    uint32_t f = 0;
    for (unsigned i = 0; i < 64 / bits; i++) f |= laneNZ(v, bits, i) << flagShift(bits, i);
    w->wCASF = f;
    controlChanged(w);
}

void setRegNZ(Pxa270wmmx *w, unsigned d, uint64_t v, unsigned bits) {
    w->wR[d].v64 = v;
    dataChanged(w);
    setFlagsNZ(w, v, bits);
}

void setReg(Pxa270wmmx *w, unsigned d, uint64_t v) {
    w->wR[d].v64 = v;
    dataChanged(w);
}

// The element size in bits 23:22 of the opcode (op1 bits 3:2): 0 byte, 1 halfword, 2 word.
unsigned sizeBits(unsigned code) { return code == 0 ? 8 : code == 1 ? 16 : code == 2 ? 32 : 0; }

// Writes a core register; r15 means the CPSR flags, as for TANDC, TORC and TEXTRC.
void setCoreReg(struct ArmCpu *cpu, uint_fast8_t reg, uint32_t val) {
    if (reg == 15) {
        val &= 0xf0000000ul;
        val |= cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR) & ~0xf0000000ul;
        reg = ARM_REG_NUM_CPSR;
    }
    cpuSetReg(cpu, reg, val);
}

int64_t saturate(int64_t x, int64_t lo, int64_t hi, bool &sat) {
    if (x < lo) {
        sat = true;
        return lo;
    }
    if (x > hi) {
        sat = true;
        return hi;
    }
    return x;
}

// WADD and WSUB. sat: 0 wrapping, 1 unsigned saturation, 3 signed saturation.
bool addSub(Pxa270wmmx *w, bool sub, unsigned op1, unsigned d, unsigned n, unsigned m) {
    const unsigned bits = sizeBits(op1 >> 2), sat = op1 & 3;
    if (!bits || sat == 2) return false;
    const uint64_t a = w->wR[n].v64, b = w->wR[m].v64, mask = laneMask(bits);
    uint64_t r = 0;
    uint32_t f = 0;
    for (unsigned i = 0; i < 64 / bits; i++) {
        const uint64_t ua = lane(a, bits, i), ub = lane(b, bits, i);
        const int64_t sa = slane(a, bits, i), sb = slane(b, bits, i);
        const uint64_t raw = (sub ? ua - ub : ua + ub) & mask;
        uint32_t nibble = 0;
        if (sub ? ua >= ub : ((ua + ub) >> bits) & 1) nibble |= kFlagC;
        const int64_t sr = sub ? sa - sb : sa + sb;
        if (sr < -(int64_t(1) << (bits - 1)) || sr >= (int64_t(1) << (bits - 1))) nibble |= kFlagV;
        uint64_t out = raw;
        bool saturated = false;
        if (sat == 1)
            out = (uint64_t)saturate(sub ? (int64_t)ua - (int64_t)ub : (int64_t)(ua + ub), 0, (int64_t)mask,
                                     saturated);
        else if (sat == 3)
            out = (uint64_t)saturate(sr, -(int64_t(1) << (bits - 1)), (int64_t(1) << (bits - 1)) - 1, saturated);
        if (saturated) w->wCSSF |= 1 << (i * bits / 8);
        setLane(r, bits, i, out);
        nibble |= laneNZ(r, bits, i);
        f |= nibble << flagShift(bits, i);
    }
    setReg(w, d, r);
    w->wCASF = f;
    controlChanged(w);
    return true;
}

// Shift and rotate counts come from the low byte of wRm or of wCGRm (CRm 8 to 11).
bool shift(Pxa270wmmx *w, bool fromCgr, unsigned op1, unsigned d, unsigned n, unsigned m) {
    const unsigned bits = (op1 >> 2) == 1 ? 16 : (op1 >> 2) == 2 ? 32 : (op1 >> 2) == 3 ? 64 : 0;
    if (!bits) return false;
    uint32_t count;
    if (fromCgr) {
        if (m < 8 || m > 11) return false;
        count = w->wCGR[m - 8] & 0xff;
    } else {
        count = w->wR[m].v64 & 0xff;
    }
    const uint64_t a = w->wR[n].v64, mask = laneMask(bits);
    uint64_t r = 0;
    for (unsigned i = 0; i < 64 / bits; i++) {
        const uint64_t x = lane(a, bits, i);
        uint64_t out;
        switch (op1 & 3) {
            case 0:  // WSRA
                out = (uint64_t)(slane(a, bits, i) >> (count >= bits ? bits - 1 : count));
                break;
            case 1:  // WSLL
                out = count >= bits ? 0 : x << count;
                break;
            case 2:  // WSRL
                out = count >= bits ? 0 : x >> count;
                break;
            default: {  // WROR
                const unsigned by = count % bits;
                out = by ? (x >> by) | (x << (bits - by)) : x;
                break;
            }
        }
        setLane(r, bits, i, out & mask);
    }
    setRegNZ(w, d, r, bits);
    return true;
}

// Coprocessor 0 CDP, by op2.
bool cdp0(Pxa270wmmx *w, unsigned op1, unsigned op2, unsigned d, unsigned n, unsigned m) {
    const uint64_t a = w->wR[n].v64, b = w->wR[m].v64;
    switch (op2) {
        case 0:
            switch (op1) {
                case 0:  // WOR
                    setRegNZ(w, d, a | b, 64);
                    return true;
                case 1:  // WXOR
                    setRegNZ(w, d, a ^ b, 64);
                    return true;
                case 2:  // WAND
                    setRegNZ(w, d, a & b, 64);
                    return true;
                case 3:  // WANDN
                    setRegNZ(w, d, a & ~b, 64);
                    return true;
                case 8:
                case 9:
                case 12:
                case 13: {  // WAVG2B, WAVG2BR, WAVG2H, WAVG2HR
                    const unsigned bits = (op1 & 4) ? 16 : 8, round = op1 & 1;
                    uint64_t r = 0;
                    for (unsigned i = 0; i < 64 / bits; i++)
                        setLane(r, bits, i, (lane(a, bits, i) + lane(b, bits, i) + round) >> 1);
                    setRegNZ(w, d, r, bits);
                    return true;
                }
                default:
                    return false;
            }

        case 1: {  // WALIGNI (op1 0 to 7 is the byte offset), WALIGNR0 to 3 (op1 8 to 11)
            unsigned by;
            if (op1 < 8)
                by = op1;
            else if (op1 < 12)
                by = w->wCGR[op1 - 8] & 7;
            else
                return false;
            // The low 8 bytes of wRm:wRn shifted right by `by` bytes.
            setReg(w, d, by ? (a >> (by * 8)) | (b << (64 - by * 8)) : a);
            return true;
        }

        case 2:
            return shift(w, false, op1, d, n, m);

        case 3: {  // WCMPEQ (op1 bits 1:0 = 0), WCMPGTU (1), WCMPGTS (3)
            const unsigned bits = sizeBits(op1 >> 2), kind = op1 & 3;
            if (!bits || kind == 2) return false;
            uint64_t r = 0;
            for (unsigned i = 0; i < 64 / bits; i++) {
                bool yes = kind == 0   ? lane(a, bits, i) == lane(b, bits, i)
                           : kind == 1 ? lane(a, bits, i) > lane(b, bits, i)
                                       : slane(a, bits, i) > slane(b, bits, i);
                setLane(r, bits, i, yes ? ~0ull : 0);
            }
            setRegNZ(w, d, r, bits);
            return true;
        }

        case 4: {  // WPACK{H,W,D}{US,SS}: wRn's lanes go in the low half, wRm's in the high half
            const unsigned from = (op1 >> 2) == 1 ? 16 : (op1 >> 2) == 2 ? 32 : (op1 >> 2) == 3 ? 64 : 0;
            if (!from || (op1 & 3) == 0 || (op1 & 3) == 2) return false;
            const bool sign = (op1 & 3) == 3;
            const unsigned to = from / 2, per = 64 / from;
            const int64_t lo = sign ? -(int64_t(1) << (to - 1)) : 0;
            const int64_t hi = sign ? (int64_t(1) << (to - 1)) - 1 : (int64_t)laneMask(to);
            uint64_t r = 0;
            for (unsigned i = 0; i < 2 * per; i++) {
                bool saturated = false;
                const int64_t x = saturate(slane(i < per ? a : b, from, i % per), lo, hi, saturated);
                if (saturated) w->wCSSF |= 1 << (i * to / 8);
                setLane(r, to, i, (uint64_t)x);
            }
            setRegNZ(w, d, r, to);
            return true;
        }

        case 6:
        case 7: {
            // op2 6 takes the high half, 7 the low half. op1 bit 0 clear: WUNPCKE{H,L}{U,S}
            // extends wRn (bit 1 signed). Set: WUNPCKI{H,L} interleaves wRn and wRm.
            const unsigned bits = sizeBits(op1 >> 2), half = op2 == 6 ? 64 / bits / 2 : 0;
            if (!bits) return false;
            uint64_t r = 0;
            if (op1 & 1) {
                if (op1 & 2) return false;
                for (unsigned i = 0; i < 64 / bits; i++)
                    setLane(r, bits, i, lane(i & 1 ? b : a, bits, half + i / 2));
                setRegNZ(w, d, r, bits);
            } else {
                if (m) return false;
                const bool sign = op1 & 2;
                for (unsigned i = 0; i < 32 / bits; i++)
                    setLane(r, bits * 2, i, sign ? (uint64_t)slane(a, bits, half + i) : lane(a, bits, half + i));
                setRegNZ(w, d, r, bits * 2);
            }
            return true;
        }

        default:
            return false;
    }
}

// Coprocessor 1 CDP, by op2.
bool cdp1(Pxa270wmmx *w, unsigned op1, unsigned op2, unsigned d, unsigned n, unsigned m) {
    const uint64_t a = w->wR[n].v64, b = w->wR[m].v64;
    switch (op2) {
        case 0:
            switch (op1) {
                case 0:  // WMULUL
                case 1:  // WMULUM
                case 2:  // WMULSL
                case 3: {  // WMULSM
                    uint64_t r = 0;
                    for (unsigned i = 0; i < 4; i++) {
                        const int64_t p = (op1 & 2) ? slane(a, 16, i) * slane(b, 16, i)
                                                    : (int64_t)(lane(a, 16, i) * lane(b, 16, i));
                        setLane(r, 16, i, (uint64_t)((op1 & 1) ? p >> 16 : p));
                    }
                    setReg(w, d, r);
                    return true;
                }
                case 4:  // WMACU
                case 5:  // WMACUZ
                case 6:  // WMACS
                case 7: {  // WMACSZ
                    uint64_t sum = (op1 & 1) ? 0 : w->wR[d].v64;
                    for (unsigned i = 0; i < 4; i++)
                        sum += (op1 & 2) ? (uint64_t)(slane(a, 16, i) * slane(b, 16, i))
                                         : lane(a, 16, i) * lane(b, 16, i);
                    setReg(w, d, sum);
                    return true;
                }
                case 8:     // WMADDU
                case 10: {  // WMADDS
                    uint64_t r = 0;
                    for (unsigned i = 0; i < 2; i++) {
                        uint64_t s;
                        if (op1 & 2)
                            s = (uint64_t)(slane(a, 16, 2 * i) * slane(b, 16, 2 * i) +
                                           slane(a, 16, 2 * i + 1) * slane(b, 16, 2 * i + 1));
                        else
                            s = lane(a, 16, 2 * i) * lane(b, 16, 2 * i) +
                                lane(a, 16, 2 * i + 1) * lane(b, 16, 2 * i + 1);
                        setLane(r, 32, i, s);
                    }
                    setReg(w, d, r);
                    return true;
                }
                default:
                    return false;
            }

        case 1: {  // WSADB, WSADBZ, WSADH, WSADHZ: the sum goes in the low word, the high word is cleared
            if (op1 != 0 && op1 != 1 && op1 != 4 && op1 != 5) return false;
            const unsigned bits = (op1 & 4) ? 16 : 8;
            uint32_t sum = (op1 & 1) ? 0 : (uint32_t)w->wR[d].v64;
            for (unsigned i = 0; i < 64 / bits; i++) {
                const uint64_t x = lane(a, bits, i), y = lane(b, bits, i);
                sum += (uint32_t)(x > y ? x - y : y - x);
            }
            setReg(w, d, sum);
            return true;
        }

        case 2:  // WSRAG, WSLLG, WSRLG, WRORG
            return shift(w, true, op1, d, n, m);

        case 3: {  // WMAX, WMIN: op1 bit 0 min, bit 1 signed, bits 3:2 size
            const unsigned bits = sizeBits(op1 >> 2);
            if (!bits) return false;
            const bool min = op1 & 1, sign = op1 & 2;
            uint64_t r = 0;
            for (unsigned i = 0; i < 64 / bits; i++) {
                const bool aBigger = sign ? slane(a, bits, i) > slane(b, bits, i) : lane(a, bits, i) > lane(b, bits, i);
                setLane(r, bits, i, lane(aBigger != min ? a : b, bits, i));
            }
            setReg(w, d, r);
            return true;
        }

        case 4:
            return addSub(w, false, op1, d, n, m);

        case 5:
            return addSub(w, true, op1, d, n, m);

        case 6: {  // WACCB, WACCH, WACCW: the unsigned sum of wRn's lanes
            const unsigned bits = sizeBits(op1 >> 2);
            if (!bits || (op1 & 3) || m) return false;
            uint64_t sum = 0;
            for (unsigned i = 0; i < 64 / bits; i++) sum += lane(a, bits, i);
            setReg(w, d, sum);
            return true;
        }

        case 7: {  // WSHUFH: an 8-bit immediate in op1:CRm, two bits per result halfword
            unsigned which = (op1 << 4) | m;
            uint64_t r = 0;
            for (unsigned i = 0; i < 4; i++, which >>= 2) setLane(r, 16, i, lane(a, 16, which & 3));
            setRegNZ(w, d, r, 16);
            return true;
        }

        default:
            return false;
    }
}

bool dataProcessing(Pxa270wmmx *w, bool cp1, bool two, uint8_t op1, uint8_t CRd, uint8_t CRn,
                    uint8_t CRm, uint8_t op2) {
    if (two) return false;
    return cp1 ? cdp1(w, op1, op2, CRd, CRn, CRm) : cdp0(w, op1, op2, CRd, CRn, CRm);
}

bool readControl(Pxa270wmmx *w, unsigned reg, uint32_t &val) {
    switch (reg) {
        case 0:  // wCID
            val = 0x69051000ul;
            return true;
        case 1:  // wCon
            val = w->wCon;
            return true;
        case 2:  // wCSSF
            val = w->wCSSF;
            return true;
        case 3:  // wCASF
            val = w->wCASF;
            return true;
        case 8:
        case 9:
        case 10:
        case 11:  // wCGR0 to 3
            val = w->wCGR[reg - 8];
            return true;
        default:
            return false;
    }
}

// The control registers WLDRW and TMCR can write: wCID is read only.
bool controlWritable(unsigned reg) { return (reg >= 1 && reg <= 3) || (reg >= 8 && reg <= 11); }

// wCID is read only.
bool writeControl(Pxa270wmmx *w, unsigned reg, uint32_t val) {
    switch (reg) {
        case 1:
            w->wCon = val & 3;
            return true;
        case 2:
            w->wCSSF = val & 0xff;
            break;
        case 3:
            w->wCASF = val;
            break;
        case 8:
        case 9:
        case 10:
        case 11:
            w->wCGR[reg - 8] = val;
            break;
        default:
            return false;
    }
    controlChanged(w);
    return true;
}

// TMIA, TMIAPH, TMIAxy (MCR op1 1): wRd is bits 8:5 (the coprocessor bit and op2), CRn picks
// the kind, vs is Rs (the Rx field) and vm is Rm (the CRm field).
bool tmia(Pxa270wmmx *w, bool cp1, unsigned CRn, unsigned op2, uint32_t vm, uint32_t vs) {
    const unsigned d = op2 | (cp1 ? 8 : 0);
    int64_t add;
    switch (CRn) {
        case 0:  // TMIA
            add = (int64_t)(int32_t)vm * (int32_t)vs;
            break;
        case 8:  // TMIAPH
            add = (int64_t)(int16_t)vm * (int16_t)vs + (int64_t)(int16_t)(vm >> 16) * (int16_t)(vs >> 16);
            break;
        case 12:
        case 13:
        case 14:
        case 15:  // TMIABB, TMIABT, TMIATB, TMIATT: x picks Rm's half, y Rs's
            add = (int64_t)(int16_t)((CRn & 2) ? vm >> 16 : vm) * (int16_t)((CRn & 1) ? vs >> 16 : vs);
            break;
        default:
            return false;
    }
    setReg(w, d, w->wR[d].v64 + (uint64_t)add);
    return true;
}

// MRC: the value for the core register. TANDC, TORC and TEXTRC give the flags in bits 31:28,
// which go to the CPSR when the register is r15.
bool mrc(Pxa270wmmx *w, bool cp1, unsigned op1, unsigned CRn, unsigned CRm, unsigned op2, uint32_t &out) {
    if (op1 == 0 && cp1 && op2 == 0) {  // TMRC
        if (CRm) return false;
        return readControl(w, CRn, out);
    }
    // The rest have the element size in op1 bits 2:1 (byte, halfword, word).
    if (op1 & 1) return false;
    const unsigned size = op1 >> 1, bits = sizeBits(size);
    if (!bits) return false;
    switch (op2) {
        case 1:
            if (CRm) return false;
            if (!cp1) {  // TMOVMSK: the top bit of each lane of wRn
                uint32_t val = 0;
                for (unsigned i = 0; i < 64 / bits; i++)
                    val |= (uint32_t)((lane(w->wR[CRn].v64, bits, i) >> (bits - 1)) & 1) << i;
                out = val;
                return true;
            }
            [[fallthrough]];  // TANDC on coprocessor 1
        case 2: {  // TANDC, TORC: every lane's NZCV nibble ANDed or ORed into the CPSR
            if (!cp1 || CRm || CRn != 3) return false;
            const bool isAnd = op2 == 1;
            uint32_t val = isAnd ? 0xf : 0;
            for (unsigned i = 0; i < 64 / bits; i++) {
                const uint32_t nibble = (w->wCASF >> flagShift(bits, i)) & 0xf;
                val = isAnd ? val & nibble : val | nibble;
            }
            out = val << 28;
            return true;
        }
        case 3:
            if (cp1) {  // TEXTRC: one lane's NZCV nibble into the CPSR
                if ((CRm & 8) || CRn != 3) return false;
                const unsigned i = (CRm & 7) % (64 / bits);
                out = ((w->wCASF >> flagShift(bits, i)) & 0xf) << 28;
            } else {  // TEXTRM: CRm bit 3 sign extends, bits 2:0 pick the lane
                const unsigned i = (CRm & 7) % (64 / bits);
                const uint64_t v = w->wR[CRn].v64;
                out = (uint32_t)((CRm & 8) ? (uint64_t)slane(v, bits, i) : lane(v, bits, i));
            }
            return true;
        default:
            return false;
    }
}

// MCR other than TMIA: v is the core register's value.
bool mcr(Pxa270wmmx *w, bool cp1, unsigned op1, unsigned CRn, unsigned CRm, unsigned op2, uint32_t v) {
    if (op1 == 0 && cp1 && op2 == 0) {  // TMCR
        if (CRm) return false;
        return writeControl(w, CRn, v);
    }
    if (cp1) return false;
    const unsigned bits = sizeBits(op2 >> 1);
    if (!bits || (op2 & 1)) return false;
    if (op1 == 2) {  // TBCST: Rn into every lane of wRd
        if (CRm) return false;
        uint64_t r = 0;
        for (unsigned i = 0; i < 64 / bits; i++) setLane(r, bits, i, v);
        setReg(w, CRn, r);
        return true;
    }
    if (op1 == 3) {  // TINSR: Rn into the lane CRm bits 2:0 picks
        if (CRm & 8) return false;
        uint64_t r = w->wR[CRn].v64;
        setLane(r, bits, (CRm & 7) % (64 / bits), v);
        setReg(w, CRn, r);
        return true;
    }
    return false;
}

// MCR and MRC. Rx is bits 15:12, CRn bits 19:16, CRm bits 3:0; op1 is bits 23:21.
bool regXfer(Pxa270wmmx *w, bool cp1, struct ArmCpu *cpu, bool two, bool read, uint8_t op1, uint8_t Rx,
             uint8_t CRn, uint8_t CRm, uint8_t op2) {
    if (two) return false;
    if (op1 == 1) {
        if (read) return false;
        return tmia(w, cp1, CRn, op2, cpuGetRegExternal(cpu, CRm), cpuGetRegExternal(cpu, Rx));
    }
    if (read) {
        uint32_t val;
        if (!mrc(w, cp1, op1, CRn, CRm, op2, val)) return false;
        setCoreReg(cpu, Rx, val);
        return true;
    }
    return mcr(w, cp1, op1, CRn, CRm, op2, cpuGetRegExternal(cpu, Rx));
}

// TMCRR and TMRRC on coprocessor 0 with op 0; the 64-bit value is high:low.
bool mcrr(Pxa270wmmx *w, bool cp1, unsigned op, unsigned wR, uint32_t lo, uint32_t hi) {
    if (cp1 || op) return false;
    setReg(w, wR, ((uint64_t)hi << 32) | lo);
    return true;
}

bool mrrc(Pxa270wmmx *w, bool cp1, unsigned op, unsigned wR, uint64_t &out) {
    if (cp1 || op) return false;
    out = w->wR[wR].v64;
    return true;
}

// TMCRR, TMRRC: Rd (bits 15:12) is the low word, Rn (bits 19:16) the high word.
bool twoReg(Pxa270wmmx *w, bool cp1, struct ArmCpu *cpu, bool read, uint8_t op, uint8_t RdLo,
            uint8_t RdHi, uint8_t wR) {
    if (read) {
        uint64_t v;
        if (!mrrc(w, cp1, op, wR, v)) return false;
        setCoreReg(cpu, RdLo, (uint32_t)v);
        setCoreReg(cpu, RdHi, (uint32_t)(v >> 32));
        return true;
    }
    return mcrr(w, cp1, op, wR, cpuGetRegExternal(cpu, RdLo), cpuGetRegExternal(cpu, RdHi));
}

// The memory part of WLDR and WSTR at ea. Coprocessor 0 moves bytes (N clear) or halfwords
// (N set), coprocessor 1 words or doublewords. The control register forms (WLDRW and WSTRW
// wCx) are LDC2 and STC2 on coprocessor 1. Accesses are checked for the current mode. False
// for an undefined form, or for a failed access with *fault set (fault->fsr nonzero), wR
// unchanged; a WSTRD whose second word fails has written the first.
bool transfer(Pxa270wmmx *w, struct ArmCpu *cpu, bool cp1, bool two, bool N, bool store, unsigned reg, uint32_t ea,
              Pxa270wmmxFault *fault) {
    if (two && (!cp1 || N)) return false;
    const unsigned size = cp1 ? (N && !two ? 8 : 4) : (N ? 2 : 1);
    *fault = Pxa270wmmxFault{};
    const auto access = [&](void *buf, uint32_t va, unsigned bytes) {
        uint_fast8_t fsr = 0;
        if (cpuMemOpGuest(cpu, buf, va, bytes, store, &fsr)) return true;
        *fault = Pxa270wmmxFault{va, uint8_t(fsr ? fsr : 10), uint8_t(bytes), store};
        return false;
    };

    if (two) {
        uint32_t val = 0;
        if (store) {
            if (!readControl(w, reg, val) || !access(&val, ea, 4)) return false;
        } else {
            // Undefined before any access, so a refused register never touches memory.
            if (!controlWritable(reg) || !access(&val, ea, 4)) return false;
            writeControl(w, reg, val);
        }
    } else if (store) {
        union REG64 v = w->wR[reg];
        if (size == 8) {
            if (!access(&v.v32[ACCESS_REG_32(0)], ea, 4) || !access(&v.v32[ACCESS_REG_32(1)], ea + 4, 4))
                return false;
        } else {
            uint32_t x32 = (uint32_t)v.v64;
            uint16_t x16 = (uint16_t)v.v64;
            uint8_t x8 = (uint8_t)v.v64;
            void *buf = size == 4 ? (void *)&x32 : size == 2 ? (void *)&x16 : (void *)&x8;
            if (!access(buf, ea, size)) return false;
        }
    } else {
        uint64_t r;
        if (size == 8) {
            uint32_t lo, hi;
            if (!access(&lo, ea, 4) || !access(&hi, ea + 4, 4)) return false;
            r = ((uint64_t)hi << 32) | lo;
        } else {
            uint32_t x32 = 0;
            uint16_t x16 = 0;
            uint8_t x8 = 0;
            void *buf = size == 4 ? (void *)&x32 : size == 2 ? (void *)&x16 : (void *)&x8;
            if (!access(buf, ea, size)) return false;
            r = size == 4 ? x32 : size == 2 ? x16 : x8;
        }
        setReg(w, reg, r);
    }
    return true;
}

// WLDR and WSTR: offsets on coprocessor 0 are in bytes, on coprocessor 1 times 4. A failed
// access takes the data abort as an ordinary load or store does (so the instruction is
// handled, not undefined); the base is written back only after the access succeeds.
bool memAccess(Pxa270wmmx *w, bool cp1, struct ArmCpu *cpu, bool two, bool N, bool store, uint8_t reg,
               uint_fast8_t addrReg, uint32_t addBefore, uint32_t addAfter, uint8_t *option) {
    if (option) return false;
    const unsigned scale = cp1 ? 4 : 1;
    const uint32_t base = cpuGetRegExternal(cpu, addrReg), ea = base + addBefore * scale;
    Pxa270wmmxFault fault;
    if (!transfer(w, cpu, cp1, two, N, store, reg, ea, &fault)) {
        if (!fault.fsr) return false;
        cpuDataAbortExternal(cpu, fault.addr, fault.size, fault.write, fault.fsr);
        return true;
    }
    if (addAfter) setCoreReg(cpu, addrReg, base + addAfter * scale);
    return true;
}

bool regXfer0(struct ArmCpu *cpu, void *userData, bool two, bool MRC, uint8_t op1, uint8_t Rx, uint8_t CRn,
              uint8_t CRm, uint8_t op2) {
    return regXfer((Pxa270wmmx *)userData, false, cpu, two, MRC, op1, Rx, CRn, CRm, op2);
}

bool regXfer1(struct ArmCpu *cpu, void *userData, bool two, bool MRC, uint8_t op1, uint8_t Rx, uint8_t CRn,
              uint8_t CRm, uint8_t op2) {
    return regXfer((Pxa270wmmx *)userData, true, cpu, two, MRC, op1, Rx, CRn, CRm, op2);
}

bool dataProcessing0(struct ArmCpu *, void *userData, bool two, uint8_t op1, uint8_t CRd, uint8_t CRn,
                     uint8_t CRm, uint8_t op2) {
    return dataProcessing((Pxa270wmmx *)userData, false, two, op1, CRd, CRn, CRm, op2);
}

bool dataProcessing1(struct ArmCpu *, void *userData, bool two, uint8_t op1, uint8_t CRd, uint8_t CRn,
                     uint8_t CRm, uint8_t op2) {
    return dataProcessing((Pxa270wmmx *)userData, true, two, op1, CRd, CRn, CRm, op2);
}

bool memAccess0(struct ArmCpu *cpu, void *userData, bool two, bool N, bool store, uint8_t CRd,
                uint_fast8_t addrReg, uint32_t addBefore, uint32_t addAfter, uint8_t *option) {
    return memAccess((Pxa270wmmx *)userData, false, cpu, two, N, store, CRd, addrReg, addBefore, addAfter, option);
}

bool memAccess1(struct ArmCpu *cpu, void *userData, bool two, bool N, bool store, uint8_t CRd,
                uint_fast8_t addrReg, uint32_t addBefore, uint32_t addAfter, uint8_t *option) {
    return memAccess((Pxa270wmmx *)userData, true, cpu, two, N, store, CRd, addrReg, addBefore, addAfter, option);
}

bool twoReg0(struct ArmCpu *cpu, void *userData, bool MRRC, uint8_t op, uint8_t Rd, uint8_t Rn, uint8_t CRm) {
    return twoReg((Pxa270wmmx *)userData, false, cpu, MRRC, op, Rd, Rn, CRm);
}

bool twoReg1(struct ArmCpu *cpu, void *userData, bool MRRC, uint8_t op, uint8_t Rd, uint8_t Rn, uint8_t CRm) {
    return twoReg((Pxa270wmmx *)userData, true, cpu, MRRC, op, Rd, Rn, CRm);
}

}  // namespace

bool pxa270wmmxCdp(Pxa270wmmx *w, bool cp1, unsigned op1, unsigned CRd, unsigned CRn, unsigned CRm, unsigned op2) {
    return cp1 ? cdp1(w, op1, op2, CRd, CRn, CRm) : cdp0(w, op1, op2, CRd, CRn, CRm);
}

bool pxa270wmmxMcr(Pxa270wmmx *w, bool cp1, unsigned op1, unsigned CRn, unsigned CRm, unsigned op2, uint32_t value) {
    return op1 != 1 && mcr(w, cp1, op1, CRn, CRm, op2, value);
}

bool pxa270wmmxMrc(Pxa270wmmx *w, bool cp1, unsigned op1, unsigned CRn, unsigned CRm, unsigned op2, uint32_t *value) {
    return mrc(w, cp1, op1, CRn, CRm, op2, *value);
}

bool pxa270wmmxMcrr(Pxa270wmmx *w, bool cp1, unsigned op, unsigned wR, uint32_t lo, uint32_t hi) {
    return mcrr(w, cp1, op, wR, lo, hi);
}

bool pxa270wmmxMrrc(Pxa270wmmx *w, bool cp1, unsigned op, unsigned wR, uint64_t *value) {
    return mrrc(w, cp1, op, wR, *value);
}

bool pxa270wmmxTransfer(Pxa270wmmx *w, struct ArmCpu *cpu, bool cp1, bool N, bool store, unsigned wR, uint32_t ea,
                        Pxa270wmmxFault *fault) {
    return transfer(w, cpu, cp1, false, N, store, wR, ea, fault);
}

// Whether a false return depends only on the encoding (never on register values), so
// running the instruction on scratch state answers for any state.
bool pxa270wmmxJitValid(uint32_t i) {
    const uint32_t cp = (i >> 8) & 15;
    if ((i >> 28) == 15 || cp > 1) return false;
    const bool cp1 = cp;
    Pxa270wmmx scratch;
    memset(&scratch, 0, sizeof(scratch));
    if ((i & 0x0f000010) == 0x0e000000)  // CDP
        return pxa270wmmxCdp(&scratch, cp1, (i >> 20) & 15, (i >> 12) & 15, (i >> 16) & 15, i & 15, (i >> 5) & 7);
    if ((i & 0x0f000010) == 0x0e000010) {  // MCR, MRC
        const unsigned op1 = (i >> 21) & 7, CRn = (i >> 16) & 15, CRm = i & 15, op2 = (i >> 5) & 7;
        if (i & (1 << 20)) {
            uint32_t v;
            return pxa270wmmxMrc(&scratch, cp1, op1, CRn, CRm, op2, &v);
        }
        return ((i >> 12) & 15) != 15 && pxa270wmmxMcr(&scratch, cp1, op1, CRn, CRm, op2, 0);
    }
    if ((i & 0x0fe00000) == 0x0c400000) {  // MCRR, MRRC
        const uint32_t rt = (i >> 12) & 15, rt2 = (i >> 16) & 15;
        if (rt == 15 || rt2 == 15 || rt == rt2) return false;
        if (i & (1 << 20)) {
            uint64_t v;
            return pxa270wmmxMrrc(&scratch, cp1, (i >> 4) & 15, i & 15, &v);
        }
        return pxa270wmmxMcrr(&scratch, cp1, (i >> 4) & 15, i & 15, 0, 0);
    }
    // LDC, STC: pre-indexed with or without writeback, or post-indexed (P or W set; with
    // both clear it is the option form or MCRR), base not r15. Every size is defined.
    if ((i & 0x0e000000) == 0x0c000000) return (i & 0x01200000) != 0 && ((i >> 16) & 15) != 15;
    return false;
}

struct Pxa270wmmx *pxa270wmmxInit(struct ArmCpu *cpu) {
    struct Pxa270wmmx *wmmx = (struct Pxa270wmmx *)malloc(sizeof(*wmmx));
    if (!wmmx) ERR("cannot alloc WMMX");
    memset(wmmx, 0, sizeof(*wmmx));

    struct ArmCoprocessor cp0 = {
        .regXfer = regXfer0,
        .dataProcessing = dataProcessing0,
        .memAccess = memAccess0,
        .twoRegF = twoReg0,
        .userData = wmmx,
    };
    struct ArmCoprocessor cp1 = {
        .regXfer = regXfer1,
        .dataProcessing = dataProcessing1,
        .memAccess = memAccess1,
        .twoRegF = twoReg1,
        .userData = wmmx,
    };
    cpuCoprocessorRegister(cpu, 0, &cp0);
    cpuCoprocessorRegister(cpu, 1, &cp1);
    cpuSetWmmx(cpu, wmmx);

    return wmmx;
}

template <typename T>
void pxa270wmmxSave(Pxa270wmmx *state, T &savestate) {
    auto chunk = savestate.GetChunk(ChunkType::pxa270wmmx, 0);
    if (!chunk) ERR("cannot allocate pxa270wmmx state");
    SaveChunkHelper helper(*chunk);
    state->DoSaveLoad(helper);
}

void pxa270wmmxLoad(Pxa270wmmx *state, SavestateLoader<ChunkType> &loader) {
    auto chunk = loader.GetChunkOrFail(ChunkType::pxa270wmmx, 0, "pxa270wmmx");
    if (!chunk) return;
    LoadChunkHelper helper(*chunk);
    state->DoSaveLoad(helper);
}

template void pxa270wmmxSave<Savestate<ChunkType>>(Pxa270wmmx *, Savestate<ChunkType> &);
template void pxa270wmmxSave<SavestateProbe<ChunkType>>(Pxa270wmmx *, SavestateProbe<ChunkType> &);
