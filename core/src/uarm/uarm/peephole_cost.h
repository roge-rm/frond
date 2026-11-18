#ifndef _PEEPHOLE_COST_H_
#define _PEEPHOLE_COST_H_

#include <cstddef>
#include <cstdint>

#include "xscale_cost.h"

// What a peephole's replaced ADS routine (peephole.cpp) would cost: the routine runs on a copy of the
// registers and flags, instruction by instruction, each charged from the cost table (1 when its condition
// fails), and every load and store it makes is reported in order. Only the forms these routines use are
// run: data processing with immediate or immediate-shift operands, B, BX, LDR/STR and LDRB/STRB with an
// immediate offset, LDM/STM. Anything else ends the run. Loads give 0: no routine branches on loaded data.

namespace cost {

namespace detail {

inline bool conditionPasses(uint32_t cond, uint32_t f) {
    const bool n = f >> 31, z = (f >> 30) & 1, c = (f >> 29) & 1, v = (f >> 28) & 1;
    switch (cond) {
        case 0: return z;
        case 1: return !z;
        case 2: return c;
        case 3: return !c;
        case 4: return n;
        case 5: return !n;
        case 6: return v;
        case 7: return !v;
        case 8: return c && !z;
        case 9: return !c || z;
        case 10: return n == v;
        case 11: return n != v;
        case 12: return !z && n == v;
        case 13: return z || n != v;
        default: return true;
    }
}

}  // namespace detail

// Runs words from index 0 with regs (r0-r15) and flags (NZCV in bits 31-28) until a BX, an unknown form
// or maxSteps instructions. charge(cost) is called before each instruction runs, then access(va, store)
// for each of its loads and stores. Returns the instructions run.
template <typename Charge, typename Access>
inline uint32_t runRoutine(const uint32_t* words, size_t count, uint32_t* regs, uint32_t& flags, Core core,
                           Charge&& charge, Access&& access, uint32_t maxSteps = 1u << 24) {
    size_t i = 0;
    uint32_t steps = 0;
    while (i < count && steps < maxSteps) {
        const uint32_t w = words[i];
        steps++;
        if (!detail::conditionPasses(w >> 28, flags)) {
            charge(conditionFailedCost(core));
            i++;
            continue;
        }
        charge(armCost(w, core));
        if ((w & 0x0ffffff0u) == 0x012fff10u) return steps;  // BX
        if ((w & 0x0e000000u) == 0x0a000000u) {               // B, BL
            int32_t off = int32_t(w << 8) >> 8;
            if (w & 0x01000000u) regs[14] = 0;
            i = size_t(int64_t(i) + 2 + off);
            continue;
        }
        if ((w & 0x0e000000u) == 0x08000000u) {  // LDM, STM
            const bool p = w & (1u << 24), u = w & (1u << 23), wb = w & (1u << 21), load = w & (1u << 20);
            const unsigned rn = (w >> 16) & 15, n = popcount16(w);
            uint32_t base = regs[rn];
            uint32_t start = u ? (p ? base + 4 : base) : (p ? base - 4 * n : base - 4 * n + 4);
            for (unsigned r = 0, k = 0; r < 16; r++)
                if (w & (1u << r)) {
                    access(start + 4 * k++, !load);
                    if (load) regs[r] = 0;
                }
            if (wb && !(load && (w & (1u << rn)))) regs[rn] = u ? base + 4 * n : base - 4 * n;
            i++;
            continue;
        }
        if ((w & 0x0e000000u) == 0x04000000u) {  // LDR, STR immediate
            const bool p = w & (1u << 24), u = w & (1u << 23), wb = w & (1u << 21), load = w & (1u << 20);
            const unsigned rn = (w >> 16) & 15, rd = (w >> 12) & 15;
            const uint32_t off = w & 0xfff, base = regs[rn];
            const uint32_t moved = u ? base + off : base - off;
            access(p ? moved : base, !load);
            if (!p || wb) regs[rn] = moved;
            if (load) regs[rd] = 0;
            i++;
            continue;
        }
        if ((w & 0x0c000000u) != 0 || (!(w & (1u << 25)) && (w & 0x90u) == 0x90u) || (w & 0x10u && !(w & (1u << 25))))
            return steps;  // not plain data processing with an immediate or immediate shift
        // Data processing.
        const bool c = (flags >> 29) & 1;
        uint32_t op2;
        bool sc = c;
        if (w & (1u << 25)) {
            const unsigned rot = ((w >> 8) & 15) * 2;
            op2 = rot ? ((w & 0xff) >> rot) | ((w & 0xff) << (32 - rot)) : (w & 0xff);
            if (rot) sc = op2 >> 31;
        } else {
            const uint32_t rm = regs[w & 15];
            const unsigned type = (w >> 5) & 3, amt = (w >> 7) & 31;
            switch (type) {
                case 0:
                    op2 = amt ? rm << amt : rm;
                    if (amt) sc = (rm >> (32 - amt)) & 1;
                    break;
                case 1:
                    op2 = amt ? rm >> amt : 0;
                    sc = amt ? (rm >> (amt - 1)) & 1 : rm >> 31;
                    break;
                case 2:
                    op2 = amt ? uint32_t(int32_t(rm) >> amt) : uint32_t(int32_t(rm) >> 31);
                    sc = amt ? (rm >> (amt - 1)) & 1 : rm >> 31;
                    break;
                default:
                    op2 = amt ? (rm >> amt) | (rm << (32 - amt)) : (rm >> 1) | (uint32_t(c) << 31);
                    sc = amt ? (rm >> (amt - 1)) & 1 : rm & 1;
                    break;
            }
        }
        const unsigned opc = (w >> 21) & 15, rn = (w >> 16) & 15, rd = (w >> 12) & 15;
        const bool s = w & (1u << 20);
        const uint32_t a = regs[rn];
        uint64_t wide = 0;
        uint32_t res = 0;
        bool arith = false, nc = sc, nv = (flags >> 28) & 1;
        auto add = [&](uint32_t x, uint32_t y, uint32_t cin) {
            wide = uint64_t(x) + y + cin;
            res = uint32_t(wide);
            nc = wide >> 32;
            nv = ((~(x ^ y) & (x ^ res)) >> 31) & 1;
            arith = true;
        };
        switch (opc) {
            case 0: case 8: res = a & op2; break;     // AND, TST
            case 1: case 9: res = a ^ op2; break;     // EOR, TEQ
            case 2: case 10: add(a, ~op2, 1); break;  // SUB, CMP
            case 3: add(op2, ~a, 1); break;           // RSB
            case 4: case 11: add(a, op2, 0); break;   // ADD, CMN
            case 5: add(a, op2, c); break;            // ADC
            case 6: add(a, ~op2, c); break;           // SBC
            case 7: add(op2, ~a, c); break;           // RSC
            case 12: res = a | op2; break;            // ORR
            case 13: res = op2; break;                // MOV
            case 14: res = a & ~op2; break;           // BIC
            default: res = ~op2; break;               // MVN
        }
        (void)arith;
        if (opc < 8 || opc > 11) {
            if (rd == 15) return steps;
            regs[rd] = res;
        }
        if (s) flags = (res & 0x80000000u) | (res ? 0 : 0x40000000u) | (nc ? 0x20000000u : 0) | (nv ? 0x10000000u : 0);
        i++;
    }
    return steps;
}

}  // namespace cost

#endif
