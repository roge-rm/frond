#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "device_profile5.h"
#include "imx1_boot_probe.h"

static void trace(void*, const Imx1ProbeTrace& t) {
    if (t.instructionSize == 2) {
        std::printf("step=%u pc=%08x instruction=%04x cpsr=%08x operand=%08x width=2\n",
                    t.step, t.pc, t.instruction, t.cpsr, t.operand);
        return;
    }
    std::printf("step=%u pc=%08x instruction=%08x cpsr=%08x operand=%08x width=4\n",
                t.step, t.pc, t.instruction, t.cpsr, t.operand);
}
static void bus(void*, const Imx1ProbeAccess& t) {
    const char* name = Imx1Bootstrap::registerName(t.address);
    if (!name && t.address >= 0x08000000 && t.address < 0x10000000) name = "SDRAM";
    std::printf("bus step=%u pc=%08x operation=%s address=%08x size=%u value=%08x register=%s status=%s\n",
                t.step, t.pc, t.write ? "write" : "read", t.address, t.size, t.value,
                name ? name : "unknown", imx1BootstrapAccessName(t.status));
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s ROM [physical|reset|linked] [instruction-budget] [exact|fast] [--bootstrap-peripherals] [--assume-mpll-settled] [--assume-spll-settled] [--gpio-a-input=VALUE] [--gpio-d-input=VALUE] [--ram-mib=32|128]\n", argv[0]);
        return 1;
    }
    const char* engine = argc >= 5 ? argv[4] : "exact";
    if (std::strcmp(engine, "exact") && std::strcmp(engine, "fast")) {
        std::fprintf(stderr, "Engine must be exact or fast\n"); return 1;
    }
    Imx1ProbeOptions options{};
    options.busObserver = bus;
    for (int i = 5; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--bootstrap-peripherals")) options.bootstrapPeripherals = true;
        else if (!std::strcmp(argv[i], "--assume-spll-settled")) options.assumeSpllSettled = true;
        else if (!std::strcmp(argv[i], "--assume-mpll-settled")) options.assumeMpllSettled = true;
        else if (!std::strncmp(argv[i], "--gpio-a-input=", 15) ||
                 !std::strncmp(argv[i], "--gpio-d-input=", 15)) {
            const bool portA = argv[i][7] == 'a';
            char* end = nullptr;
            errno = 0;
            const auto value = std::strtoul(argv[i] + 15, &end, 0);
            if (errno || argv[i][15] < '0' || argv[i][15] > '9' || *end || value > UINT32_MAX) {
                std::fprintf(stderr, "GPIO-%c input must be a 32-bit value\n", portA ? 'A' : 'D'); return 1;
            }
            (portA ? options.gpioAInputKnown : options.gpioDInputKnown) = true;
            (portA ? options.gpioAInput : options.gpioDInput) = value;
        } else if (!std::strcmp(argv[i], "--ram-mib=32")) options.ramMiB = 32;
        else if (!std::strcmp(argv[i], "--ram-mib=128")) options.ramMiB = 128;
        else { std::fprintf(stderr, "Unknown diagnostic option: %s\n", argv[i]); return 1; }
    }
    if ((options.gpioAInputKnown || options.gpioDInputKnown || options.ramMiB) && !options.bootstrapPeripherals) {
        std::fprintf(stderr, "External-input scenarios require --bootstrap-peripherals\n"); return 1;
    }
    if (options.assumeMpllSettled && !options.bootstrapPeripherals) {
        std::fprintf(stderr, "MPLL scenario requires --bootstrap-peripherals\n"); return 1;
    }
    if (options.assumeSpllSettled && !options.bootstrapPeripherals) {
        std::fprintf(stderr, "SPLL scenario requires --bootstrap-peripherals\n"); return 1;
    }
    unsetenv("FROND_JIT");
    const char* name = argc >= 3 ? argv[2] : "physical";
    Imx1ProbeEntry entry;
    if (!std::strcmp(name, "physical")) entry = Imx1ProbeEntry::physicalFlash;
    else if (!std::strcmp(name, "reset")) entry = Imx1ProbeEntry::resetAlias;
    else if (!std::strcmp(name, "linked")) entry = Imx1ProbeEntry::linkedAlias;
    else { std::fprintf(stderr, "Unknown entry mapping: %s\n", name); return 1; }
    unsigned long budget = 100000;
    if (argc >= 4) {
        char* end = nullptr;
        errno = 0;
        budget = std::strtoul(argv[3], &end, 10);
        if (errno || !*argv[3] || *end || !budget || budget > 10000000) {
            std::fprintf(stderr, "Instruction budget must be 1..10000000\n");
            return 1;
        }
    }
    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    if (!input || input.tellg() != 0x7d0000) {
        std::fprintf(stderr, "Expected an exact 0x7d0000-byte Zodiac ROM\n");
        return 1;
    }
    input.seekg(0);
    std::vector<uint8_t> rom(0x7d0000);
    if (!input.read(reinterpret_cast<char*>(rom.data()), rom.size())) {
        std::fprintf(stderr, "Cannot read ROM\n"); return 1;
    }
    const auto identity = deviceProfileInspectNor5(rom.size(), [&](uint64_t offset, void* data, size_t size) {
        if (offset > rom.size() || size > rom.size() - offset) return false;
        std::memcpy(data, rom.data() + offset, size); return true;
    });
    if (!identity.profile || identity.profile->deviceType != deviceTypeZodiac2 || !identity.validLayout) {
        std::fprintf(stderr, "ROM does not match the staged Zodiac family profile\n"); return 1;
    }
    std::printf("Experimental i.MX1 SmallROM probe; entry=%s budget=%lu engine=%s\n", name, budget, engine);
    std::puts("Physical flash=10000000; VINITHI low assumed.");
    if (!options.bootstrapPeripherals) std::puts("No RAM or MMIO model.");
    else {
        std::puts("Documented bootstrap register subset enabled; unsupported operations stop.");
        std::printf("MPLL settled assumption=%s; nominal FCLK only after completed restart; no lock claim.\n",
                    options.assumeMpllSettled ? "enabled" : "disabled");
        std::printf("SPLL settled assumption=%s; initial nominal output/LF=1; restart holds output high for 2 CLK32 then activates target/LF=1 with phase zero; bus waits for that event without retiring instructions.\n",
                    options.assumeSpllSettled ? "enabled" : "disabled");
        if (options.gpioAInputKnown)
            std::printf("Hypothetical static full-port GPIO-A input=%08x\n", options.gpioAInput);
        else std::puts("External GPIO-A levels are unknown.");
        if (options.gpioDInputKnown)
            std::printf("Hypothetical static full-port GPIO-D input=%08x\n", options.gpioDInput);
        else std::puts("External GPIO-D levels are unknown.");
        std::printf("Diagnostic RAM capacity=%u MiB, initial contents zero; bus timing unqualified.\n", options.ramMiB);
    }
    if (entry != Imx1ProbeEntry::physicalFlash) std::puts("Selected alias is an unverified mapping experiment.");
    const auto result = imx1BootProbe(rom.data(), rom.size(), entry, budget, trace, nullptr,
                                     !std::strcmp(engine, "fast"), &options);
    std::printf("stop=%s step=%u pc=%08x instruction=%0*x address=%08x size=%u value=%08x operand=%08x width=%u\n",
                imx1ProbeStopName(result.stop), result.trace.step, result.trace.pc,
                int(result.trace.instructionSize * 2), result.trace.instruction, result.address, result.size, result.value,
                result.trace.operand, result.trace.instructionSize);
    std::printf("bus-accesses=%llu peripheral-status=%s\n", (unsigned long long)result.busAccesses,
                imx1BootstrapAccessName(result.peripheralStatus));
    std::printf("jit-blocks=%llu jit-instructions=%llu final-cpsr=%08x\n",
                (unsigned long long)result.jitBlocks, (unsigned long long)result.jitInstructions, result.cpsr);
    for (unsigned i = 0; i < 16; ++i)
        std::printf("r%u=%08x%c", i, result.registers[i], i % 4 == 3 ? '\n' : ' ');
    std::printf("cpsr=%08x spsr=%08x\n", result.cpsr, result.spsr);
    return 2;  // A diagnostic stop never establishes a successful boot.
}
