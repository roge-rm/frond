// The TwGlue log: a small ARM program is run one instruction at a time with the log watching, and the file it
// writes is checked for the entry, a table call with a string argument, its return, an r9 module call, and the
// dumps of TwInputSetFormat's features, TwInputPoll's buffer, TwGetDBDataDirectory's results and
// VFSVolumeEnumerate's iterator and volume. Hand-stepped calls after it check that a call restarted or branched
// back to its first instruction is logged once, the poll rate limit, VFSDirEntryEnumerate's fields, BlendBlt's
// point and rect, and a TwGetDBDataDirectory string at the end of mapped memory. The MMU
// is on with a coarse page table, so the log's own page walk is used for every read.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "CPU.h"
#include "MMU.h"
#include "imx1_boot_probe.h"
#include "memory_buffer.h"
#include "RAM.h"
#include "patch_dispatch.h"
#include "system_state.h"
#include "twglue_log.h"

namespace {
MemoryBuffer backing{};
void put(uint32_t address, uint32_t value) { std::memcpy(backing.buffer + address, &value, 4); }
uint32_t bl(uint32_t from, uint32_t to) { return 0xeb000000 | (((to - from - 8) >> 2) & 0xffffff); }
uint32_t movImm(uint32_t reg, uint32_t value) {  // mov reg,#value for a value an ARM immediate can hold
    for (uint32_t rot = 0; rot < 16; ++rot) {
        const uint32_t v = rot ? (value << (2 * rot)) | (value >> (32 - 2 * rot)) : value;
        if (v < 256) return 0xe3a00000 | reg << 12 | rot << 8 | v;
    }
    assert(false);
    return 0;
}
bool physical(void*, uint32_t pa, void* data, uint32_t size) {
    if (uint64_t(pa) + size > 0x20000) return false;
    std::memcpy(data, backing.buffer + pa, size);
    return true;
}
}

int main() {
    assert(memoryBufferAllocate(&backing, 0x20000));
    constexpr uint32_t app = 0x1000, fn = 0x2000, moduleFn = 0x2100, stub = 0x2200, table = 0x4000, module = 0x5200,
                       r9 = 0x5100, text = 0x3000, end = 0x1100;
    put(app, 0xea000000 | ((0x40 - 8) >> 2));  // b app+0x40
    put(app + 28, 0x54574e41);                  // "ANWT"
    uint32_t at = app + 0x40;
    put(at, 0xe1a05001); at += 4;               // mov r5,r1
    put(at, 0xe5954014); at += 4;               // ldr r4,[r5,#20]
    put(at, 0xe3a00007); at += 4;               // mov r0,#7
    put(at, 0xe3a01a03); at += 4;               // mov r1,#0x3000
    put(at, 0xe1a0e00f); at += 4;               // mov lr,pc
    put(at, 0xe12fff14); at += 4;               // bx r4
    // TwInputSetFormat(3, 0x3300, 16), TwInputPoll(3, 0x3400, 8), TwGetDBDataDirectory(0, 0x3100, 0x3200, 32),
    // VFSVolumeEnumerate(0x3600, 0x3500)
    const uint32_t calls[4][5] = {{333, 3, 0x3300, 16, 0}, {336, 3, 0x3400, 8, 0}, {472, 0, 0x3100, 0x3200, 32},
                                  {241, 0x3600, 0x3500, 0, 0}};
    for (const auto& c : calls) {
        put(at, 0xe5954000 | c[0] * 4); at += 4;  // ldr r4,[r5,#slot*4]
        for (uint32_t reg = 0; reg < 4; ++reg) { put(at, movImm(reg, c[reg + 1])); at += 4; }
        put(at, 0xe1a0e00f); at += 4;           // mov lr,pc
        put(at, 0xe12fff14); at += 4;           // bx r4
    }
    put(at, 0xe3a09c51); at += 4;               // mov r9,#0x5100
    put(at, bl(at, stub)); at += 4;
    put(end, 0xeafffffe);                       // b .
    put(at, 0xea000000 | (((end - at - 8) >> 2) & 0xffffff));
    put(fn, 0xe3a0002a); put(fn + 4, 0xe12fff1e);            // mov r0,#42 ; bx lr
    put(moduleFn, 0xe3a00009); put(moduleFn + 4, 0xe12fff1e);  // mov r0,#9 ; bx lr
    put(stub, 0xe519c004); put(stub + 4, 0xe59cf008);          // ldr ip,[r9,#-4] ; ldr pc,[ip,#8]
    put(r9 - 4, module); put(module + 8, moduleFn);
    for (uint32_t i = 0; i < TwGlueLog::slots; ++i) put(table + 4 * i, 0x20000000 + 4 * i);
    put(table + 20, fn);
    constexpr uint32_t setFormat = 0x2400, poll = 0x2500, dataDir = 0x2600;
    put(setFormat, 0xe12fff1e);                                    // bx lr
    put(poll, 0xe3a03f4a); put(poll + 4, 0xe5813000);              // mov r3,#296 ; str r3,[r1]
    put(poll + 8, 0xe3e03004); put(poll + 12, 0xe5813004);         // mvn r3,#4 ; str r3,[r1,#4]
    put(poll + 16, 0xe3a00000); put(poll + 20, 0xe12fff1e);        // mov r0,#0 ; bx lr
    put(dataDir, 0xe3a03041); put(dataDir + 4, 0xe5c23000);        // mov r3,#'A' ; strb r3,[r2]
    put(dataDir + 8, 0xe3a03001); put(dataDir + 12, 0xe5c23001);   // mov r3,#1 ; strb r3,[r2,#1]
    put(dataDir + 16, 0xe3a03000); put(dataDir + 20, 0xe5c23002);  // mov r3,#0 ; strb r3,[r2,#2]
    put(dataDir + 24, 0xe3a03005); put(dataDir + 28, 0xe1c130b0);  // mov r3,#5 ; strh r3,[r1]
    put(dataDir + 32, 0xe3a00000); put(dataDir + 36, 0xe12fff1e);  // mov r0,#0 ; bx lr
    constexpr uint32_t volumeEnumerate = 0x2700;
    put(volumeEnumerate, 0xe3a03002); put(volumeEnumerate + 4, 0xe1c030b0);   // mov r3,#2 ; strh r3,[r0]
    put(volumeEnumerate + 8, 0xe3e03000); put(volumeEnumerate + 12, 0xe5813000);  // mvn r3,#0 ; str r3,[r1]
    put(volumeEnumerate + 16, 0xe3a00000); put(volumeEnumerate + 20, 0xe12fff1e);  // mov r0,#0 ; bx lr
    put(table + 241 * 4, volumeEnumerate);
    put(0x3600, 7);
    put(table + 333 * 4, setFormat);
    put(table + 336 * 4, poll);
    put(table + 472 * 4, dataDir);
    constexpr uint32_t dirEnumerate = 0x2800, blend = 0x2900;  // only hand-stepped, never run
    put(table + 214 * 4, dirEnumerate);
    put(table + 386 * 4, blend);
    put(0x3300, 296); put(0x3304, 0); put(0x3308, 297); put(0x330c, 0xffffffff);
    std::memcpy(backing.buffer + text, "Hello glue", 11);
    constexpr uint32_t ttb = 0x10000, coarse = 0xc000;
    put(ttb, coarse | 1);
    for (uint32_t i = 0; i < 0x20; ++i) put(coarse + 4 * i, (i << 12) | 0xff2);  // small pages, AP 3, identity

    auto* mem = memInit();
    ramSetNoFramebuffer(ramInit(mem, nullptr, 0, 0x20000, &backing, true));  // the stubs write RAM
    auto* patches = initPatchDispatch();
    ArmCpu* cpu = cpuInit(app, mem, ARM_MEMORY_SYSTEM_MMU, false, false, -1, 0, 0, patches, new PacePatch{},
                          createSystemState(), ArmCpuModel::arm920t, true);
    patchDispatchSetCpu(patches, cpu);
    cpuReset(cpu, app);
    mmuSetDomainCfg(cpuGetMMU(cpu), 3);
    mmuSetTTP(cpuGetMMU(cpu), ttb);
    cpuSetReg(cpu, ARM_REG_NUM_CPSR, 0xa00000df);
    cpuSetReg(cpu, 0, 0);
    cpuSetReg(cpu, 1, table);
    cpuSetReg(cpu, 2, 0x2300);
    cpuSetReg(cpu, 13, 0x8000);
    cpuSetReg(cpu, 14, 0x6000);
    cpuSetReg(cpu, 15, app);

    const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/frond-twglue-test.log";
    Imx1ReferenceTime time;
    time.ticks = 32768 * 3;
    FILE* out = std::fopen(path.c_str(), "w");
    assert(out);
    {
        TwGlueLog log(cpu, &time, out, physical, nullptr);
        for (int n = 0; n < 400; ++n) {
            const uint32_t pc = cpuGetRegisters(cpu)[15];
            if (pc == end) break;
            uint32_t instruction;
            std::memcpy(&instruction, backing.buffer + pc, 4);
            log.step({uint64_t(n), pc, instruction, 0, 0, 4});
            assert(cpuCycleAnyKind(cpu, 1) >= 1);
        }
        assert(cpuGetRegisters(cpu)[15] == end);
        // PilotMain: a jump to the entry with r0 set, then a call to Call68KFunc, both stepped by hand.
        cpuSetReg(cpu, 0, 1);
        cpuSetReg(cpu, 2, 0x2300);
        cpuSetReg(cpu, 14, 0x6000);
        log.step({1000, app, 0, 0, 0, 4});
        log.step({1001, 0x2300, 0, 0, 0, 4});

        // Hand-stepped calls: regs set, a step at the target, then a step at lr returns it.
        uint64_t seq = 2000;
        auto at = [&](uint32_t pc) { log.step({seq++, pc, 0, 0, 0, 4}); };
        auto regs = [&](uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
            const uint32_t v[4] = {r0, r1, r2, r3};
            for (uint32_t i = 0; i < 4; ++i) cpuSetReg(cpu, i, v[i]);
            cpuSetReg(cpu, 13, 0x8000);
            cpuSetReg(cpu, 14, 0x6000);
        };
        // 4000 ms: slot 5 entered, interrupted (ROM code), restarted, branches to itself, falls through and returns.
        time.ticks = 32768 * 4;
        regs(7, 0x3000, 0, 0);
        at(fn); at(0x20010000); at(0x20010004); at(fn); at(fn); at(fn + 4); at(0x6000);
        // 5000 ms: reaching slot 5's first instruction without a jump isn't a call.
        time.ticks = 32768 * 5;
        at(fn - 8); at(fn - 4); at(fn); at(fn + 4); at(0x6000);
        // 6000 ms: queue 4 with format [294 295 256], then 11 polls.
        time.ticks = 32768 * 6;
        put(0x3300, 294); put(0x3304, 295); put(0x3308, 256);
        regs(4, 0x3300, 12, 0);
        at(setFormat); at(0x6000);
        for (uint32_t n = 1; n <= 11; ++n) {
            // 1-8 logged as the first; 9 changes only 294/295 (skipped); 10 changes 256 (logged); 11 changes nothing.
            put(0x3400, n); put(0x3404, n * 2); put(0x3408, n == 10 ? 77 : 0);
            if (n == 11) { put(0x3400, 10); put(0x3404, 20); put(0x3408, 77); }
            regs(4, 0x3400, 12, 0);
            at(poll); at(0x6000);
        }
        // 7000 ms: VFSDirEntryEnumerate(1, &iter, &info), the second time with nothing changed.
        time.ticks = 32768 * 7;
        put(0x3700, 0x80);
        put(0x3710, 0x20); put(0x3714, 0x3720); put(0x3718, 32);
        std::memcpy(backing.buffer + 0x3720, "A.PRC", 6);
        regs(1, 0x3700, 0x3710, 0);
        at(dirEnumerate);
        put(0x3700, 0xffffffff);
        std::memcpy(backing.buffer + 0x3720, "B.DAT", 6);
        at(0x6000);
        time.ticks = 32768 * 8;
        regs(1, 0x3700, 0x3710, 0);
        at(dirEnumerate); at(0x6000);
        // 9000 ms: TwGfxBlendBlt(dst, &point, src, &rect, alpha).
        time.ticks = 32768 * 9;
        put(0x3800, 10); put(0x3804, uint32_t(-2));
        put(0x3810, 0); put(0x3814, 4); put(0x3818, 16); put(0x381c, 12);
        regs(0x3900, 0x3800, 0x3a00, 0x3810);
        at(blend); at(0x6000);
        // 10000 ms: TwGetDBDataDirectory with its string in the last 24 bytes of mapped memory and r3 = 64.
        time.ticks = 32768 * 10;
        std::memcpy(backing.buffer + 0x1ffe8, "/PALM/Programs/Foo-ABCD", 24);
        regs(0, 0x3100, 0x1ffe8, 64);
        at(dataDir); at(0x6000);
    }
    std::ifstream in(path);
    std::stringstream all;
    all << in.rdbuf();
    const std::string log = all.str();
    std::fputs(log.c_str(), stdout);
    std::fflush(stdout);
    auto has = [&](const char* s) { return log.find(s) != std::string::npos; };
    assert(has("3000 entry pc=00001000 r0=00000000 r1=00004000 r2=00002300 lr=00006000"));
    assert(has("3000 table 00004000 slots=612"));
    auto line = [&](const char* kind) {  // the first line holding kind
        const size_t at = log.find(kind);
        return at == std::string::npos ? std::string() : log.substr(at, log.find('\n', at) - at);
    };
    const std::string call = line("3000 call 5 "), ret = line("3000 ret 5 ");
    assert(call.find(" r0=00000007 r1=00003000 ") != std::string::npos && call.find(" r1=\"Hello glue\"") != std::string::npos);
    assert(ret.find("r0=0000002a") != std::string::npos);
    assert(line("3000 call DAL 2 ").size() && line("3000 ret DAL 2 ").find("r0=00000009") != std::string::npos);
    assert(line("3000 entry2 ").find("r0=00000001 r1=") != std::string::npos && line("3000 entry2 ").find(" r2=00002300 ") != std::string::npos);
    assert(line("3000 call 1000 Call68KFunc").size());
    assert(line("3000 call 333 TwInputSetFormat_").find(" features=[296 0 297 -1]") != std::string::npos);
    assert(line("3000 ret 336 TwInputPoll_").find(" n=1 buf=[296 -5]") != std::string::npos);
    assert(line("3000 call 241 VFSVolumeEnumerate_").find(" iter=00000000") != std::string::npos);
    assert(line("3000 ret 241 VFSVolumeEnumerate_").find(" iter=00000000->ffffffff vol=7->2") != std::string::npos);
    assert(line("3000 ret 472 TwGetDBDataDirectory_").find(" r1.u16=5 r2.buf=\"\" hex=4101") != std::string::npos);
    auto count = [&](const char* s) {
        size_t n = 0;
        for (size_t at = log.find(s); at != std::string::npos; at = log.find(s, at + 1)) ++n;
        return n;
    };
    assert(count("4000 call 5 ") == 1 && count("4000 ret 5 ") == 1);
    assert(count("5000 call ") == 0);
    assert(line("6000 call 333 ").find(" features=[294 295 256]") != std::string::npos);
    for (uint32_t n = 1; n <= 8; ++n) {
        const std::string want = " n=" + std::to_string(n) + " buf=[" + std::to_string(n) + " " + std::to_string(n * 2) + " 0]";
        assert(log.find(want) != std::string::npos);
    }
    assert(log.find(" n=9 ") == std::string::npos && log.find(" n=11 ") == std::string::npos);
    assert(log.find(" n=10 buf=[10 20 77]") != std::string::npos);
    assert(count("6000 ret 336 ") == 11);
    assert(line("7000 call 214 VFSDirEntryEnumerate_").find(" iter=00000080") != std::string::npos);
    assert(line("7000 ret 214 ").find(" iter=00000080->ffffffff attr=00000020 name=\"B.DAT\"") != std::string::npos);
    assert(line("7000 ret 214 ").find("unchanged") == std::string::npos);
    assert(line("8000 ret 214 ").find(" iter=ffffffff->ffffffff attr=00000020 name=\"B.DAT\" (info unchanged)") != std::string::npos);
    assert(line("9000 call 386 TwGfxBlendBlt_").find(" point=[10 -2] rect=[0 4 16 12]") != std::string::npos);
    assert(line("10000 ret 472 ").find(" r2.buf=\"/PALM/Programs/Foo-ABCD\"") != std::string::npos);
    std::remove(path.c_str());
    std::puts("ok");
    return 0;
}
