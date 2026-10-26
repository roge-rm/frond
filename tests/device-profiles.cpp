#include <cassert>
#include <cstring>
#include <climits>
#include <iostream>
#include <set>
#include <string>
#include <vector>
#include "device_profile5.h"
#include "device_configuration.h"
#include "rom_info5.h"
#include "session/session_file5.h"

static void put32(std::vector<uint8_t>& data, size_t at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) data[at + i] = value >> (i * 8);
}

int main() {
    static_assert(deviceTypeE2 == 0 && deviceTypeFrankenE2 == 1 && deviceTypePV == 2 && deviceTypeTX == 3);
    static_assert(int(DisplayMode::mode_320x320) == 5 && int(DisplayMode::mode_320x480) == 6);
    std::set<std::string> ids;
    std::set<int> types;
    unsigned desktopCount = 0;
    for (const auto& p : deviceProfiles5) {
        assert(ids.insert(p.id).second && types.insert(p.deviceType).second);
        assert(deviceProfile5(p.deviceType) == &p);
        desktopCount += deviceProfileSupportsDesktop5(p.deviceType);
        assert(deviceProfileSupportsDesktop5(p.deviceType) == (p.deviceType == deviceTypeTX));
        assert(deviceConfigurationGetNandSize(p.deviceType) == p.nandSize);
        assert(deviceConfigurationDefaultDisplayMode(p.deviceType) == p.defaultDisplay);
        for (uint32_t ram : {0u, 1u, 8u << 20, 16u << 20, 24u << 20, 32u << 20, 64u << 20, 1u << 31, UINT32_MAX}) {
            const bool expected = p.deviceType == deviceTypePV
                ? ram >= (8u << 20) && (ram & (ram - 1)) == 0
                : ram == (32u << 20) || (p.deviceType != deviceTypeTX && ram == (16u << 20));
            assert(deviceConfigurationSupportsRamSize(p.deviceType, ram) == expected);
        }
        for (int mode : {INT_MIN, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, INT_MAX}) {
            const bool expected = mode >= 1 && mode <= 8 &&
                (p.deviceType == deviceTypePV || mode == int(p.defaultDisplay));
            assert(deviceConfigurationDeviceSupportsDisplayMode(p.deviceType, DisplayMode(mode)) == expected);
        }
        const size_t nor = p.norSize ? p.norSize : 256;
        assert(deviceProfileValidateFirmware5(p.deviceType, nor, p.nandSize, false, true) == DeviceFirmwareError5::none);
        assert(deviceProfileValidateFirmware5(p.deviceType, 0, p.nandSize, false, true) == DeviceFirmwareError5::norSize);
        assert(deviceProfileValidateFirmware5(p.deviceType, nor, p.nandSize + 1, true, false) == DeviceFirmwareError5::nandSize);
    }
    assert(desktopCount == 1);
    for (int id : {INT_MIN, -1, 4, 99, INT_MAX}) {
        assert(!deviceProfile5(id) && !deviceProfileSupportsDesktop5(id));
        assert(!deviceProfileSupportsRam5(id, 32u << 20));
        assert(!deviceProfileSupportsDisplay5(id, 6));
        assert(deviceProfileValidateFirmware5(id, 1, 0, true, false) == DeviceFirmwareError5::unknownDevice);
    }
    assert(deviceProfileValidateFirmware5(3, 16u << 20, 138412032, false, false) == DeviceFirmwareError5::explicitNand);
    assert(deviceProfileValidateFirmware5(3, 16u << 20, 138412032, true, false) == DeviceFirmwareError5::none);
    assert(deviceProfileValidateFirmware5(3, (16u << 20) - 1, 138412032, true, false) == DeviceFirmwareError5::norSize);
    assert(deviceProfileValidateFirmware5(3, 16u << 20, 128u << 20, false, true) == DeviceFirmwareError5::nandSize);

    // Public header layout and IDs from RomInfo5, no dump data.
    for (auto hal : {0x68737072u, 0x446d4752u, 0x50765076u, 0x12345678u}) {
        std::vector<uint8_t> header(256);
        put32(header, 8, 0xfeedbeef);
        put32(header, 0x80, 0x50616c6d); // Palm
        put32(header, 0x84, hal);
        std::memcpy(header.data() + 0x8c, "Synthetic", 10);
        const DeviceType5 expected = hal == 0x68737072 ? deviceTypeE2 : hal == 0x446d4752 ? deviceTypeFrankenE2
            : hal == 0x50765076 ? deviceTypePV : deviceTypeInvalid;
        for (size_t size = 0; size <= header.size(); ++size) {
            const std::vector<uint8_t> truncated(header.begin(), header.begin() + size);
            RomInfo5 info(truncated.data(), truncated.size());
            assert(info.IsValid() == (size >= 0x8c + 10));
            assert(info.GetDeviceType() == (info.IsValid() ? expected : deviceTypeInvalid));
        }
        std::memset(header.data() + 0x8c, 'x', 32);
        assert(!RomInfo5(header.data(), header.size()).IsValid());
    }

    // The 0.2.0 v4/v5 envelope keeps the same device and display IDs.
    for (int type : {0, 1, 2, 3}) for (bool sd : {false, true}) {
        const uint8_t synthetic[] = {0, 1, 2, 3};
        std::vector<uint8_t> card(512);
        SessionFile5 source;
        source.SetDeviceType(type).SetDisplayMode(int(deviceProfile5(type)->defaultDisplay))
            .SetRamSize(32u << 20).SetNor(sizeof(synthetic), synthetic)
            .SetMemory(sizeof(synthetic), synthetic).SetSavestate(sizeof(synthetic), synthetic);
        if (sd) source.SetSd(card.size(), card.data());
        assert(source.Serialize());
        SessionFile5 decoded;
        assert(decoded.Deserialize(source.GetSerializedSessionSize(), source.GetSerializedSession()));
        assert(decoded.GetVersion() == (sd ? 5u : 4u));
        assert(decoded.GetDeviceType() == type && decoded.GetRamSize() == (32u << 20));
        assert(decoded.GetDisplayMode() == int(deviceProfile5(type)->defaultDisplay));
        assert(decoded.GetMemorySize() == sizeof(synthetic));
        assert(std::memcmp(decoded.GetMemory(), synthetic, sizeof(synthetic)) == 0);
        assert(decoded.GetSdSize() == (sd ? 512u : 0u));
    }
    std::cout << "PASS: native cohort inventory, geometry, header bounds and v4/v5 session IDs\n";
}
