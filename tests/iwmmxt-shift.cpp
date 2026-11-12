// iWMMXt shifts and rotates, the register forms (count in the low byte of wRm) and the G
// forms (count in the low byte of wCGR0-3, as QEMU takes it), at counts around each element
// width, plus TMCRR and TMRRC.
// Logical shifts by the element width or more give 0, arithmetic shifts fill with the sign,
// rotates wrap the count at the element width. Each result and the N and Z flags in wCASF
// are checked against a plain reference.
#include <cassert>
#include <cstdint>
#include <cstdio>
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
static void step(uint32_t instruction) {
    write32(0xa0000000, instruction);
    cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu, ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC);
    cpuSetReg(cpu, 15, 0xa0000000);
    cpuSetReg(cpu, 0, 0xa0001000);
    cpuCyclePure(cpu);
    assert(cpuGetRegisters(cpu)[15] == 0xa0000004);  // an undefined instruction would have trapped
}
static void loadWmmx(unsigned reg, uint64_t value) {
    write32(0xa0001000, uint32_t(value));
    write32(0xa0001004, uint32_t(value >> 32));
    step(0xedd00100 | (reg << 12));  // LDCL p1,cN,[r0]
}
static uint64_t readWmmx(unsigned reg) {
    step(0xedc00100 | (reg << 12));  // STCL p1,cN,[r0]
    return uint64_t(read32(0xa0001000)) | (uint64_t(read32(0xa0001004)) << 32);
}
static void writeControl(unsigned reg, uint32_t value) {
    write32(0xa0001000, value);
    step(0xfd900100 | (reg << 12));  // LDC2 p1,cN,[r0]: control registers
}
static uint32_t readControl(unsigned reg) {
    step(0xfd800100 | (reg << 12));  // STC2 p1,cN,[r0]
    return read32(0xa0001000);
}

enum Kind { SRA, SLL, SRL, ROR };

// One element of `bits` bits shifted by `count`.
static uint64_t element(Kind kind, unsigned bits, uint64_t v, uint32_t count) {
    const uint64_t mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
    v &= mask;
    switch (kind) {
        case SRA: {
            const unsigned n = count >= bits ? bits - 1 : count;
            const bool neg = v >> (bits - 1);
            uint64_t r = v >> n;
            if (neg && n) r |= mask << (bits - n);
            return r & mask;
        }
        case SLL: return count >= bits ? 0 : (v << count) & mask;
        case SRL: return count >= bits ? 0 : v >> count;
        default: {
            const unsigned n = count % bits;
            return n ? ((v >> n) | (v << (bits - n))) & mask : v;
        }
    }
}

int main() {
    cpu = makeCpu();
    cpuSetCPAR(cpu, 3);

    const uint64_t inputs[] = {0x8123456789abcdefull, 0x0000000100000001ull, 0xffffffffffffffffull, 0};
    const unsigned widths[] = {16, 32, 64};
    unsigned checked = 0;
    for (Kind kind : {SRA, SLL, SRL, ROR})
        for (unsigned wi = 0; wi < 3; ++wi) {
            const unsigned bits = widths[wi];
            // op1: element size in bits 3:2 (01 h, 10 w, 11 d), kind in bits 1:0 (SRA, SLL, SRL, ROR).
            const unsigned op1Final = ((wi + 1) << 2) | unsigned(kind);
            std::vector<uint32_t> counts = {0, 1, bits - 1, bits, bits + 1, 255};
            for (bool g : {false, true})
                for (uint32_t count : counts)
                    for (uint32_t extra : g ? std::vector<uint32_t>{0, 0x100, 0x10000} : std::vector<uint32_t>{0}) {
                        if (g && extra && count > 64) continue;
                        const uint32_t cnt = count + extra;
                        for (uint64_t in : inputs) {
                            loadWmmx(1, in);
                            unsigned crm = 2;
                            if (g) { writeControl(8 + 1, cnt); crm = 8 + 1; }
                            else loadWmmx(2, cnt);
                            // CDP p0 or p1, op1, CRd 3, CRn 1, CRm, op2 = 2
                            step(0xee000000 | (op1Final << 20) | (1u << 16) | (3u << 12) | ((g ? 1u : 0u) << 8) | (2u << 5) | crm);
                            uint64_t want = 0, flags = 0;
                            const unsigned n = 64 / bits;
                            for (unsigned i = 0; i < n; ++i) {
                                const uint64_t e = element(kind, bits, in >> (i * bits), cnt & 0xff);
                                want |= e << (i * bits);
                                if (bits == 64) {
                                    flags = (e >> 63 ? 0x80000000ull : 0) | (e ? 0 : 0x40000000ull);
                                } else {
                                    const unsigned base = i * (bits == 16 ? 8 : 16);
                                    if (e >> (bits - 1)) flags |= (bits == 16 ? 0x80ull : 0x8000ull) << base;
                                    if (!e) flags |= (bits == 16 ? 0x40ull : 0x4000ull) << base;
                                }
                            }
                            const uint64_t got = readWmmx(3);
                            if (got != want || readControl(3) != uint32_t(flags)) {
                                printf("kind %d bits %u %s count %u in %016llx: got %016llx flags %08x, want %016llx flags %08llx\n",
                                       kind, bits, g ? "G" : "reg", cnt, (unsigned long long)in, (unsigned long long)got,
                                       readControl(3), (unsigned long long)want, (unsigned long long)flags);
                                return 1;
                            }
                            ++checked;
                        }
                    }
        }
    printf("iwmmxt shifts: %u results ok\n", checked);

    // TMCRR wRn from two core registers, TMRRC back; the transfer does not trap.
    cpuSetReg(cpu, 1, 0x89abcdef);
    cpuSetReg(cpu, 2, 0x01234567);
    step(0xec400000 | (2u << 16) | (1u << 12) | 5);  // MCRR p0,0,r1,r2,c5
    assert(readWmmx(5) == 0x0123456789abcdefull);
    cpuSetReg(cpu, 3, 0);
    cpuSetReg(cpu, 4, 0);
    step(0xec500000 | (4u << 16) | (3u << 12) | 5);  // MRRC p0,0,r3,r4,c5
    assert(cpuGetRegExternal(cpu, 3) == 0x89abcdef && cpuGetRegExternal(cpu, 4) == 0x01234567);
    printf("iwmmxt tmcrr/tmrrc: ok\n");
    return 0;
}
