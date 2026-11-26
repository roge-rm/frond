// The 68k core's GDB remote stub: a loopback client sends the input, the Debugger is a stand-in
// with 64 KiB of memory at 0. The input's first 8 bytes seed the emulator side's choices (break
// states as the CPU would report them).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <thread>

#include "Debugger.h"
#include "GdbStub.h"

namespace {
uint64_t seed;
unsigned Next(unsigned range) {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    return unsigned((seed >> 33) % range);
}
uint8_t memoryBytes[0x10000];
}  // namespace

extern "C" void MyAssertFailed(const char* assertion, const char* file, unsigned int line) {
    std::fprintf(stderr, "EmAssert failed: %s at %s:%u\n", assertion, file, line);
    std::abort();
}

Debugger::BreakState Debugger::GetBreakState() const { return breakState; }
void Debugger::Reset() { breakState = BreakState::none, enabled = false; }
void Debugger::Enable() { enabled = true; }
Debugger::BreakMode Debugger::GetBreakMode() const { return breakMode; }
void Debugger::SetBreakpoint(emuptr) {}
void Debugger::ClearBreakpoint(emuptr) {}
void Debugger::SetWatchpoint(emuptr, WatchpointType, size_t) {}
void Debugger::ClearWatchpoint(emuptr, WatchpointType, size_t) {}
Debugger::WatchpointType Debugger::GetWatchpointType() const { return WatchpointType::write; }
emuptr Debugger::GetWatchpointAddress() const { return 0x1234; }
void Debugger::Interrupt() { breakState = BreakState::externalInterrupt; }
void Debugger::Continue() { breakState = BreakState::none; }
void Debugger::Step() { breakState = BreakState::none, stepping = true; }
uint8 Debugger::MemoryRead8(emuptr addr) { return memoryBytes[addr & 0xffff]; }
uint16 Debugger::MemoRead16(emuptr addr) { return uint16(MemoryRead8(addr) << 8 | MemoryRead8(addr + 1)); }
uint32 Debugger::MemoryRead32(emuptr addr) { return uint32(MemoRead16(addr)) << 16 | MemoRead16(addr + 2); }
void Debugger::MemoryWrite(emuptr addr, uint8* data, size_t len) {
    for (size_t i = 0; i < len; ++i) memoryBytes[(addr + i) & 0xffff] = data[i];
}
void Debugger::UpdateBreakState() { breakState = BreakState::none; }
const array<uint32, Debugger::REGISTER_COUNT>& Debugger::ReadRegisters() { return registers; }
// As the real one: indexes past PC are ignored.
void Debugger::SetRegister(size_t index, uint32 value) {
    if (index < REGISTER_COUNT) registers[index] = value;
}
void Debugger::SetBreakMode(BreakMode mode) { breakMode = mode; }
// Stands in for the CPU stopping the guest: the harness sets the break state through it.
void Debugger::NotifyTrap(uint16 state) { breakState = BreakState(state); }

namespace {
uint32 port;
// One stub for the process on a loopback port: the first one from a pid-based start that binds.
GdbStub& Stub(Debugger& debugger) {
    static GdbStub* stub = [&debugger] {
        for (port = 30000 + uint32(getpid()) % 20000;; port = port == 60000 ? 30000 : port + 1) {
            auto* s = new GdbStub(debugger, port);
            s->Listen();
            if (s->GetConnectionState() == GdbStub::ConnectionState::listening) return s;
            delete s;
        }
    }();
    return *stub;
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    if (size < 8 || size > 65536) return 0;
    seed = 0;
    for (int i = 0; i < 8; ++i) seed = seed << 8 | bytes[i];
    static Debugger debugger;
    auto& stub = Stub(debugger);
    debugger.SetBreakMode(Next(4) == 0 ? Debugger::BreakMode::appOnly : Debugger::BreakMode::all);
    const int client = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(uint16_t(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (client < 0 || connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address))) std::abort();
    for (int i = 0; i < 100 && !stub.IsDebuggerConnected(); ++i) stub.Cycle(100);
    if (!stub.IsDebuggerConnected()) std::abort();
    // The debugger side: the input, then end of stream. Replies are read and dropped on a thread.
    if (send(client, bytes + 8, size - 8, MSG_NOSIGNAL) != ssize_t(size - 8)) std::abort();
    shutdown(client, SHUT_WR);
    std::thread reader([client] {
        char sink[4096];
        while (recv(client, sink, sizeof(sink), 0) > 0) {}
    });
    for (unsigned i = 0; i < 100000 && stub.IsDebuggerConnected(); ++i) {
        // While the guest runs it now and then stops for a reason the CPU would give.
        if (stub.GetRunState() == GdbStub::RunState::running && Next(4) == 0)
            debugger.NotifyTrap(uint16(Next(7)));
        stub.Cycle(0);
    }
    if (stub.IsDebuggerConnected()) {
        // Not yet at the end of the input: hang up from the debugger side.
        shutdown(client, SHUT_RDWR);
        for (int i = 0; i < 100 && stub.IsDebuggerConnected(); ++i) stub.Cycle(10);
    }
    shutdown(client, SHUT_RDWR);
    reader.join();
    close(client);
    return 0;
}
