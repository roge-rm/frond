#include <algorithm>
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
    static_assert(int(DisplayMode::mode_320x320) == 5 && int(DisplayMode::mode_320x480) == 6);
    std::set<std::string> names;
    std::set<int> types;
    unsigned desktopCount = 0;
    const std::vector<std::string> reserved{"palm-zire-72", "sony-clie-tg50", "palm-iiix", "palm-m515"};
    for (const auto& p : deviceProfiles5) {
        // Names are unique, short and plain; they go into session headers.
        assert(names.insert(p.name).second && std::strlen(p.name) <= deviceNameMax5);
        for (const char* c = p.name; *c; ++c) assert((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-');
        assert(deviceProfileNamed5(p.name) == &p);
        if (p.deviceType == deviceTypeInvalid) {
            assert(p.support == DeviceSupport5::reserved && p.board == DeviceBoard5::none && p.art != DeviceArt::none);
            assert(std::find(reserved.begin(), reserved.end(), p.name) != reserved.end());
            assert(deviceTypeNamed5(p.name) == deviceTypeInvalid);
            continue;
        }
        // Built devices round-trip name -> device -> name.
        assert(types.insert(p.deviceType).second);
        assert(deviceProfile5(p.deviceType) == &p);
        assert(deviceTypeNamed5(p.name) == p.deviceType && std::string(deviceName5(deviceTypeNamed5(p.name))) == p.name);
        desktopCount += deviceProfileSupportsDesktop5(p.deviceType);
        assert(deviceProfileSupportsDesktop5(p.deviceType) == (p.deviceType == deviceTypeTX || p.deviceType == deviceTypeE2));
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
    assert(desktopCount == 2 && names.size() == 8 && types.size() == 4);
    assert(std::string(deviceName5(deviceTypeTX)) == "palm-tx" && std::string(deviceName5(deviceTypeE2)) == "palm-e2");
    assert(deviceProfileNamed5("palm-tx")->capabilities.serial && !deviceProfileNamed5("palm-e2")->capabilities.serial);
    // Unknown names are refused, including numbers, old ids, case and prefixes.
    for (const char* name : {"", "3", "0", "tx", "tungsten-e2", "franken-e2", "Palm-TX", "palm-tx ", "palm", "palm-tx2"}) {
        assert(!deviceProfileNamed5(name) && deviceTypeNamed5(name) == deviceTypeInvalid);
    }
    assert(!deviceProfileNamed5(nullptr) && !deviceName5(deviceTypeInvalid) && !deviceName5(99));
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
    // E2 NOR is an 8 MiB part with its card header after the 128 KiB boot block.
    assert(deviceProfileValidateFirmware5(0, 8u << 20, 34603008, false, true) == DeviceFirmwareError5::none);
    for (size_t nor : {size_t(4u << 20), size_t((8u << 20) - 1), size_t((8u << 20) + 1), size_t(16u << 20)})
        assert(deviceProfileValidateFirmware5(0, nor, 34603008, false, true) == DeviceFirmwareError5::norSize);
    assert(deviceProfileValidateNorLayout5(0, 0x20000, false) == DeviceFirmwareError5::none);
    for (size_t at : {size_t(0), size_t(0x8000), size_t(0x40000)})
        assert(deviceProfileValidateNorLayout5(0, at, false) == DeviceFirmwareError5::norLayout);
    for (int other : {1, 2, 3}) assert(deviceProfileValidateNorLayout5(other, 0, false) == DeviceFirmwareError5::none);
    // E2 sessions from builds before the size and layout checks still load; TX sessions don't.
    for (size_t nor : {size_t(4u << 20), size_t(16u << 20)})
        assert(deviceProfileValidateFirmware5(0, nor, 34603008, true, false) == DeviceFirmwareError5::none);
    assert(deviceProfileValidateFirmware5(0, 0, 34603008, true, false) == DeviceFirmwareError5::norSize);
    assert(deviceProfileValidateFirmware5(0, 8u << 20, 34603007, true, false) == DeviceFirmwareError5::nandSize);
    assert(deviceProfileValidateNorLayout5(0, 0, true) == DeviceFirmwareError5::none);
    {
        std::vector<uint8_t> nor(8u << 20);
        put32(nor, 0x20000 + 8, 0xfeedbeef);
        put32(nor, 0x20000 + 0x80, 0x50616c6d);
        put32(nor, 0x20000 + 0x84, 0x68737072);
        const RomInfo5 info(nor.data(), nor.size());
        assert(info.IsValid() && info.GetDeviceType() == deviceTypeE2 && info.GetHeaderOffset() == 0x20000);
    }

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

    // A session saved with a device name loads with that name.
    for (const auto& p : deviceProfiles5) for (bool sd : {false, true}) {
        if (p.deviceType == deviceTypeInvalid) continue;
        const uint8_t synthetic[] = {0, 1, 2, 3};
        std::vector<uint8_t> card(512);
        SessionFile5 source;
        source.SetDeviceName(p.name).SetDisplayMode(int(p.defaultDisplay))
            .SetRamSize(32u << 20).SetNor(sizeof(synthetic), synthetic)
            .SetMemory(sizeof(synthetic), synthetic).SetSavestate(sizeof(synthetic), synthetic);
        if (sd) source.SetSd(card.size(), card.data());
        assert(source.Serialize());
        const auto* bytes = static_cast<const uint8_t*>(source.GetSerializedSession());
        assert(std::string(reinterpret_cast<const char*>(bytes + 8)) == p.name);
        SessionFile5 decoded;
        assert(decoded.Deserialize(source.GetSerializedSessionSize(), source.GetSerializedSession()));
        assert(decoded.GetVersion() == 6u);
        assert(decoded.GetDeviceName() == p.name && deviceProfileNamed5(decoded.GetDeviceName().c_str()) == &p);
        assert(decoded.GetRamSize() == (32u << 20));
        assert(decoded.GetDisplayMode() == int(p.defaultDisplay));
        assert(decoded.GetMemorySize() == sizeof(synthetic));
        assert(std::memcmp(decoded.GetMemory(), synthetic, sizeof(synthetic)) == 0);
        assert(decoded.GetSdSize() == (sd ? 512u : 0u));
    }
    {
        // No name, or one that isn't plain, is never written.
        const uint8_t synthetic[] = {0};
        for (const char* name : {"", "Palm TX", "palm_tx", "0123456789012345678901234567890123"}) {
            SessionFile5 source;
            source.SetDeviceName(name).SetNor(sizeof(synthetic), synthetic);
            assert(!source.Serialize());
        }
        // Numbered sessions from before v6 are refused.
        for (uint32_t version : {0u, 1u, 4u, 5u}) {
            std::vector<uint8_t> old(64);
            put32(old, 0, 0x19800819); put32(old, 4, version); put32(old, 8, 3);
            SessionFile5 decoded;
            assert(SessionFile5::IsSessionFile(old.size(), old.data()));
            assert(!decoded.Deserialize(old.size(), old.data()));
        }
        // A name field with bytes after its NUL is refused.
        SessionFile5 source;
        source.SetDeviceName("palm-tx").SetNor(sizeof(synthetic), synthetic);
        assert(source.Serialize());
        std::vector<uint8_t> bytes(static_cast<const uint8_t*>(source.GetSerializedSession()),
                                   static_cast<const uint8_t*>(source.GetSerializedSession()) + source.GetSerializedSessionSize());
        SessionFile5 decoded;
        bytes[8 + 20] = 'x';
        assert(!decoded.Deserialize(bytes.size(), bytes.data()));
        bytes.resize(20);
        assert(!decoded.Deserialize(bytes.size(), bytes.data()));
    }
    std::cout << "PASS: device names, cohort inventory, geometry, E2 NOR size/layout, header bounds and v6 named sessions\n";
}
