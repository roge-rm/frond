// The TwGlue log: a small ARM program is run one instruction at a time with the log watching, and the file it
// writes is checked for the entry, a table call with a string argument, its return, and an r9 module call.
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
    std::memcpy(backing.buffer + text, "Hello glue", 11);

    auto* mem = memInit();
    ramInit(mem, nullptr, 0, 0x20000, &backing, true);
    auto* patches = initPatchDispatch();
    ArmCpu* cpu = cpuInit(app, mem, ARM_MEMORY_SYSTEM_MMU, false, false, -1, 0, 0, patches, new PacePatch{},
                          createSystemState(), ArmCpuModel::arm920t, true);
    patchDispatchSetCpu(patches, cpu);
    cpuReset(cpu, app);
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
        TwGlueLog log(cpu, &time, out);
        for (int n = 0; n < 200; ++n) {
            const uint32_t pc = cpuGetRegisters(cpu)[15];
            if (pc == end) break;
            uint32_t instruction;
            std::memcpy(&instruction, backing.buffer + pc, 4);
            log.step({uint64_t(n), pc, instruction, 0, 0, 4});
            assert(cpuCycleAnyKind(cpu, 1) >= 1);
        }
        assert(cpuGetRegisters(cpu)[15] == end);
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
    assert(has("3000 call 5 ") && has("r0=00000007 r1=00003000") && has("r1=\"Hello glue\""));
    assert(has("3000 ret 5 ") && has("r0=0000002a"));
    assert(has("3000 call DAL 2 ") && has("3000 ret DAL 2 r0=00000009"));
    std::remove(path.c_str());
    std::puts("ok");
    return 0;
}
