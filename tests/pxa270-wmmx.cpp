// Runs each iWMMXt instruction the PXA27x has through the interpreter and checks the result
// against a plain per-lane model. The encodings come from GNU binutils (arm-palmos-as
// -mcpu=iwmmxt); every one uses wR1 as the destination, wR2 and wR3 (or r2 and r3) as sources.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "CPU.h"
#include "patch_dispatch.h"
#include "pxa270_WMMX.h"
#include "system_state.h"

// Like assert, but kept with NDEBUG since the argument does the work.
#define must(x) do { if (!(x)) { fprintf(stderr, "FAIL: %s\n", #x); exit(1); } } while (0)

static constexpr uint32_t base = 0x10000;
static std::vector<uint8_t> memory(0x4000);
static bool access(void*, uint32_t addr, uint_fast8_t size, bool write, void* buf) {
    if (addr < base || uint64_t(addr) + size > base + memory.size()) return false;
    if (write) std::memcpy(memory.data() + addr - base, buf, size);
    else std::memcpy(buf, memory.data() + addr - base, size);
    return true;
}
static void put(uint32_t addr, uint32_t value) { must(access(nullptr, addr, 4, true, &value)); }

static ArmCpu* cpu;
static int failures;

// Runs one instruction; false if it took the undefined instruction vector.
static bool run(uint32_t instr) {
    put(base, instr);
    cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu, ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC);
    cpuSetReg(cpu, 15, base);
    cpuCyclePure(cpu);
    return cpuGetRegisters(cpu)[15] == base + 4;
}
static uint32_t reg(unsigned r) { return cpuGetRegExternal(cpu, r); }
static void setW(unsigned n, uint64_t v) {
    cpuSetReg(cpu, 0, uint32_t(v)); cpuSetReg(cpu, 1, uint32_t(v >> 32));
    must(run(0xec410000 | n));  // TMCRR wRn, r0, r1
}
static uint64_t getW(unsigned n) {
    must(run(0xec510000 | n));  // TMRRC r0, r1, wRn
    return uint64_t(reg(0)) | uint64_t(reg(1)) << 32;
}
// TMCR and TMRC through r12, which the cases below don't use.
static void setC(unsigned c, uint32_t v) {
    cpuSetReg(cpu, 12, v);
    if (!run(0xee00c110 | c << 16)) { fprintf(stderr, "FAIL: tmcr\n"); exit(1); }
}
static uint32_t getC(unsigned c) {
    if (!run(0xee10c110 | c << 16)) { fprintf(stderr, "FAIL: tmrc\n"); exit(1); }
    return reg(12);
}

static void check(bool ok, const std::string& what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what.c_str()); failures++; }
}

// Lanes, lane 0 in the low bits.
static uint64_t ul(uint64_t v, unsigned bits, unsigned i) {
    return bits == 64 ? v : (v >> (i * bits)) & ((1ull << bits) - 1);
}
static int64_t sl(uint64_t v, unsigned bits, unsigned i) {
    uint64_t x = ul(v, bits, i);
    return bits < 64 && (x >> (bits - 1)) ? int64_t(x | ~((1ull << bits) - 1)) : int64_t(x);
}
static uint64_t build(unsigned bits, const std::function<uint64_t(unsigned)>& f) {
    uint64_t r = 0;
    for (unsigned i = 0; i < 64 / bits; i++)
        r |= (f(i) & (bits == 64 ? ~0ull : (1ull << bits) - 1)) << (i * bits);
    return r;
}
static int64_t clamp(int64_t x, int64_t lo, int64_t hi) { return x < lo ? lo : x > hi ? hi : x; }
static int64_t smin(unsigned b) { return -(int64_t(1) << (b - 1)); }
static int64_t smax(unsigned b) { return (int64_t(1) << (b - 1)) - 1; }
static int64_t umax(unsigned b) { return int64_t((1ull << b) - 1); }

struct Op { uint32_t enc; std::string name; std::function<uint64_t(uint64_t a, uint64_t b, uint64_t d)> f; };

int main() {
    auto mem = memInit(); must(memRegionAdd(mem, base, memory.size(), access, nullptr));
    PacePatch pace{}; auto pd = initPatchDispatch();
    cpu = cpuInit(base, mem, ARM_MEMORY_SYSTEM_MMU, true, false, -1, 0x69054114, 0x0b16a16a, pd, &pace,
                  createSystemState());
    patchDispatchSetCpu(pd, cpu);
    pxa270wmmxInit(cpu);

    // Coprocessors 0 and 1 are refused until CPAR lets them in.
    check(!run(0xee412050), "TBCSTH with CPAR clear is undefined");
    cpuSetCPAR(cpu, 3);

    const uint64_t A[] = {0x8001'7fff'00ff'ff80ull, 0x0123'4567'89ab'cdefull, 0xffff'ffff'ffff'ffffull,
                          0x0000'0000'0000'0000ull, 0x7f80'0180'fe02'4000ull};
    const uint64_t B[] = {0x7fff'8001'ff01'0080ull, 0xfedc'ba98'7654'3210ull, 0x0000'0000'0000'0001ull,
                          0x8000'0000'8000'0000ull, 0x0280'7f7f'0101'c000ull};
    const uint64_t D = 0x1111'2222'3333'4444ull;

    std::vector<Op> ops = {
        {0xee021003, "wor", [](uint64_t a, uint64_t b, uint64_t) { return a | b; }},
        {0xee121003, "wxor", [](uint64_t a, uint64_t b, uint64_t) { return a ^ b; }},
        {0xee221003, "wand", [](uint64_t a, uint64_t b, uint64_t) { return a & b; }},
        {0xee321003, "wandn", [](uint64_t a, uint64_t b, uint64_t) { return a & ~b; }},
        {0xee821003, "wavg2b", [](uint64_t a, uint64_t b, uint64_t) { return build(8, [&](unsigned i) { return (ul(a, 8, i) + ul(b, 8, i)) / 2; }); }},
        {0xee921003, "wavg2br", [](uint64_t a, uint64_t b, uint64_t) { return build(8, [&](unsigned i) { return (ul(a, 8, i) + ul(b, 8, i) + 1) / 2; }); }},
        {0xeec21003, "wavg2h", [](uint64_t a, uint64_t b, uint64_t) { return build(16, [&](unsigned i) { return (ul(a, 16, i) + ul(b, 16, i)) / 2; }); }},
        {0xeed21003, "wavg2hr", [](uint64_t a, uint64_t b, uint64_t) { return build(16, [&](unsigned i) { return (ul(a, 16, i) + ul(b, 16, i) + 1) / 2; }); }},
        {0xee521023, "waligni #5", [](uint64_t a, uint64_t b, uint64_t) { return (a >> 40) | (b << 24); }},
        {0xee021023, "waligni #0", [](uint64_t a, uint64_t, uint64_t) { return a; }},
        {0xeeb21023, "walignr3 (wCGR3 = 2)", [](uint64_t a, uint64_t b, uint64_t) { return (a >> 16) | (b << 48); }},
        {0xee021063, "wcmpeqb", [](uint64_t a, uint64_t b, uint64_t) { return build(8, [&](unsigned i) { return ul(a, 8, i) == ul(b, 8, i) ? ~0ull : 0; }); }},
        {0xee421063, "wcmpeqh", [](uint64_t a, uint64_t b, uint64_t) { return build(16, [&](unsigned i) { return ul(a, 16, i) == ul(b, 16, i) ? ~0ull : 0; }); }},
        {0xee821063, "wcmpeqw", [](uint64_t a, uint64_t b, uint64_t) { return build(32, [&](unsigned i) { return ul(a, 32, i) == ul(b, 32, i) ? ~0ull : 0; }); }},
        {0xee121063, "wcmpgtub", [](uint64_t a, uint64_t b, uint64_t) { return build(8, [&](unsigned i) { return ul(a, 8, i) > ul(b, 8, i) ? ~0ull : 0; }); }},
        {0xee521063, "wcmpgtuh", [](uint64_t a, uint64_t b, uint64_t) { return build(16, [&](unsigned i) { return ul(a, 16, i) > ul(b, 16, i) ? ~0ull : 0; }); }},
        {0xee921063, "wcmpgtuw", [](uint64_t a, uint64_t b, uint64_t) { return build(32, [&](unsigned i) { return ul(a, 32, i) > ul(b, 32, i) ? ~0ull : 0; }); }},
        {0xee321063, "wcmpgtsb", [](uint64_t a, uint64_t b, uint64_t) { return build(8, [&](unsigned i) { return sl(a, 8, i) > sl(b, 8, i) ? ~0ull : 0; }); }},
        {0xee721063, "wcmpgtsh", [](uint64_t a, uint64_t b, uint64_t) { return build(16, [&](unsigned i) { return sl(a, 16, i) > sl(b, 16, i) ? ~0ull : 0; }); }},
        {0xeeb21063, "wcmpgtsw", [](uint64_t a, uint64_t b, uint64_t) { return build(32, [&](unsigned i) { return sl(a, 32, i) > sl(b, 32, i) ? ~0ull : 0; }); }},
        {0xee521083, "wpackhus", [](uint64_t a, uint64_t b, uint64_t) { return build(8, [&](unsigned i) { return uint64_t(clamp(sl(i < 4 ? a : b, 16, i % 4), 0, umax(8))); }); }},
        {0xee921083, "wpackwus", [](uint64_t a, uint64_t b, uint64_t) { return build(16, [&](unsigned i) { return uint64_t(clamp(sl(i < 2 ? a : b, 32, i % 2), 0, umax(16))); }); }},
        {0xeed21083, "wpackdus", [](uint64_t a, uint64_t b, uint64_t) { return build(32, [&](unsigned i) { return uint64_t(clamp(int64_t(i ? b : a), 0, umax(32))); }); }},
        {0xee721083, "wpackhss", [](uint64_t a, uint64_t b, uint64_t) { return build(8, [&](unsigned i) { return uint64_t(clamp(sl(i < 4 ? a : b, 16, i % 4), smin(8), smax(8))); }); }},
        {0xeeb21083, "wpackwss", [](uint64_t a, uint64_t b, uint64_t) { return build(16, [&](unsigned i) { return uint64_t(clamp(sl(i < 2 ? a : b, 32, i % 2), smin(16), smax(16))); }); }},
        {0xeef21083, "wpackdss", [](uint64_t a, uint64_t b, uint64_t) { return build(32, [&](unsigned i) { return uint64_t(clamp(int64_t(i ? b : a), smin(32), smax(32))); }); }},
        {0xee0210c0, "wunpckehub", [](uint64_t a, uint64_t, uint64_t) { return build(16, [&](unsigned i) { return ul(a, 8, 4 + i); }); }},
        {0xee4210c0, "wunpckehuh", [](uint64_t a, uint64_t, uint64_t) { return build(32, [&](unsigned i) { return ul(a, 16, 2 + i); }); }},
        {0xee8210c0, "wunpckehuw", [](uint64_t a, uint64_t, uint64_t) { return ul(a, 32, 1); }},
        {0xee2210c0, "wunpckehsb", [](uint64_t a, uint64_t, uint64_t) { return build(16, [&](unsigned i) { return uint64_t(sl(a, 8, 4 + i)); }); }},
        {0xee6210c0, "wunpckehsh", [](uint64_t a, uint64_t, uint64_t) { return build(32, [&](unsigned i) { return uint64_t(sl(a, 16, 2 + i)); }); }},
        {0xeea210c0, "wunpckehsw", [](uint64_t a, uint64_t, uint64_t) { return uint64_t(sl(a, 32, 1)); }},
        {0xee0210e0, "wunpckelub", [](uint64_t a, uint64_t, uint64_t) { return build(16, [&](unsigned i) { return ul(a, 8, i); }); }},
        {0xee2210e0, "wunpckelsb", [](uint64_t a, uint64_t, uint64_t) { return build(16, [&](unsigned i) { return uint64_t(sl(a, 8, i)); }); }},
        {0xee1210c3, "wunpckihb", [](uint64_t a, uint64_t b, uint64_t) { return build(8, [&](unsigned i) { return ul(i & 1 ? b : a, 8, 4 + i / 2); }); }},
        {0xee5210c3, "wunpckihh", [](uint64_t a, uint64_t b, uint64_t) { return build(16, [&](unsigned i) { return ul(i & 1 ? b : a, 16, 2 + i / 2); }); }},
        {0xee9210c3, "wunpckihw", [](uint64_t a, uint64_t b, uint64_t) { return ul(a, 32, 1) | ul(b, 32, 1) << 32; }},
        {0xee1210e3, "wunpckilb", [](uint64_t a, uint64_t b, uint64_t) { return build(8, [&](unsigned i) { return ul(i & 1 ? b : a, 8, i / 2); }); }},
        {0xee5210e3, "wunpckilh", [](uint64_t a, uint64_t b, uint64_t) { return build(16, [&](unsigned i) { return ul(i & 1 ? b : a, 16, i / 2); }); }},
        {0xee9210e3, "wunpckilw", [](uint64_t a, uint64_t b, uint64_t) { return ul(a, 32, 0) | ul(b, 32, 0) << 32; }},
        {0xee021103, "wmulul", [](uint64_t a, uint64_t b, uint64_t) { return build(16, [&](unsigned i) { return ul(a, 16, i) * ul(b, 16, i); }); }},
        {0xee121103, "wmulum", [](uint64_t a, uint64_t b, uint64_t) { return build(16, [&](unsigned i) { return (ul(a, 16, i) * ul(b, 16, i)) >> 16; }); }},
        {0xee221103, "wmulsl", [](uint64_t a, uint64_t b, uint64_t) { return build(16, [&](unsigned i) { return uint64_t(sl(a, 16, i) * sl(b, 16, i)); }); }},
        {0xee321103, "wmulsm", [](uint64_t a, uint64_t b, uint64_t) { return build(16, [&](unsigned i) { return uint64_t((sl(a, 16, i) * sl(b, 16, i)) >> 16); }); }},
        {0xee421103, "wmacu", [](uint64_t a, uint64_t b, uint64_t d) { uint64_t s = d; for (unsigned i = 0; i < 4; i++) s += ul(a, 16, i) * ul(b, 16, i); return s; }},
        {0xee521103, "wmacuz", [](uint64_t a, uint64_t b, uint64_t) { uint64_t s = 0; for (unsigned i = 0; i < 4; i++) s += ul(a, 16, i) * ul(b, 16, i); return s; }},
        {0xee621103, "wmacs", [](uint64_t a, uint64_t b, uint64_t d) { int64_t s = int64_t(d); for (unsigned i = 0; i < 4; i++) s += sl(a, 16, i) * sl(b, 16, i); return uint64_t(s); }},
        {0xee721103, "wmacsz", [](uint64_t a, uint64_t b, uint64_t) { int64_t s = 0; for (unsigned i = 0; i < 4; i++) s += sl(a, 16, i) * sl(b, 16, i); return uint64_t(s); }},
        {0xee821103, "wmaddu", [](uint64_t a, uint64_t b, uint64_t) { return build(32, [&](unsigned i) { return ul(a, 16, 2 * i) * ul(b, 16, 2 * i) + ul(a, 16, 2 * i + 1) * ul(b, 16, 2 * i + 1); }); }},
        {0xeea21103, "wmadds", [](uint64_t a, uint64_t b, uint64_t) { return build(32, [&](unsigned i) { return uint64_t(sl(a, 16, 2 * i) * sl(b, 16, 2 * i) + sl(a, 16, 2 * i + 1) * sl(b, 16, 2 * i + 1)); }); }},
        {0xee021123, "wsadb", [](uint64_t a, uint64_t b, uint64_t d) { uint32_t s = uint32_t(d); for (unsigned i = 0; i < 8; i++) s += uint32_t(std::abs(int64_t(ul(a, 8, i)) - int64_t(ul(b, 8, i)))); return uint64_t(s); }},
        {0xee121123, "wsadbz", [](uint64_t a, uint64_t b, uint64_t) { uint32_t s = 0; for (unsigned i = 0; i < 8; i++) s += uint32_t(std::abs(int64_t(ul(a, 8, i)) - int64_t(ul(b, 8, i)))); return uint64_t(s); }},
        {0xee421123, "wsadh", [](uint64_t a, uint64_t b, uint64_t d) { uint32_t s = uint32_t(d); for (unsigned i = 0; i < 4; i++) s += uint32_t(std::abs(int64_t(ul(a, 16, i)) - int64_t(ul(b, 16, i)))); return uint64_t(s); }},
        {0xee521123, "wsadhz", [](uint64_t a, uint64_t b, uint64_t) { uint32_t s = 0; for (unsigned i = 0; i < 4; i++) s += uint32_t(std::abs(int64_t(ul(a, 16, i)) - int64_t(ul(b, 16, i)))); return uint64_t(s); }},
        {0xee021163, "wmaxub", [](uint64_t a, uint64_t b, uint64_t) { return build(8, [&](unsigned i) { return std::max(ul(a, 8, i), ul(b, 8, i)); }); }},
        {0xee421163, "wmaxuh", [](uint64_t a, uint64_t b, uint64_t) { return build(16, [&](unsigned i) { return std::max(ul(a, 16, i), ul(b, 16, i)); }); }},
        {0xee821163, "wmaxuw", [](uint64_t a, uint64_t b, uint64_t) { return build(32, [&](unsigned i) { return std::max(ul(a, 32, i), ul(b, 32, i)); }); }},
        {0xee221163, "wmaxsb", [](uint64_t a, uint64_t b, uint64_t) { return build(8, [&](unsigned i) { return uint64_t(std::max(sl(a, 8, i), sl(b, 8, i))); }); }},
        {0xee621163, "wmaxsh", [](uint64_t a, uint64_t b, uint64_t) { return build(16, [&](unsigned i) { return uint64_t(std::max(sl(a, 16, i), sl(b, 16, i))); }); }},
        {0xeea21163, "wmaxsw", [](uint64_t a, uint64_t b, uint64_t) { return build(32, [&](unsigned i) { return uint64_t(std::max(sl(a, 32, i), sl(b, 32, i))); }); }},
        {0xee121163, "wminub", [](uint64_t a, uint64_t b, uint64_t) { return build(8, [&](unsigned i) { return std::min(ul(a, 8, i), ul(b, 8, i)); }); }},
        {0xeeb21163, "wminsw", [](uint64_t a, uint64_t b, uint64_t) { return build(32, [&](unsigned i) { return uint64_t(std::min(sl(a, 32, i), sl(b, 32, i))); }); }},
        {0xee0211c0, "waccb", [](uint64_t a, uint64_t, uint64_t) { uint64_t s = 0; for (unsigned i = 0; i < 8; i++) s += ul(a, 8, i); return s; }},
        {0xee4211c0, "wacch", [](uint64_t a, uint64_t, uint64_t) { uint64_t s = 0; for (unsigned i = 0; i < 4; i++) s += ul(a, 16, i); return s; }},
        {0xee8211c0, "waccw", [](uint64_t a, uint64_t, uint64_t) { return ul(a, 32, 0) + ul(a, 32, 1); }},
        {0xee1211eb, "wshufh #0x1b", [](uint64_t a, uint64_t, uint64_t) { return build(16, [&](unsigned i) { return ul(a, 16, 3 - i); }); }},
    };
    // WADD and WSUB in every size and saturation.
    const char* sizes = "bhw";
    for (unsigned s = 0; s < 3; s++) {
        const unsigned bits = 8u << s;
        for (unsigned sub = 0; sub < 2; sub++) {
            for (unsigned sat : {0u, 1u, 3u}) {
                const std::string name = std::string(sub ? "wsub" : "wadd") + sizes[s] + (sat == 1 ? "us" : sat == 3 ? "ss" : "");
                const uint32_t enc = (sub ? 0xee0211a3 : 0xee021183) | (s << 22) | (sat << 20);
                ops.push_back({enc, name, [bits, sub, sat](uint64_t a, uint64_t b, uint64_t) {
                    return build(bits, [&](unsigned i) {
                        if (sat == 1) {
                            const int64_t x = sub ? int64_t(ul(a, bits, i)) - int64_t(ul(b, bits, i)) : int64_t(ul(a, bits, i) + ul(b, bits, i));
                            return uint64_t(clamp(x, 0, umax(bits)));
                        }
                        const int64_t x = sub ? sl(a, bits, i) - sl(b, bits, i) : sl(a, bits, i) + sl(b, bits, i);
                        return uint64_t(sat == 3 ? clamp(x, smin(bits), smax(bits)) : x);
                    });
                }});
            }
        }
    }
    // Shifts by register (wR3) and by wCGR1 (the "g" forms), with counts in and out of range.
    const char* shiftNames[] = {"wsra", "wsll", "wsrl", "wror"};
    for (unsigned kind = 0; kind < 4; kind++) {
        for (unsigned size = 1; size < 4; size++) {
            const unsigned bits = 8u << size;
            for (unsigned g = 0; g < 2; g++) {
                const std::string name = std::string(shiftNames[kind]) + "hwd"[size - 1] + (g ? "g" : "");
                const uint32_t enc = (g ? 0xee021149 : 0xee021043) | (size << 22) | (kind << 20);  // g: wCGR1
                ops.push_back({enc, name, [bits, kind](uint64_t a, uint64_t b, uint64_t) {
                    const unsigned n = unsigned(b & 0xff);
                    return build(bits, [&](unsigned i) {
                        const uint64_t x = ul(a, bits, i);
                        switch (kind) {
                            case 0: return uint64_t(sl(a, bits, i) >> std::min(n, bits - 1));
                            case 1: return n >= bits ? 0 : x << n;
                            case 2: return n >= bits ? 0 : x >> n;
                            default: { const unsigned r = n % bits; return r ? (x >> r) | (x << (bits - r)) : x; }
                        }
                    });
                }});
            }
        }
    }

    unsigned ran = 0;
    for (auto& op : ops) {
        const uint32_t enc = op.enc;
        const bool isShift = op.name.rfind("wsra", 0) == 0 || op.name.rfind("wsll", 0) == 0 ||
                             op.name.rfind("wsrl", 0) == 0 || op.name.rfind("wror", 0) == 0;
        for (unsigned k = 0; k < sizeof A / sizeof A[0]; k++) {
            for (uint64_t count : {uint64_t(0), uint64_t(3), uint64_t(15), uint64_t(17), uint64_t(40), uint64_t(70)}) {
                uint64_t b = B[k];
                if (isShift) b = (b & ~0xffull) | count;
                else if (count) continue;
                setW(1, D); setW(2, A[k]); setW(3, b);
                setC(9, uint32_t(b));  // wCGR1, for the "g" shifts
                setC(11, 2);           // wCGR3, for walignr3
                char what[160];
                snprintf(what, sizeof what, "%s a=%016llx b=%016llx", op.name.c_str(), (unsigned long long)A[k], (unsigned long long)b);
                if (!run(enc)) { check(false, std::string(what) + " was undefined"); continue; }
                const uint64_t got = getW(1), want = op.f(A[k], b, D);
                if (got != want) {
                    char more[80];
                    snprintf(more, sizeof more, ": got %016llx want %016llx", (unsigned long long)got, (unsigned long long)want);
                    check(false, std::string(what) + more);
                }
                ran++;
            }
        }
    }

    // Transfers between core and iWMMXt registers.
    setW(2, 0x8899'aabb'ccdd'eeffull);
    cpuSetReg(cpu, 2, 0x12345678);
    check(run(0xee412010) && getW(1) == 0x7878'7878'7878'7878ull, "tbcstb");
    check(run(0xee412050) && getW(1) == 0x5678'5678'5678'5678ull, "tbcsth");
    check(run(0xee412090) && getW(1) == 0x1234'5678'1234'5678ull, "tbcstw");
    check(run(0xee4eb050), "tbcsth wR14, r11 (the Media app's first iWMMXt instruction)");
    setW(1, 0);
    check(run(0xee612015) && getW(1) == 0x0000'7800'0000'0000ull, "tinsrb #5");
    setW(1, 0);
    check(run(0xee612053) && getW(1) == 0x5678'0000'0000'0000ull, "tinsrh #3");
    setW(1, 0);
    check(run(0xee612091) && getW(1) == 0x1234'5678'0000'0000ull, "tinsrw #1");
    check(run(0xee121073) && reg(1) == 0xcc, "textrmub #3");
    check(run(0xee12107b) && reg(1) == 0xffffffcc, "textrmsb #3");
    check(run(0xee521073) && reg(1) == 0x8899, "textrmuh #3");
    check(run(0xee52107b) && reg(1) == 0xffff8899, "textrmsh #3");
    check(run(0xee921071) && reg(1) == 0x8899aabb, "textrmuw #1");
    setW(2, 0x8000'7fff'0080'ff00ull);
    check(run(0xee121030) && reg(1) == 0x96, "tmovmskb");
    check(run(0xee521030) && reg(1) == 0x9, "tmovmskh");
    check(run(0xee921030) && reg(1) == 0x2, "tmovmskw");

    // Multiply-accumulate from core registers.
    setW(1, 10); cpuSetReg(cpu, 2, uint32_t(-3)); cpuSetReg(cpu, 3, 0x7fffffff);
    check(run(0xee203032) && getW(1) == uint64_t(10 + int64_t(-3) * 0x7fffffff), "tmia");
    setW(1, 0); cpuSetReg(cpu, 2, 0x0002'fffd); cpuSetReg(cpu, 3, 0x0005'0007);
    check(run(0xee283032) && getW(1) == uint64_t(2 * 5 + -3 * 7), "tmiaph");
    setW(1, 0);
    check(run(0xee2d3032) && getW(1) == uint64_t(-3 * 5), "tmiabt");
    setW(1, 0);
    check(run(0xee2e3032) && getW(1) == uint64_t(2 * 7), "tmiatb");

    // Flags: per-lane NZCV in wCASF, gathered into the CPSR by TANDC, TORC and TEXTRC.
    setW(2, 0x0000'8000'0001'7fffull); setW(3, 0x0000'8000'ffff'0001ull);
    check(run(0xee421183), "waddh");  // lanes: 8000 (N V), 0000 (Z C), 0000 (Z C V), 0000 (Z)
    const uint32_t casf = getC(3);
    check(casf == ((0x9u << 4) | (0x6u << 12) | (0x7u << 20) | (0x4u << 28)),
          "waddh flags " + std::to_string(casf));
    check(run(0xee53f130) && (reg(ARM_REG_NUM_CPSR) >> 28) == 0x0, "tandch");
    check(run(0xee53f150) && (reg(ARM_REG_NUM_CPSR) >> 28) == 0xf, "torch");
    check(run(0xee53f173) && (reg(ARM_REG_NUM_CPSR) >> 28) == 0x4, "textrch #3");
    check(run(0xee53f170) && (reg(ARM_REG_NUM_CPSR) >> 28) == 0x9, "textrch #0");
    setW(2, 0xff); setW(3, 0x01); setC(2, 0);
    check(run(0xee121183) && getW(1) == 0xff && (getC(2) & 1), "waddbus saturates and sets wCSSF");

    // Loads and stores, with writeback.
    std::memset(memory.data() + 0x100, 0, 0x40);
    for (unsigned i = 0; i < 16; i++) memory[0x100 + i] = uint8_t(0xa0 + i);
    cpuSetReg(cpu, 2, base + 0xfc);
    check(run(0xed921004) && getW(1) == 0xa0, "wldrb [r2, #4]");
    check(run(0xedd21004) && getW(1) == 0xa1a0, "wldrh [r2, #4]");
    check(run(0xed921101) && getW(1) == 0xa3a2a1a0, "wldrw [r2, #4]");
    cpuSetReg(cpu, 2, base + 0xf8);
    check(run(0xedd21102) && getW(1) == 0xa7a6a5a4a3a2a1a0ull, "wldrd [r2, #8]");
    check(run(0xedf21102) && getW(1) == 0xa7a6a5a4a3a2a1a0ull && reg(2) == base + 0x100, "wldrd [r2, #8]!");
    check(run(0xec321004) && getW(1) == 0xa0 && reg(2) == base + 0xfc, "wldrb [r2], #-4");
    setW(1, 0x0102'0304'0506'0708ull);
    cpuSetReg(cpu, 2, base + 0x118);
    check(run(0xedc21102), "wstrd [r2, #8]");
    uint64_t stored; std::memcpy(&stored, memory.data() + 0x120, 8);
    check(stored == 0x0102'0304'0506'0708ull, "wstrd wrote the doubleword");
    check(run(0xedc21004), "wstrh [r2, #4]");
    check(memory[0x11c] == 0x08 && memory[0x11d] == 0x07, "wstrh wrote the halfword");
    setC(9, 0xcafef00d);
    check(run(0xfd829101), "wstrw wcgr1, [r2, #4]");
    uint32_t word; std::memcpy(&word, memory.data() + 0x11c, 4);
    check(word == 0xcafef00d, "wstrw wcgr1 wrote the word");
    setC(9, 0);
    check(run(0xfd929101) && getC(9) == 0xcafef00d, "wldrw wcgr1, [r2, #4]");

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("PASS: %u iWMMXt data cases, transfers, TMIA, flags and loads and stores\n", ran);
}
