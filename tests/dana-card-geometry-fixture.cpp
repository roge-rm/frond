#include "MachineFolder.h"
#include "device_profile5.h"
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) return 1;
    const uint64_t size = std::stoull(argv[2]);
    if (size != (32ull << 20) + 512 && size != (32ull << 20) + 16384) return 1;
    std::vector<uint8_t> payload(size, 0), file;
    if (!frondSdFile::encodeV2(payload, 1, file)) return 1;
    {
        std::ofstream out(argv[1], std::ios::binary);
        out.write(reinterpret_cast<const char*>(file.data()), file.size());
        out.close();
        if (!out) return 1;
    }
    frondSdFile::Info info;
    std::vector<uint32_t> table;
    if (frondMachine::readSdHeader(argv[1], info, &table) != frondMachine::SiblingStatus::ok ||
        frondMachine::checkSdPayload(argv[1], info, table) != frondMachine::SiblingStatus::ok ||
        info.version != 2 || info.payloadBytes != size) return 1;
    const bool geometry = deviceProfileCardSizeValid5(deviceTypeDana, DeviceCardSlot5::sd0,
                                                     DeviceCardFamily5::sdsc, size);
    if (geometry != (size == (32ull << 20) + 16384)) return 1;
    std::cout << "PASS: v2 header, table and payload valid; bytes=" << size
              << "; CSD representable=" << geometry << '\n';
}
