#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "imx1_boot_probe.h"

namespace {
void literal(std::vector<uint32_t>& words, unsigned reg, uint32_t value) {
    words.push_back(0xe59f0000 | reg << 12);
    words.push_back(0xea000000);
    words.push_back(value);
}
Imx1ProbeResult run(std::vector<uint32_t> words) {
    words.push_back(0xe7f000f0);
    Imx1ProbeOptions options{};
    options.bootstrapPeripherals = true;
    const auto r = imx1BootProbe(reinterpret_cast<const uint8_t*>(words.data()), words.size() * 4,
                                Imx1ProbeEntry::physicalFlash, 100, nullptr, nullptr, false, &options);
    assert(!r.jitBlocks && !r.jitInstructions);
    return r;
}
uint32_t transfer(unsigned base, unsigned dest, unsigned offset, bool load, unsigned type,
                  bool pre = true, bool up = true, bool immediate = true, bool writeback = false) {
    return 0xe0000090 | uint32_t(pre) << 24 | uint32_t(up) << 23 | uint32_t(immediate) << 22 |
           uint32_t(writeback) << 21 | uint32_t(load) << 20 | base << 16 | dest << 12 | type << 5 |
           (immediate ? ((offset & 0xf0) << 4) | (offset & 15) : offset);
}
std::vector<uint32_t> memory() {
    std::vector<uint32_t> words;
    literal(words, 0, 0x00300100);
    literal(words, 1, 0x80feff7f);
    words.push_back(0xe5801000);
    return words;
}
void finished(const Imx1ProbeResult& r) {
    assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.instruction == 0xe7f000f0);
}
void jumpTable() {
    for (unsigned index = 0; index < 3; ++index) {
        const auto r = run({0xe3a03000 | index, 0xe3530002, 0x908ff103, 0xe7f000f0,
                            0xea000001, 0xea000002, 0xea000003,
                            0xe3a04010, 0xea000003, 0xe3a04011, 0xea000001,
                            0xe3a04012, 0xeaffffff});
        finished(r);
        assert(r.trace.pc == 0x10000034 && r.registers[4] == 0x10 + index);
        assert(r.cpsr == (index == 2 ? 0x600000d3u : 0x800000d3u));
        assert(!r.busAccesses && r.registers[3] == index);
    }
    const auto skipped = run({0xe3a03003, 0xe3530002, 0x908ff103, 0xe3a04099});
    finished(skipped);
    assert(skipped.trace.pc == 0x10000010 && skipped.registers[4] == 0x99 &&
           skipped.cpsr == 0x200000d3);
    // Flag-setting PC writes, other data-processing opcodes and register-specified shifts stay gated.
    for (uint32_t instruction : {0xe09ff103u, 0xe0cff103u, 0xe19ff103u, 0xe08ff213u}) {
        const auto r = run({instruction});
        assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.step == 0 &&
               r.trace.instruction == instruction && r.registers[15] == 0x10000000 &&
               r.cpsr == 0xd3 && !r.busAccesses);
    }
}
void blockTransfers() {
    for (bool pre : {false, true}) for (bool up : {false, true}) {
        const uint32_t base = 0x00300140;
        const uint32_t mode = uint32_t(pre) << 24 | uint32_t(up) << 23;
        const uint32_t finalBase = up ? base + 8 : base - 8;
        const uint32_t first = up ? base + (pre ? 4 : 0) : base - (pre ? 8 : 4);
        std::vector<uint32_t> words;
        literal(words, 0, base); literal(words, 1, 0x11223344); literal(words, 2, 0x55667788);
        words.push_back(0xe8200006 | mode);  // STM{mode} r0!,{r1,r2}
        literal(words, 3, base);
        words.push_back(0xe8330030 | mode);  // LDM{mode} r3!,{r4,r5}
        literal(words, 6, first); words.push_back(0xe5967000); words.push_back(0xe5968004);
        const auto r = run(words); finished(r);
        assert(r.registers[0] == finalBase && r.registers[3] == finalBase &&
               r.registers[4] == 0x11223344 && r.registers[5] == 0x55667788 &&
               r.registers[7] == 0x11223344 && r.registers[8] == 0x55667788);
    }
    // The observed STMIB has no writeback. A base in the list is valid without writeback.
    std::vector<uint32_t> words;
    literal(words, 0, 0x00300140); literal(words, 1, 0x11223344);
    words.push_back(0xe9800003);  // STMIB r0,{r0,r1}
    words.push_back(0xe5902004); words.push_back(0xe5903008);
    auto r = run(words); finished(r);
    assert(r.registers[0] == 0x00300140 && r.registers[2] == 0x00300140 && r.registers[3] == 0x11223344);
    for (uint32_t instruction : {0xe9c08006u, 0xe9800000u, 0xe98f0006u,
                                 0xe9a00003u, 0xe9300001u}) {
        words.clear(); literal(words, 0, 0x00300140); words.push_back(instruction);
        r = run(words);
        assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.instruction == instruction);
        assert(r.busAccesses == 0 && r.registers[0] == 0x00300140);
    }
}
}  // namespace

int main() {
    auto words = memory();
    words.push_back(transfer(0, 2, 0, true, 1));
    words.push_back(transfer(0, 3, 0, true, 3));
    words.push_back(transfer(0, 4, 1, true, 2));
    words.push_back(0xe3a05002);
    words.push_back(transfer(0, 6, 5, true, 1, true, true, false));
    words.push_back(transfer(0, 7, 2, true, 1, true, true, true, true));
    words.push_back(transfer(0, 8, 2, true, 3, false, false));
    auto r = run(words);
    finished(r);
    assert(r.registers[0] == 0x00300100 && r.registers[2] == 0xff7f &&
           r.registers[3] == 0xffffff7f && r.registers[4] == 0xffffffff &&
           r.registers[6] == 0x80fe && r.registers[7] == 0x80fe && r.registers[8] == 0xffff80fe);

    // Immediate and register stores, post-index writeback, and negative offsets.
    words = memory();
    words.push_back(transfer(0, 1, 4, false, 1, true, true, true, true));
    words.push_back(transfer(0, 2, 0, true, 1));
    words.push_back(0xe3a05002);
    words.push_back(transfer(0, 1, 5, false, 1, false, false, false));
    words.push_back(transfer(0, 3, 2, true, 1, true, true));
    words.push_back(transfer(0, 4, 2, true, 1, true, false));
    r = run(words); finished(r);
    assert(r.registers[0] == 0x00300102 && r.registers[2] == 0xff7f &&
           r.registers[3] == 0xff7f && r.registers[4] == 0xff7f);

    words = memory(); words.push_back(0xe3a05002);
    words.push_back(transfer(0, 2, 5, true, 1, true, true, false, true));
    words.push_back(transfer(0, 3, 5, true, 1, false, true, false));
    words.push_back(transfer(0, 1, 0x5c, false, 1));
    words.push_back(0xe5904060);  // Read a different word from the stored halfword.
    words.push_back(0xe590605c);
    r = run(words); finished(r);
    assert(r.registers[0] == 0x00300104 && r.registers[2] == 0x80fe &&
           r.registers[3] == 0x80fe && !r.registers[4] && r.registers[6] == 0xff7f);

    r = run({transfer(15, 2, 0, true, 1), 0xe7f000f0, 0x12348001});
    finished(r); assert(r.registers[2] == 0x8001);
    words = memory(); words.push_back(transfer(0, 2, 3, true, 2));
    r = run(words); finished(r); assert(r.registers[2] == 0xffffff80);

    // Reject unpredictable encodings and ARMv5 doubleword aliases before touching memory.
    for (uint32_t instruction : {
             transfer(0, 15, 0, true, 1), transfer(0, 0, 2, true, 1, true, true, true, true),
             transfer(0, 0, 2, false, 1, true, true, true, true),
             transfer(0, 2, 0, true, 1, true, true, false, true),
             transfer(0, 1, 0, false, 1, false, true, false),
             transfer(15, 2, 2, true, 1, false), transfer(0, 2, 15, true, 1, true, true, false),
             transfer(0, 2, 1, true, 1), transfer(0, 2, 1, true, 3), transfer(0, 1, 1, false, 1),
             transfer(0, 2, 0, true, 1, false, true, true, true),
             transfer(0, 2, 0, true, 1, true, true, false) | 0x100,
             0xe1c020d0u, 0xe1c020f0u}) {
        words = memory(); words.push_back(instruction);
        r = run(words);
        assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.instruction == instruction);
        assert(r.busAccesses == 1 && r.registers[0] == 0x00300100);
    }
    words = memory(); words.push_back(transfer(0, 2, 1, true, 1) & 0x0fffffff);
    r = run(words); finished(r); assert(r.busAccesses == 1 && !r.registers[2]);
    words.clear(); literal(words, 0, 0x0021c024); words.push_back(transfer(0, 2, 0, true, 1));
    r = run(words);
    assert(r.stop == Imx1ProbeStop::unsupportedPeripheral && r.address == 0x0021c024 &&
           r.size == 2 && r.peripheralStatus == Imx1BootstrapAccess::width);
    words.clear(); literal(words, 0, 0x00400000); words.push_back(transfer(0, 2, 0, true, 1));
    r = run(words); assert(r.stop == Imx1ProbeStop::unsupportedRead && r.address == 0x00400000);

    // Non-flag scalar multiplication retains low 32 bits and supports accumulation.
    words.clear(); literal(words, 0, 0xfffffffe); words.push_back(0xe3a01003);
    words.push_back(0xe0020190);  // MUL r2,r0,r1
    words.push_back(0xe3a03007); words.push_back(0xe0243190);  // MLA r4,r0,r1,r3
    words.push_back(0x00050190);  // MULEQ skipped with Z clear.
    r = run(words); finished(r);
    assert(r.registers[2] == 0xfffffffa && r.registers[4] == 1 && !r.registers[5]);
    assert(r.cpsr == 0xd3);
    // Rd may be Rm: the ARM920T gives the ordinary product.
    words.clear(); literal(words, 0, 0xfffffffe); words.push_back(0xe3a01003);
    words.push_back(0xe0000190);  // MUL r0,r0,r1
    r = run(words); finished(r);
    assert(r.registers[0] == 0xfffffffa && r.registers[1] == 3 && r.cpsr == 0xd3);
    for (uint32_t instruction : {0xe0021190u,
                                 0xe00f0190u, 0xe0020f90u, 0xe002019fu, 0xe022f190u,
                                 0xe0802190u, 0xe1000091u}) {  // SWP with Rn == Rd
        r = run({instruction});
        assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.step == 0);
    }
    blockTransfers();
    jumpTable();
    std::puts("PASS: Exact ARMv4 transfers, scalar multiply, aligned PC-relative jump tables and conservative instruction guards");
}
