// The 68k GDB stub answers a '?' packet while nothing has stopped, as a stub that halted at the start of
// the program (it used to assert). A debugger on loopback talks to the stub with a stand-in Debugger.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "Debugger.h"
#include "GdbStub.h"

namespace {
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


int main() {
    Debugger debugger;
    GdbStub* stub = nullptr;
    uint32 port = 30000 + uint32(getpid()) % 20000;
    for (;; port = port == 60000 ? 30000 : port + 1) {
        stub = new GdbStub(debugger, port);
        stub->Listen();
        if (stub->GetConnectionState() == GdbStub::ConnectionState::listening) break;
        delete stub;
    }
    const int client = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(uint16_t(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (client < 0 || connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address))) return 1;
    for (int i = 0; i < 100 && !stub->IsDebuggerConnected(); ++i) stub->Cycle(100);
    if (!stub->IsDebuggerConnected()) return 1;

    debugger.Continue();  // connecting interrupted the guest; here it has not stopped
    // The packet is '?', checksum 3f; the guest has not stopped.
    const char packet[] = "$?#3f";
    if (send(client, packet, sizeof(packet) - 1, MSG_NOSIGNAL) != ssize_t(sizeof(packet) - 1)) return 1;
    std::string reply;
    for (int i = 0; i < 100 && reply.find("#b8") == std::string::npos; ++i) {
        stub->Cycle(10);
        char buffer[256];
        const ssize_t n = recv(client, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n > 0) reply.append(buffer, size_t(n));
    }
    if (reply.find("$S05#b8") == std::string::npos) {
        std::fprintf(stderr, "gdbstub-68k-stop: FAIL, reply '%s'\n", reply.c_str());
        return 1;
    }
    close(client);
    std::puts("gdbstub-68k-stop: ok");
    return 0;
}
