// The ARM cores' GDB remote stub: the debugger's bytes come from the input over a socket pair, the
// CPU and PACE are stand-ins with 64 KiB of memory at 0. The input's first 8 bytes seed the
// emulator side's choices (PACE mode, steps, breakpoints hit, PACE instruction results).
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <thread>

#include "../../core/src/uarm/uarm/gdbstub.cpp"

struct ArmCpu {};

namespace {
ArmCpu cpu;
uint32_t registers[16], cpsr = ARM_SR_MODE_SYS;
bool paceMode;
alignas(8) uint8_t memoryBytes[0x10000];
}  // namespace

void uarmAbort(void) { std::abort(); }
bool cpuIsThumb(ArmCpu*) { return cpsr & ARM_SR_T; }
bool cpuIsModePace(ArmCpu*) { return paceMode; }
PaceDebugState paceDebugState() { return {}; }
uint32_t* cpuGetRegisters(ArmCpu*) { return registers; }
bool cpuMemOpExternal(ArmCpu*, void* buf, uint32_t vaddr, uint_fast8_t size, bool write) {
    if (vaddr >= sizeof(memoryBytes) || sizeof(memoryBytes) - vaddr < size) return false;
    if (write) std::memcpy(memoryBytes + vaddr, buf, size);
    else std::memcpy(buf, memoryBytes + vaddr, size);
    return true;
}
void cpuSetCurInstrPC(ArmCpu*, uint32_t) {}
uint32_t cpuGetRegExternal(ArmCpu*, uint_fast8_t reg) { return reg == ARM_REG_NUM_CPSR ? cpsr : reg < 16 ? registers[reg] : 0; }
void paceGetMemeryFault(uint32_t* addr, bool* wasWrite, uint_fast8_t* fsr) { *addr = 0x1234, *wasWrite = true, *fsr = 5; }
void cpuInvalidateCodeCache(ArmCpu*) {}
bool cpuOriginalTg50PaceEntryForDiagnostic(ArmCpu*) { return paceMode; }
void cpuSetReg(ArmCpu*, uint_fast8_t reg, uint32_t val) {
    if (reg == ARM_REG_NUM_CPSR) cpsr = val;
    else if (reg < 16) registers[reg] = val;
}
uint32_t paceGetPC() { return registers[15]; }
std::string NativeBoundary::InspectUiApp(SoC&) { return "{\"available\":false}"; }

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    if (size < 8 || size > 65536) return 0;
    uint64_t seed = 0;
    for (int i = 0; i < 8; ++i) seed = seed << 8 | bytes[i];
    auto next = [&seed](unsigned range) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return unsigned((seed >> 33) % range);
    };
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 0;
    // The debugger side: the input, then end of stream. Replies are read and dropped on a thread.
    if (send(pair[1], bytes + 8, size - 8, MSG_NOSIGNAL) != ssize_t(size - 8)) std::abort();
    shutdown(pair[1], SHUT_WR);
    std::thread reader([fd = pair[1]] {
        char sink[4096];
        while (recv(fd, sink, sizeof(sink), 0) > 0) {}
    });
    std::fill(std::begin(registers), std::end(registers), 0);
    cpsr = ARM_SR_MODE_SYS;
    paceMode = false;
    stub s;
    s.cpu = &cpu;
    s.listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);  // never listens, so accept just fails
    s.sock = pair[0];
    s.state = RunState::Stopped;
    for (unsigned i = 0; i < 200000 && s.sock >= 0; ++i) {
        const uint32_t pc = next(8) == 0 ? next(0x10000) & ~1u : registers[15];
        switch (next(16)) {
            case 0: paceMode = !paceMode; break;
            case 1: gdbStubDebugBreakRequested(&s); break;
            case 2: gdbStubInstructionExecuted(&s); break;
            case 3: {
                static const unsigned statuses[] = {0, 4, 5, 10, 11, 32, 40, 47, 48, 49, 50};
                gdbStubPaceInstructionCompleted(&s, pc, statuses[next(11)]);
                break;
            }
            default: break;
        }
        gdbStubReportPc(&s, pc, cpsr & ARM_SR_T);
        registers[15] += 4;
    }
    disconnect(&s);
    close(s.listener);
    reader.join();
    close(pair[1]);
    return 0;
}
