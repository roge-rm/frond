#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "imx1_boot_probe.h"

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    assert(argc == 2);
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(input)), {});
    assert(!rom.empty());
    Imx1ProbeOptions options;
    options.bootstrapPeripherals = true;
    options.aspUSampleKnown = true;
    options.aspUSample = 0x9b00;
    options.aspPenInputs.contactKnown = options.aspPenInputs.contactDown = true;
    options.aspPenInputs.normalKnown = options.aspPenInputs.referenceKnown = true;
    options.aspPenInputs.normal = {0x1100, 0x5500, 0xa900, 0x1234};
    options.aspPenInputs.reference = {0x0180, 0x3280, 0x7100};
    uint32_t lastPointer = ~0u;
    options.busContext = &lastPointer;
    options.busObserver = [](void* context, const Imx1ProbeAccess& event) {
        if (!event.write && event.address == 0x00215034) {
            auto& previous = *static_cast<uint32_t*>(context);
            if (previous != (event.value & 15) || event.pc == 0x20096cf8)
                std::printf("PTR pc=%08x write-pointer=%u\n", event.pc, event.value & 15);
            previous = event.value & 15;
        }
        if (event.address == 0x00215000 ||
            (event.write && event.address >= 0x00215010 && event.address <= 0x00215034))
            std::printf("ASP pc=%08x address=%08x write=%u value=%08x status=%u\n",
                        event.pc, event.address, event.write, event.value, unsigned(event.status));
    };
    const auto result = imx1BootProbe(rom.data(), rom.size(), Imx1ProbeEntry::linkedAlias,
                                    8000000, nullptr, nullptr, false, &options);
    std::printf("stop=%s pc=%08x status=%u address=%08x value=%08x\n", imx1ProbeStopName(result.stop), result.trace.pc,
                unsigned(result.peripheralStatus), result.address, result.value);
    for (unsigned r = 0; r < 16; ++r) std::printf("r%u=%08x\n", r, result.registers[r]);
    assert(result.stop == Imx1ProbeStop::budget);
    assert(result.registers[7] == 3 && result.registers[8] == 101 && result.registers[10] == 226);
    assert(result.registers[11] == 0x01300088 && result.registers[5] == 0x01300088 &&
           result.registers[4] == 0x01300088);
}
