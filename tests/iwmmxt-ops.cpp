// The iWMMXt instructions the Zire 72 Camera's MPEG-4 video encoder runs, each checked on
// random and edge inputs against a plain reference: logic, averages, align, compare, pack,
// unpack, sum of differences, multiplies and multiply-accumulates, add and subtract with
// each saturation, shuffle, and the core-register transfers (TMCR, TBCST, TINSR, TEXTRM,
// TMOVMSK, TANDC, TORC, TEXTRC, TMIA). Results, core registers and the N and Z flags in wCASF where an
// instruction sets them are compared. Shifts are in iwmmxt-shift.cpp.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include "CPU.h"
#include <cstring>
#include "patch_dispatch.h"
#include "pxa270_WMMX.h"
#include "system_state.h"

// 16 KiB of RAM at 0xa0000000: the instruction at the start, data from 0xa0001000.
static std::vector<uint8_t> ram(0x4000);
static bool ramAccess(void*, uint32_t addr, uint_fast8_t size, bool write, void* buf) {
    if (addr < 0xa0000000u || uint64_t(addr) + size > 0xa0000000ull + ram.size()) return false;
    if (write) std::memcpy(ram.data() + addr - 0xa0000000u, buf, size);
    else std::memcpy(buf, ram.data() + addr - 0xa0000000u, size);
    return true;
}
// A PXA270 core with the iWMMXt coprocessors and nothing else.
static ArmCpu* makeCpu() {
    auto mem = memInit();
    assert(memRegionAdd(mem, 0xa0000000u, ram.size(), ramAccess, nullptr));
    static PacePatch pace{};
    auto pd = initPatchDispatch();
    auto* c = cpuInit(0xa0000000u, mem, ARM_MEMORY_SYSTEM_MMU, true, false, -1, 0x69054114, 0x0b16a16a, pd, &pace,
                      createSystemState());
    patchDispatchSetCpu(pd, c);
    pxa270wmmxInit(c);
    return c;
}

static ArmCpu *cpu;
static void write32(uint32_t addr, uint32_t value) { assert(cpuMemOpExternal(cpu, &value, addr, 4, true)); }
static uint32_t read32(uint32_t addr) {
    uint32_t value = 0;
    assert(cpuMemOpExternal(cpu, &value, addr, 4, false));
    return value;
}
static bool step(uint32_t instruction) {
    write32(0xa0000000, instruction);
    cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu, ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC);
    cpuSetReg(cpu, 15, 0xa0000000);
    cpuSetReg(cpu, 0, 0xa0001000);
    cpuCyclePure(cpu);
    return cpuGetRegisters(cpu)[15] == 0xa0000004;  // an undefined instruction traps instead
}
static void must(uint32_t instruction) { assert(step(instruction)); }
static void loadWmmx(unsigned reg, uint64_t value) {
    write32(0xa0001000, uint32_t(value));
    write32(0xa0001004, uint32_t(value >> 32));
    must(0xedd00100 | (reg << 12));  // LDCL p1,cN,[r0]
}
static uint64_t readWmmx(unsigned reg) {
    must(0xedc00100 | (reg << 12));  // STCL p1,cN,[r0]
    return uint64_t(read32(0xa0001000)) | (uint64_t(read32(0xa0001004)) << 32);
}
static void writeControl(unsigned reg, uint32_t value) {
    write32(0xa0001000, value);
    must(0xfd900100 | (reg << 12));  // LDC2 p1,cN,[r0]
}
static uint32_t readControl(unsigned reg) {
    must(0xfd800100 | (reg << 12));  // STC2 p1,cN,[r0]
    return read32(0xa0001000);
}

static uint64_t seed = 0x9e3779b97f4a7c15ull;
static uint64_t rnd() {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}
// Random values, with some lanes forced to edges so saturation and signs are reached.
static uint64_t value() {
    static const uint64_t edges[] = {0, ~0ull, 0x8000800080008000ull, 0x7fff7fff7fff7fffull, 0x8080808080808080ull,
                                     0x7f7f7f7f7f7f7f7full, 0x0000000100000001ull, 0x80000000ffffffffull};
    const uint64_t r = rnd();
    switch (r & 7) {
        case 0: return edges[(r >> 3) & 7];
        case 1: return rnd() & 0x00ff00ff00ff00ffull;
        default: return rnd();
    }
}

static uint64_t lane(uint64_t v, unsigned bits, unsigned i) {
    return bits == 64 ? v : (v >> (i * bits)) & ((1ull << bits) - 1);
}
static int64_t slane(uint64_t v, unsigned bits, unsigned i) {
    const uint64_t u = lane(v, bits, i);
    return bits == 64 ? int64_t(u) : int64_t(u << (64 - bits)) >> (64 - bits);
}
static uint64_t put(uint64_t v, unsigned bits, unsigned i, uint64_t x) {
    if (bits == 64) return x;
    const uint64_t m = ((1ull << bits) - 1) << (i * bits);
    return (v & ~m) | ((x << (i * bits)) & m);
}
static int64_t clampS(int64_t x, unsigned bits) {
    const int64_t hi = (int64_t(1) << (bits - 1)) - 1, lo = -hi - 1;
    return x > hi ? hi : x < lo ? lo : x;
}
static int64_t clampU(int64_t x, unsigned bits) {
    const int64_t hi = (int64_t(1) << bits) - 1;
    return x > hi ? hi : x < 0 ? 0 : x;
}
// N and Z for each lane in wCASF: byte lane i at bits 4i+3/4i+2, halfword 8i+7/8i+6,
// word 16i+15/16i+14, doubleword 31/30.
static uint32_t nz(uint64_t v, unsigned bits) {
    uint32_t f = 0;
    const unsigned n = 64 / bits, step = 32 / n;
    for (unsigned i = 0; i < n; ++i) {
        const uint64_t e = lane(v, bits, i);
        if (e >> (bits - 1) & 1) f |= 1u << (step * i + step - 1);
        if (!e) f |= 1u << (step * i + step - 2);
    }
    return f;
}

struct In { uint64_t n, m, d; uint32_t cgr[4], rs, rm; };
struct Out { uint64_t d; bool flags; uint32_t casf; };
static const unsigned sizes[] = {8, 16, 32, 64};

static uint32_t cdp(unsigned cp, unsigned op1, unsigned op2, unsigned crd, unsigned crn, unsigned crm) {
    return 0xee000000u | (op1 << 20) | (crn << 16) | (crd << 12) | (cp << 8) | (op2 << 5) | crm;
}

struct Case { std::string name; unsigned cp, op1, op2; std::function<Out(const In &)> ref; unsigned crm = 2; };

static std::vector<Case> cases() {
    std::vector<Case> c;
    // Logic, cp0 op2 000: op1 0 WOR, 1 WXOR, 2 WAND, 3 WANDN (wRn & ~wRm). Flags from the 64 bits.
    const char *logic[] = {"wor", "wxor", "wand", "wandn"};
    for (unsigned op = 0; op < 4; ++op)
        c.push_back({logic[op], 0, op, 0, [op](const In &x) {
            const uint64_t r = op == 0 ? x.n | x.m : op == 1 ? x.n ^ x.m : op == 2 ? x.n & x.m : x.n & ~x.m;
            return Out{r, true, nz(r, 64)};
        }});
    // WAVG2, cp0 op2 000, op1 1 H 0 R: unsigned (a + b + R) / 2.
    for (unsigned h : {0u, 1u})
        for (unsigned r : {0u, 1u})
            c.push_back({std::string("wavg2") + (h ? "h" : "b") + (r ? "r" : ""), 0, 8 | (h << 2) | r, 0, [h, r](const In &x) {
                const unsigned bits = h ? 16 : 8;
                uint64_t v = 0;
                for (unsigned i = 0; i < 64 / bits; ++i) v = put(v, bits, i, (lane(x.n, bits, i) + lane(x.m, bits, i) + r) >> 1);
                return Out{v, false, 0};
            }});
    // WALIGNI, cp0 op2 001, op1 = byte offset 0-7: (wRm:wRn) >> 8 * offset.
    for (unsigned k = 0; k < 8; ++k)
        c.push_back({"waligni" + std::to_string(k), 0, k, 1, [k](const In &x) {
            return Out{k ? (x.n >> (8 * k)) | (x.m << (64 - 8 * k)) : x.n, false, 0};
        }});
    // WALIGNR0-3, cp0 op2 001, op1 10gg: the offset is wCGRg bits 2:0.
    for (unsigned g = 0; g < 4; ++g)
        c.push_back({"walignr" + std::to_string(g), 0, 8 | g, 1, [g](const In &x) {
            const unsigned k = x.cgr[g] & 7;
            return Out{k ? (x.n >> (8 * k)) | (x.m << (64 - 8 * k)) : x.n, false, 0};
        }});
    // WCMPEQ/WCMPGT, cp0 op2 011, op1 ss S G: G 1 is greater-than, S signed.
    for (unsigned s = 0; s < 3; ++s)
        for (unsigned kind = 0; kind < 3; ++kind) {
            const unsigned op1 = (s << 2) | (kind == 2 ? 2 : 0) | (kind ? 1 : 0);
            const char *names[] = {"wcmpeq", "wcmpgtu", "wcmpgts"};
            c.push_back({std::string(names[kind]) + "bhw"[s], 0, op1, 3, [s, kind](const In &x) {
                const unsigned bits = sizes[s];
                uint64_t v = 0;
                for (unsigned i = 0; i < 64 / bits; ++i) {
                    const bool t = kind == 0 ? lane(x.n, bits, i) == lane(x.m, bits, i)
                                 : kind == 1 ? lane(x.n, bits, i) > lane(x.m, bits, i)
                                             : slane(x.n, bits, i) > slane(x.m, bits, i);
                    v = put(v, bits, i, t ? ~0ull : 0);
                }
                return Out{v, true, nz(v, bits)};
            }});
        }
    // WPACK, cp0 op2 100, op1 ss S 1: source size ss (01 h, 10 w, 11 d) to half that, signed
    // source saturated to signed (S) or unsigned; wRn gives the low half, wRm the high half.
    for (unsigned s = 1; s < 4; ++s)
        for (unsigned sg : {0u, 1u})
            c.push_back({std::string("wpack") + "-hwd"[s] + (sg ? "ss" : "us"), 0, (s << 2) | (sg << 1) | 1, 4, [s, sg](const In &x) {
                const unsigned bits = sizes[s], half = bits / 2, n = 64 / bits;
                uint64_t v = 0;
                for (unsigned i = 0; i < n; ++i) {
                    const int64_t a = slane(x.n, bits, i), b = slane(x.m, bits, i);
                    v = put(v, half, i, uint64_t(sg ? clampS(a, half) : clampU(a, half)));
                    v = put(v, half, i + n, uint64_t(sg ? clampS(b, half) : clampU(b, half)));
                }
                return Out{v, false, 0};
            }});
    // WUNPCKEH/IH (op2 110) and WUNPCKEL/IL (op2 111), cp0, op1 ss S I: I interleaves wRn and
    // wRm lanes from the high or low half, else the half of wRn is zero- or sign-extended (S).
    for (unsigned low : {0u, 1u})
        for (unsigned s = 0; s < 3; ++s)
            for (unsigned kind = 0; kind < 3; ++kind) {
                const unsigned op1 = (s << 2) | (kind == 2 ? 2 : 0) | (kind == 0 ? 1 : 0);
                const char *names[] = {"i", "eu", "es"};
                c.push_back({std::string("wunpck") + (low ? "l" : "h") + names[kind] + "bhw"[s], 0, op1, low ? 7u : 6u,
                             [low, s, kind](const In &x) {
                    const unsigned bits = sizes[s], n = 32 / bits, base = low ? 0 : n;
                    uint64_t v = 0;
                    if (kind == 0) {
                        for (unsigned i = 0; i < n; ++i) {
                            v = put(v, bits, 2 * i, lane(x.n, bits, base + i));
                            v = put(v, bits, 2 * i + 1, lane(x.m, bits, base + i));
                        }
                        return Out{v, true, nz(v, bits)};
                    }
                    for (unsigned i = 0; i < n; ++i)
                        v = put(v, 2 * bits, i, kind == 1 ? lane(x.n, bits, base + i) : uint64_t(slane(x.n, bits, base + i)));
                    return Out{v, true, nz(v, 2 * bits)};
                }});
            }
    // WMUL, cp1 op2 000, op1 00 S M: 16-bit lanes, low (M 0) or high half of the product.
    for (unsigned sg : {0u, 1u})
        for (unsigned m : {0u, 1u})
            c.push_back({std::string("wmul") + (sg ? "s" : "u") + (m ? "m" : "l"), 1, (sg << 1) | m, 0, [sg, m](const In &x) {
                uint64_t v = 0;
                for (unsigned i = 0; i < 4; ++i) {
                    const int64_t p = sg ? slane(x.n, 16, i) * slane(x.m, 16, i) : int64_t(lane(x.n, 16, i) * lane(x.m, 16, i));
                    v = put(v, 16, i, uint64_t(p) >> (m ? 16 : 0));
                }
                return Out{v, false, 0};
            }});
    // WMAC, cp1 op2 000, op1 01 S Z: wRd (or 0 with Z) plus the four 16-bit products.
    for (unsigned sg : {0u, 1u})
        for (unsigned z : {0u, 1u})
            c.push_back({std::string("wmac") + (sg ? "s" : "u") + (z ? "z" : ""), 1, 4 | (sg << 1) | z, 0, [sg, z](const In &x) {
                uint64_t v = z ? 0 : x.d;
                for (unsigned i = 0; i < 4; ++i)
                    v += sg ? uint64_t(slane(x.n, 16, i) * slane(x.m, 16, i)) : lane(x.n, 16, i) * lane(x.m, 16, i);
                return Out{v, false, 0};
            }});
    // WMADD, cp1 op2 000, op1 10 S 0: each 32-bit lane is the sum of its two 16-bit products.
    for (unsigned sg : {0u, 1u})
        c.push_back({std::string("wmadd") + (sg ? "s" : "u"), 1, 8 | (sg << 1), 0, [sg](const In &x) {
            uint64_t v = 0;
            for (unsigned i = 0; i < 2; ++i) {
                const uint64_t p = sg ? uint64_t(slane(x.n, 16, 2 * i) * slane(x.m, 16, 2 * i) + slane(x.n, 16, 2 * i + 1) * slane(x.m, 16, 2 * i + 1))
                                      : lane(x.n, 16, 2 * i) * lane(x.m, 16, 2 * i) + lane(x.n, 16, 2 * i + 1) * lane(x.m, 16, 2 * i + 1);
                v = put(v, 32, i, p);
            }
            return Out{v, false, 0};
        }});
    // WSAD, cp1 op2 001, op1 0 H 0 Z: sum of absolute differences into wRd's low word (or
    // onto it without Z); the high word is cleared.
    for (unsigned h : {0u, 1u})
        for (unsigned z : {0u, 1u})
            c.push_back({std::string("wsad") + (h ? "h" : "b") + (z ? "z" : ""), 1, (h << 2) | z, 1, [h, z](const In &x) {
                const unsigned bits = h ? 16 : 8;
                uint32_t sum = z ? 0 : uint32_t(x.d);
                for (unsigned i = 0; i < 64 / bits; ++i) {
                    const int64_t d = int64_t(lane(x.n, bits, i)) - int64_t(lane(x.m, bits, i));
                    sum += uint32_t(d < 0 ? -d : d);
                }
                return Out{sum, false, 0};
            }});
    // WMAX/WMIN, cp1 op2 011, op1 ss S N: N 1 is the minimum, S signed.
    for (unsigned s = 0; s < 3; ++s)
        for (unsigned sg : {0u, 1u})
            for (unsigned mn : {0u, 1u})
                c.push_back({std::string(mn ? "wmin" : "wmax") + (sg ? "s" : "u") + "bhw"[s], 1, (s << 2) | (sg << 1) | mn, 3,
                             [s, sg, mn](const In &x) {
                    const unsigned bits = sizes[s];
                    uint64_t v = 0;
                    for (unsigned i = 0; i < 64 / bits; ++i) {
                        const bool nBigger = sg ? slane(x.n, bits, i) > slane(x.m, bits, i) : lane(x.n, bits, i) > lane(x.m, bits, i);
                        v = put(v, bits, i, (nBigger != bool(mn)) ? lane(x.n, bits, i) : lane(x.m, bits, i));
                    }
                    return Out{v, false, 0};
                }});
    // WADD (op2 100) and WSUB (op2 101), cp1, op1 ss tt: tt 00 modulo, 01 unsigned and 11
    // signed saturation.
    for (unsigned sub : {0u, 1u})
        for (unsigned s = 0; s < 3; ++s)
            for (unsigned t : {0u, 1u, 3u})
                c.push_back({std::string(sub ? "wsub" : "wadd") + "bhw"[s] + (t == 0 ? "" : t == 1 ? "us" : "ss"), 1, (s << 2) | t,
                             sub ? 5u : 4u, [sub, s, t](const In &x) {
                    const unsigned bits = sizes[s];
                    uint64_t v = 0;
                    for (unsigned i = 0; i < 64 / bits; ++i) {
                        int64_t r;
                        if (t == 3) r = clampS(sub ? slane(x.n, bits, i) - slane(x.m, bits, i) : slane(x.n, bits, i) + slane(x.m, bits, i), bits);
                        else {
                            r = sub ? int64_t(lane(x.n, bits, i)) - int64_t(lane(x.m, bits, i)) : int64_t(lane(x.n, bits, i) + lane(x.m, bits, i));
                            if (t == 1) r = clampU(r, bits);
                        }
                        v = put(v, bits, i, uint64_t(r));
                    }
                    return Out{v, true, nz(v, bits)};
                }});
    // WSHUFH, cp1 op2 111: the 8-bit immediate is op1:CRm, two bits per destination halfword.
    for (unsigned imm : {0x00u, 0x1bu, 0x55u, 0x72u, 0x87u, 0xaau, 0xd2u, 0xe4u, 0xffu})
        c.push_back({"wshufh" + std::to_string(imm), 1, imm >> 4, 7, [imm](const In &x) {
            uint64_t v = 0;
            for (unsigned i = 0; i < 4; ++i) v = put(v, 16, i, lane(x.n, 16, (imm >> (2 * i)) & 3));
            return Out{v, true, nz(v, 16)};
        }, imm & 15});
    return c;
}

int main() {
    cpu = makeCpu();
    cpuSetCPAR(cpu, 3);
    unsigned checked = 0, failed = 0;

    // Register layouts: separate registers, then the destination also as the first and as
    // the second source.
    struct Layout { unsigned d, n, m; };
    const Layout layouts[] = {{3, 1, 2}, {1, 1, 2}, {2, 1, 2}};
    for (const Case &k : cases())
        for (const Layout &l : layouts)
            for (unsigned trial = 0; trial < 300; ++trial) {
                In x{value(), value(), value(), {uint32_t(rnd()), uint32_t(rnd()), uint32_t(rnd()), uint32_t(rnd())}, 0, 0};
                if (l.d == l.n) x.d = x.n;
                if (l.d == l.m) x.d = x.m;
                // WUNPCKE* take no wRm; their CRm field is 0, as binutils encodes them.
                const bool noM = k.name.rfind("wunpck", 0) == 0 && k.name[7] == 'e';
                const unsigned crm = k.op2 == 7 && k.cp == 1 ? k.crm : noM ? 0 : l.m;
                if (k.op2 == 7 && k.cp == 1 && l.d == l.m) continue;  // WSHUFH has no wRm
                for (unsigned g = 0; g < 4; ++g) writeControl(8 + g, x.cgr[g]);
                loadWmmx(l.n, x.n);
                if (l.m != l.n) loadWmmx(l.m, x.m);
                if (l.d != l.n && l.d != l.m) loadWmmx(l.d, x.d);
                writeControl(3, 0x5a5a5a5a);
                In y = x;
                if (k.op2 == 7 && k.cp == 1) y.m = 0;
                const Out want = k.ref(y);
                const bool ran = step(cdp(k.cp, k.op1, k.op2, l.d, l.n, crm));
                const uint64_t got = ran ? readWmmx(l.d) : 0;
                const uint32_t casf = readControl(3);
                // Only N and Z are checked; C and V are left to each instruction.
                if (!ran || got != want.d || (want.flags && (casf & 0xccccccccu) != want.casf)) {
                    if (failed++ < 40)
                        printf("FAIL %s d%u n%u m%u: n %016llx m %016llx d %016llx: got %s%016llx casf %08x, want %016llx casf %08x\n",
                               k.name.c_str(), l.d, l.n, l.m, (unsigned long long)x.n, (unsigned long long)x.m,
                               (unsigned long long)x.d, ran ? "" : "UNDEFINED ", (unsigned long long)got, casf,
                               (unsigned long long)want.d, want.flags ? want.casf : 0);
                    break;
                }
                ++checked;
            }

    // Core register transfers.
    auto check = [&](bool ok, const char *what) {
        if (!ok) { ++failed; printf("FAIL %s\n", what); }
        else ++checked;
    };
    for (unsigned trial = 0; trial < 200; ++trial) {
        const uint32_t r = uint32_t(rnd()), r2 = uint32_t(rnd());
        const uint64_t w = value(), acc = value();
        // TMCR wCGR1, r4 (MCR p1, 0, r4, c9, c0) then TMRC r5, wCGR1.
        cpuSetReg(cpu, 4, r);
        must(0xee000110 | (9u << 16) | (4u << 12));
        check(readControl(9) == r, "tmcr wcgr1");
        cpuSetReg(cpu, 5, 0);
        must(0xee100110 | (9u << 16) | (5u << 12));
        check(cpuGetRegExternal(cpu, 5) == r, "tmrc wcgr1");
        // TBCSTB/H/W wR6, r4: MCR p0, 010, r4, c6, c0, size in op2 bits 2:1.
        for (unsigned s = 0; s < 3; ++s) {
            must(0xee400010 | (6u << 16) | (4u << 12) | (s << 6));
            const unsigned bits = sizes[s];
            uint64_t v = 0;
            for (unsigned i = 0; i < 64 / bits; ++i) v = put(v, bits, i, r);
            check(readWmmx(6) == v, "tbcst");
        }
        // TINSRB/H/W wR6, r4, #i: MCR p0, 011, index in CRm bits 2:0.
        for (unsigned s = 0; s < 3; ++s)
            for (unsigned i = 0; i < 64 / sizes[s]; ++i) {
                loadWmmx(6, w);
                must(0xee600010 | (6u << 16) | (4u << 12) | (s << 6) | i);
                check(readWmmx(6) == put(w, sizes[s], i, r), "tinsr");
            }
        // TEXTRM{U,S}{B,H,W} r5, wR6, #i: MRC p0, op1 ss0, op2 011, CRm bit 3 signed.
        loadWmmx(6, w);
        for (unsigned s = 0; s < 3; ++s)
            for (unsigned sg : {0u, 1u})
                for (unsigned i = 0; i < 64 / sizes[s]; ++i) {
                    must(0xee100070 | (s << 22) | (6u << 16) | (5u << 12) | (sg << 3) | i);
                    const uint32_t want = sg ? uint32_t(slane(w, sizes[s], i)) : uint32_t(lane(w, sizes[s], i));
                    check(cpuGetRegExternal(cpu, 5) == want, "textrm");
                }
        // TMOVMSKB/H/W r5, wR6: the top bit of each lane.
        for (unsigned s = 0; s < 3; ++s) {
            must(0xee100030 | (s << 22) | (6u << 16) | (5u << 12));
            uint32_t want = 0;
            for (unsigned i = 0; i < 64 / sizes[s]; ++i) want |= uint32_t(lane(w, sizes[s], i) >> (sizes[s] - 1)) << i;
            check(cpuGetRegExternal(cpu, 5) == want, "tmovmsk");
        }
        // TANDCB/H/W r15: CPSR NZCV is the AND of every lane's wCASF flags.
        for (unsigned s = 0; s < 3; ++s) {
            writeControl(3, r);
            must(0xee130130 | (s << 22) | (15u << 12));
            const unsigned step = 4u << s;
            uint32_t want = 0xf;
            for (unsigned i = 0; i < 32 / step; ++i) want &= (r >> (step * i + step - 4)) & 0xf;
            check((cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR) >> 28) == want, "tandc");
        }
        // TORCB/H/W r15: the OR of every lane's flags. TEXTRCB/H/W r15, #i: lane i's flags.
        for (unsigned s = 0; s < 3; ++s) {
            writeControl(3, r);
            must(0xee130150 | (s << 22) | (15u << 12));
            const unsigned step = 4u << s;
            uint32_t want = 0;
            for (unsigned i = 0; i < 32 / step; ++i) want |= (r >> (step * i + step - 4)) & 0xf;
            check((cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR) >> 28) == want, "torc");
            for (unsigned i = 0; i < 32 / step; ++i) {
                must(0xee130170 | (s << 22) | (15u << 12) | i);
                check((cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR) >> 28) == ((r >> (step * i + step - 4)) & 0xf), "textrc");
            }
        }
        // TMIA, TMIAPH and TMIAxy wR6, r4, r7: wR6 plus the signed product(s).
        cpuSetReg(cpu, 7, r2);
        const unsigned kinds[] = {0x0, 0x8, 0xc, 0xd, 0xe, 0xf};
        for (unsigned kind : kinds) {
            loadWmmx(6, acc);
            // MCR p0, 001: CRn picks the kind, bits 8:5 are wRd, bits 15:12 Rs, bits 3:0 Rm.
            must(0xee200010 | (kind << 16) | (4u << 12) | (6u << 5) | 7);
            int64_t sum;
            if (kind == 0) sum = int64_t(int32_t(r2)) * int32_t(r);
            else if (kind == 8) sum = int64_t(int16_t(r2)) * int16_t(r) + int64_t(int16_t(r2 >> 16)) * int16_t(r >> 16);
            else sum = int64_t(int16_t((kind & 2) ? r2 >> 16 : r2)) * int16_t((kind & 1) ? r >> 16 : r);
            check(readWmmx(6) == acc + uint64_t(sum), "tmia");
        }
    }
    // TANDC, TORC and TEXTRC with CRn other than 3 are undefined.
    for (unsigned crn = 0; crn < 16; ++crn) {
        if (crn == 3) continue;
        check(!step(0xee100130 | (crn << 16) | (15u << 12)), "tandc crn");
        check(!step(0xee100150 | (crn << 16) | (15u << 12)), "torc crn");
        check(!step(0xee100170 | (crn << 16) | (15u << 12)), "textrc crn");
    }
    // TMCR to wCSSF, wCASF or a wCGR sets wCon CUP (bit 0); TMCR to wCon writes it. Read with TMRC r5, wCon.
    for (unsigned crn : {2u, 3u, 8u, 11u}) {
        cpuSetReg(cpu, 4, 0);
        must(0xee010110 | (4u << 12));  // TMCR wCon, r4
        must(0xee110110 | (5u << 12));
        check(cpuGetRegExternal(cpu, 5) == 0, "tmcr wcon clears");
        cpuSetReg(cpu, 4, 0x12345678);
        must(0xee000110 | (crn << 16) | (4u << 12));
        must(0xee110110 | (5u << 12));
        check((cpuGetRegExternal(cpu, 5) & 1) == 1, "tmcr sets cup");
    }
    // WLDRW into wCSSF, wCASF or a wCGR sets CUP, into wCon writes it as given; WSTRW leaves CUP alone.
    for (unsigned crn : {2u, 3u, 8u, 11u}) {
        cpuSetReg(cpu, 4, 0);
        must(0xee010110 | (4u << 12));  // TMCR wCon, r4
        writeControl(crn, 0x12345678);
        must(0xee110110 | (5u << 12));
        check((cpuGetRegExternal(cpu, 5) & 1) == 1, "wldrw sets cup");
        must(0xee010110 | (4u << 12));
        readControl(crn);
        must(0xee110110 | (5u << 12));
        check(cpuGetRegExternal(cpu, 5) == 0, "wstrw leaves cup");
    }
    for (uint32_t v : {0u, 1u, 2u, 3u}) {
        writeControl(1, v);
        must(0xee110110 | (5u << 12));
        check(cpuGetRegExternal(cpu, 5) == v, "wldrw wcon as given");
    }
    // Loads and stores as the encoder writes them: WLDRD post-indexed (ldcl p1, c14, [r1], #8),
    // WSTRW post-indexed down (stc p1, c5, [r1], #-4) and WLDRD pre-indexed down without
    // writeback (ldcl p1, c0, [r1, #-128]). Offsets count words.
    for (unsigned trial = 0; trial < 50; ++trial) {
        const uint64_t w = value();
        write32(0xa0002000, uint32_t(w));
        write32(0xa0002004, uint32_t(w >> 32));
        cpuSetReg(cpu, 1, 0xa0002000);
        must(0xecf1e102);
        check(readWmmx(14) == w && cpuGetRegExternal(cpu, 1) == 0xa0002008, "wldrd post-indexed");
        loadWmmx(5, ~w);
        cpuSetReg(cpu, 1, 0xa0002010);
        must(0xec215101);
        check(read32(0xa0002010) == uint32_t(~w) && cpuGetRegExternal(cpu, 1) == 0xa000200c, "wstrw post-indexed down");
        cpuSetReg(cpu, 1, 0xa0002080);
        must(0xed510120);
        check(readWmmx(0) == w && cpuGetRegExternal(cpu, 1) == 0xa0002080, "wldrd pre-indexed down");
    }
    printf("iwmmxt ops: %u results checked, %u failed\n", checked, failed);
    return failed ? 1 : 0;
}
