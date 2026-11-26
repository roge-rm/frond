// The ARM devices' NetLib stand-in (NetworkSimulation.cpp) and its host sockets (HostNetwork.cpp):
// Palm NetLib traps with stack arguments and guest memory from the input, in each network mode.
// The CPU and PACE are stand-ins with 64 KiB of memory at 0. Internet mode opens real sockets, so
// this runs only in its own network namespace (unshare -rn), and names resolve only as localhost.
#include <fuzzer/FuzzedDataProvider.h>
#include <netdb.h>

#include <cstring>

#include "../../core/src/uarm/uarm/NetworkSimulation.cpp"
#include "Isolated.h"

struct ArmCpu {};

namespace {
ArmCpu cpu;
uint32_t dregs[8], aregs[8], pc = 0x1000;
alignas(8) uint8_t memoryBytes[0x10000];
}  // namespace

bool cpuMemOpExternal(ArmCpu*, void* buf, uint32_t vaddr, uint_fast8_t size, bool write) {
    if (vaddr >= sizeof(memoryBytes) || sizeof(memoryBytes) - vaddr < size) return false;
    if (write) std::memcpy(memoryBytes + vaddr, buf, size);
    else std::memcpy(buf, memoryBytes + vaddr, size);
    return true;
}
void cpuInvalidateCodeCache(ArmCpu*) {}
uint32_t paceGetPC() { return pc; }
void paceSetPC(uint32_t value) { pc = value; }
void paceSetDreg(uint8_t index, uint32_t value) { dregs[index & 7] = value; }
uint32_t paceGetAreg(uint8_t index) { return aregs[index & 7]; }
void paceSetAreg(uint8_t index, uint32_t value) { aregs[index & 7] = value; }

// Guest host names never reach a real resolver.
extern "C" int getaddrinfo(const char* name, const char*, const addrinfo*, addrinfo** out) {
    *out = nullptr;
    if (!name || std::strcmp(name, "localhost")) return EAI_NONAME;
    auto* answer = static_cast<addrinfo*>(calloc(1, sizeof(addrinfo) + sizeof(sockaddr_in)));
    auto* address = reinterpret_cast<sockaddr_in*>(answer + 1);
    address->sin_family = AF_INET;
    address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    answer->ai_family = AF_INET;
    answer->ai_addr = reinterpret_cast<sockaddr*>(address);
    answer->ai_addrlen = sizeof(sockaddr_in);
    *out = answer;
    return 0;
}
extern "C" void freeaddrinfo(addrinfo* list) { free(list); }

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    RequireIsolatedNetwork();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    static const uint16_t traps[] = {0xa0ba, 0xa0b8, 0xa0b9, 0xa800, 0xa801, 0xa802, 0xa803, 0xa804, 0xa805, 0xa806,
                                     0xa807, 0xa808, 0xa809, 0xa80a, 0xa80b, 0xa80c, 0xa80f, 0xa811, 0xa813, 0xa815,
                                     0xa81b, 0xa81c, 0xa820, 0xa823, 0xa824, 0xa829, 0xa82e, 0xa833, 0xa83b};
    static const char* const modes[] = {"disabled", "connected", "timeout", "disconnected", "internet", "wireless"};
    if (size > 65536) return 0;
    FuzzedDataProvider data(bytes, size);
    std::memset(memoryBytes, 0, sizeof(memoryBytes));
    std::memset(dregs, 0, sizeof(dregs));
    NetworkSimulation net;
    net.SetMode(modes[data.ConsumeIntegralInRange(0, 5)]);
    std::unique_ptr<NetworkSimulation::Checkpoint> checkpoint;
    for (unsigned calls = 0; calls < 64 && data.remaining_bytes(); ++calls) {
        switch (data.ConsumeIntegralInRange(0, 7)) {
            case 0: {
                // Bytes into guest memory: buffers, addresses, names.
                const auto at = data.ConsumeIntegralInRange<uint32_t>(0, sizeof(memoryBytes) - 1);
                const auto blob = data.ConsumeBytes<uint8_t>(std::min<size_t>(sizeof(memoryBytes) - at, 600));
                if (!blob.empty()) std::memcpy(memoryBytes + at, blob.data(), blob.size());
                break;
            }
            case 1:
                net.SetMode(modes[data.ConsumeIntegralInRange(0, 5)]);
                break;
            case 2:
                if (checkpoint) checkpoint.reset();
                else checkpoint = std::make_unique<NetworkSimulation::Checkpoint>(net);
                break;
            default: {
                // A trap: SP picks the stack, its first word is the library reference (mostly ours).
                const uint32_t sp = data.ConsumeBool() ? 0x8000 : data.ConsumeIntegralInRange<uint32_t>(0, 0xffff);
                aregs[7] = sp;
                pc = data.ConsumeBool() ? 0x1000 : data.ConsumeIntegral<uint32_t>();
                const auto args = data.ConsumeBytes<uint8_t>(32);
                if (sp < sizeof(memoryBytes) && !args.empty()) std::memcpy(memoryBytes + sp, args.data(), std::min(args.size(), sizeof(memoryBytes) - sp));
                if (data.ConsumeBool() && sp + 2 <= sizeof(memoryBytes))
                    memoryBytes[sp] = NetworkSimulation::refnum >> 8, memoryBytes[sp + 1] = NetworkSimulation::refnum & 0xff;
                const uint16_t trap = data.ConsumeBool() ? traps[data.ConsumeIntegralInRange<size_t>(0, std::size(traps) - 1)]
                                                         : data.ConsumeIntegral<uint16_t>();
                net.Handle(&cpu, trap);
                break;
            }
        }
    }
    checkpoint.reset();
    return 0;
}
