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

static void palmIIIx();
static void originalMips();

int main() {
    static_assert(int(DisplayMode::mode_320x320) == 5 && int(DisplayMode::mode_320x480) == 6);
    std::set<std::string> names;
    std::set<int> types;
    unsigned desktopCount = 0;
    const std::vector<std::string> reserved{"palm-m515"};
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
        assert(deviceProfileSupportsDesktop5(p.deviceType) ==
               (p.deviceType == deviceTypeTX || p.deviceType == deviceTypeE2 || p.deviceType == deviceTypeZire72 ||
                p.deviceType == deviceTypeTG50 || p.deviceType == deviceTypeIIIx || p.deviceType == deviceTypeFrankenE2));
        assert((p.deviceType == deviceTypeZire72) == (p.board == DeviceBoard5::pxa270Zire72));
        assert(deviceConfigurationGetNandSize(p.deviceType) == p.nandSize);
        assert(deviceConfigurationDefaultDisplayMode(p.deviceType) == p.defaultDisplay);
        for (uint32_t ram : {0u, 1u, 4u << 20, 8u << 20, 16u << 20, 24u << 20, 32u << 20, 64u << 20, 1u << 31, UINT32_MAX}) {
            const bool expected = p.deviceType == deviceTypePV
                ? ram >= (8u << 20) && (ram & (ram - 1)) == 0
                : p.deviceType == deviceTypeTG50 ? ram == (16u << 20)
                : p.deviceType == deviceTypeIIIx ? ram == (4u << 20)
                : ram == (32u << 20) || (p.deviceType != deviceTypeTX && p.deviceType != deviceTypeZire72 && ram == (16u << 20));
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
    // The frontend reader recognises every card-header profile from the same public metadata.
    for (const auto& p : deviceProfiles5) {
        if (p.detect != DeviceDetect5::cardHeader) continue;
        std::vector<uint8_t> nor(std::max(size_t(0x40000), p.norSize));
        const size_t at = p.headerOffset;
        put32(nor, at + 8, 0xfeedbeef);
        put32(nor, at + 0x80, p.companyId);
        put32(nor, at + 0x84, p.halId);
        if (p.norLayout == DeviceNorLayout5::smallAndBigCards) {
            std::memcpy(nor.data() + at + 0x10, "PalmCard", 9);
            const auto big = p.bigCardOffset;
            std::memcpy(nor.data() + big + 0x10, "PalmCard", 9);
            put32(nor, big + 8, 0xfeedbeef);
            put32(nor, big + 0x80, p.companyId);
            put32(nor, big + 0x84, p.halId);
            put32(nor, big + 0x6c, nor.size() - big);
        }
        auto inspect = [&] {
            return deviceProfileInspectNor5(nor.size(), [&](uint64_t offset, uint8_t* out, size_t count) {
                if (offset > nor.size() || count > nor.size() - offset) return false;
                std::memcpy(out, nor.data() + offset, count);
                return true;
            });
        };
        assert(inspect().profile == &p && inspect().hasHeader && inspect().validLayout);
        if (p.norLayout == DeviceNorLayout5::smallAndBigCards) {
            nor[p.bigCardOffset + 0x84] ^= 1;
            assert(inspect().profile == &p && !inspect().validLayout);
        }
        nor.resize(at + 12);
        assert(!inspect().profile && !inspect().hasHeader);
    }
    assert(desktopCount == 6 && names.size() == 8 && types.size() == 7);
    assert(std::string(deviceName5(deviceTypeTX)) == "palm-tx" && std::string(deviceName5(deviceTypeE2)) == "palm-e2");
    assert(deviceProfileNamed5("palm-tx")->capabilities.serial && deviceProfileNamed5("palm-e2")->capabilities.serial);
    // Unknown names are refused, including numbers, old ids, case and prefixes.
    for (const char* name : {"", "3", "0", "tx", "tungsten-e2", "franken-e2", "Palm-TX", "palm-tx ", "palm", "palm-tx2"}) {
        assert(!deviceProfileNamed5(name) && deviceTypeNamed5(name) == deviceTypeInvalid);
    }
    assert(!deviceProfileNamed5(nullptr) && !deviceName5(deviceTypeInvalid) && !deviceName5(99));
    for (int id : {INT_MIN, -1, 7, 99, INT_MAX}) {
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
    for (int other : {2, 3}) assert(deviceProfileValidateNorLayout5(other, 0, false) == DeviceFirmwareError5::none);
    // The de-NVFS firmware keeps the E2 board and qualifies its alternate NOR by its full header.
    {
        const auto* p = deviceProfile5(deviceTypeE2);
        const auto* v = &deviceFirmwareVariants5[0];
        assert(v->deviceType == p->deviceType && std::string(v->id) == "e2-denvfs");
        assert(v->norSize == (6u << 20) && v->headerOffset == p->headerOffset && v->cardEndsAtEof);
        assert(std::string(v->fastAudioNorMd5)=="c2d90f775b1737de5bcd11634bf51d7d");
        assert(v->disableSleep);
        assert(v->fastAudioArmStartEntry==0x205bc180 && v->fastAudioArmStartCall==0x205bc200 &&
               v->fastAudioArmStartBranch==0xeb00543b);
        assert(deviceProfileFastAudioQualified5(*p,p->frontend.fastAudioNorMd5));
        assert(deviceProfileFastAudioQualified5(*p,v->fastAudioNorMd5,v));
        assert(!deviceProfileFastAudioQualified5(*p,v->fastAudioNorMd5));
        assert(!deviceProfileFastAudioQualified5(*p,"",v));
        assert(!deviceProfileFastAudioQualified5(*p,nullptr,v));
        assert(!deviceProfileFastAudioQualified5(*p,"c2d90f775b1737de5bcd11634bf51d70",v));
        assert(!deviceProfileFastAudioQualified5(*deviceProfile5(deviceTypeFrankenE2),v->fastAudioNorMd5,v));
        auto make = [&](size_t size, size_t at) {
            std::vector<uint8_t> nor(size);
            put32(nor, at + 8, 0xfeedbeef);
            std::memcpy(nor.data() + at + 0x10, v->cardName, std::strlen(v->cardName) + 1);
            std::memcpy(nor.data() + at + 0x30, v->manufacturer, std::strlen(v->manufacturer) + 1);
            put32(nor, at + 0x6c, size - at);
            put32(nor, at + 0x80, v->companyId);
            put32(nor, at + 0x84, v->halId);
            return nor;
        };
        auto inspect = [](const std::vector<uint8_t>& nor) {
            return deviceProfileInspectNor5(nor.size(), [&](uint64_t at, uint8_t* out, size_t count) {
                if (at > nor.size() || count > nor.size() - at) return false;
                std::memcpy(out, nor.data() + at, count);
                return true;
            });
        };
        auto good = make(v->norSize, v->headerOffset);
        assert(inspect(good).profile == p && inspect(good).variant == v && inspect(good).validLayout);
        const RomInfo5 info(good.data(), good.size());
        assert(info.IsValid() && info.GetDeviceType() == deviceTypeE2 && info.GetFirmwareVariant() == v);
        assert(!info.NeedsNand() && info.NeedsNandPatch() && info.GetRecommendedRamSize() == (32u << 20));
        assert(deviceProfileValidateFirmware5(p->deviceType, good.size(), p->nandSize, false, false, v) == DeviceFirmwareError5::none);
        assert(deviceProfileValidateFirmware5(p->deviceType, good.size(), p->nandSize, false, false) == DeviceFirmwareError5::norSize);
        assert(deviceProfileValidateFirmware5(deviceTypeFrankenE2, good.size(), p->nandSize, false, false, v) == DeviceFirmwareError5::norSize);
        auto forged = *v;
        assert(deviceProfileValidateFirmware5(p->deviceType, good.size(), p->nandSize, false, false, &forged) == DeviceFirmwareError5::norSize);
        assert(deviceProfileValidateFirmware5(p->deviceType, good.size(), p->nandSize + 1, false, true, v) == DeviceFirmwareError5::nandSize);
        for (size_t field : {size_t(0x10), size_t(0x16), size_t(0x30), size_t(0x39), size_t(0x80), size_t(0x84), size_t(0x6c)}) {
            auto bad = good;
            bad[v->headerOffset + field] ^= 1;
            assert(!inspect(bad).variant && !RomInfo5(bad.data(), bad.size()).GetFirmwareVariant());
        }
        for (size_t at : {size_t(0), size_t(0x8000), size_t(0x40000)}) {
            auto bad = make(v->norSize, at);
            assert(!inspect(bad).variant && !RomInfo5(bad.data(), bad.size()).GetFirmwareVariant());
        }
        for (size_t size : {v->norSize - 1, v->norSize + 1, p->norSize}) {
            auto bad = make(size, v->headerOffset);
            assert(!inspect(bad).variant && !RomInfo5(bad.data(), bad.size()).GetFirmwareVariant());
        }
        // Retail-size custom card names keep the existing geometry rules.
        auto custom = make(p->norSize, p->headerOffset);
        assert(inspect(custom).profile == p && inspect(custom).validLayout && !inspect(custom).variant);
        assert(deviceProfileValidateFirmware5(p->deviceType, custom.size(), p->nandSize, false, false) == DeviceFirmwareError5::none);
        for (size_t size : {size_t(6u << 20), size_t(8u << 20)})
            assert(deviceProfileNorSizeSupported5(p->deviceType, size));
        for (size_t size : {size_t(0), size_t(6u << 20) - 1, size_t(6u << 20) + 1})
            assert(!deviceProfileNorSizeSupported5(p->deviceType, size));
        assert(!deviceProfileNorSizeSupported5(deviceTypeFrankenE2, v->norSize));
        assert(!deviceProfileNorSizeSupported5(deviceTypeInvalid, v->norSize));
    }
    // E3 keeps its named-session identity and E2 board, with a full-height LCD.
    {
        const auto* p = deviceProfileNamed5("palm-e2-franken");
        assert(p && p->deviceType == deviceTypeFrankenE2 && p->support == DeviceSupport5::experimentalDesktop);
        assert(p->board == DeviceBoard5::pxa255E2 && p->art == DeviceArt::e2);
        assert(p->frontend.available && deviceProfileRunnerEligible5(p->deviceType));
        assert(p->support == DeviceSupport5::experimentalDesktop);
        assert(p->frontend.width == 320 && p->frontend.height == 480 && p->frontend.silkscreenHeight == 0);
        assert(p->frontend.stripHeight == 80 && deviceProfileStorageSlot5(*p) == DeviceStorageSlot5::sd);
        assert(!std::strstr(p->frontend.buttons, "power"));
        assert(std::strstr(deviceProfile5(deviceTypeE2)->frontend.buttons, "power"));
        const auto display = deviceConfigurationDisplayConfigForDevice(p->deviceType, p->defaultDisplay);
        assert(display.width == 320 && display.height == 480 && display.graffitiHeight == 0);
        const auto& caps = p->capabilities;
        assert(caps.keyboard && caps.serial && caps.usb && caps.infrared && caps.bluetooth && caps.sound);
        assert(caps.fastAudio && !caps.network && !caps.microphone && !caps.backgroundPanel);
        assert(!std::strcmp(p->frontend.fastAudioNorMd5, "a79f98bddd50c61e7eb89eee46eb81bd"));
        for (const auto& other : deviceProfiles5)
            assert(deviceProfileIsE2Board5(other.deviceType) ==
                   (other.deviceType == deviceTypeE2 || other.deviceType == deviceTypeFrankenE2));
        for (const auto& other : deviceProfiles5)
            assert(deviceProfileHasHotSyncButton5(other.deviceType) ==
                   (other.deviceType == deviceTypeTX || other.deviceType == deviceTypeE2 ||
                    other.deviceType == deviceTypeFrankenE2 || other.deviceType == deviceTypeIIIx));
        for (int unknown : {-1, 99}) {
            assert(!deviceProfileIsE2Board5(unknown) && !deviceProfileHasHotSyncButton5(unknown));
        }
        assert(p->norSize == (8u << 20) && p->headerOffset == 0x20000 && p->legacySessionNor);
        for (size_t size : {size_t(256), size_t(4u << 20), size_t((8u << 20) - 1), size_t((8u << 20) + 1), size_t(16u << 20)}) {
            assert(deviceProfileValidateFirmware5(p->deviceType, size, p->nandSize, false, false) == DeviceFirmwareError5::norSize);
            assert(deviceProfileValidateFirmware5(p->deviceType, size, p->nandSize, true, false) == DeviceFirmwareError5::none);
        }
        assert(deviceProfileValidateFirmware5(p->deviceType, 0, p->nandSize, true, false) == DeviceFirmwareError5::norSize);
        assert(deviceProfileValidateFirmware5(p->deviceType, p->norSize, p->nandSize + 1, true, false) == DeviceFirmwareError5::nandSize);
        for (size_t at : {size_t(0), size_t(0x8000), size_t(0x40000)}) {
            assert(deviceProfileValidateNorLayout5(p->deviceType, at, false) == DeviceFirmwareError5::norLayout);
            assert(deviceProfileValidateNorLayout5(p->deviceType, at, true) == DeviceFirmwareError5::none);
        }
        std::vector<uint8_t> nor(p->norSize);
        auto inspect = [&] {
            return deviceProfileInspectNor5(nor.size(), [&](uint64_t at, uint8_t* out, size_t count) {
                if (at > nor.size() || count > nor.size() - at) return false;
                std::memcpy(out, nor.data() + at, count);
                return true;
            });
        };
        for (size_t at : {size_t(0), size_t(0x20000), size_t(0x40000)}) {
            std::fill(nor.begin(), nor.end(), 0);
            put32(nor, at + 8, 0xfeedbeef);
            put32(nor, at + 0x80, p->companyId);
            put32(nor, at + 0x84, p->halId);
            const RomInfo5 info(nor.data(), nor.size());
            assert(info.IsValid() && info.GetDeviceType() == p->deviceType && info.GetHeaderOffset() == at);
            assert(info.GetRecommendedRamSize() == (32u << 20));
            assert(inspect().profile == p && inspect().validLayout == (at == 0x20000));
            put32(nor, at + 0x80, 0x58585858);
            assert(inspect().hasHeader && !inspect().profile);
            assert(RomInfo5(nor.data(), nor.size()).GetDeviceType() == deviceTypeInvalid);
        }
    }
    // Zire 72 NOR: small card at 0x0 and a matching big card at 0x20000, sessions included.
    assert(deviceProfileValidateFirmware5(deviceTypeZire72, 14942208, 0, false, false) == DeviceFirmwareError5::none);
    for (size_t nor : {size_t(14942207), size_t(14876672), size_t(8u << 20), size_t(16u << 20), size_t(0x20000)})
        assert(deviceProfileValidateFirmware5(deviceTypeZire72, nor, 0, false, false) == DeviceFirmwareError5::norSize);
    assert(deviceProfileValidateFirmware5(deviceTypeZire72, 14942208, 1, false, true) == DeviceFirmwareError5::nandSize);
    assert(deviceProfileValidateNorLayout5(deviceTypeZire72, 0, false, true) == DeviceFirmwareError5::none);
    assert(deviceProfileValidateNorLayout5(deviceTypeZire72, 0, true, true) == DeviceFirmwareError5::none);
    for (bool session : {false, true}) {
        assert(deviceProfileValidateNorLayout5(deviceTypeZire72, 0, session, false) == DeviceFirmwareError5::norLayout);
        for (size_t at : {size_t(0x8000), size_t(0x20000)})
            assert(deviceProfileValidateNorLayout5(deviceTypeZire72, at, session, true) == DeviceFirmwareError5::norLayout);
    }
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

    // Zire 72 layout from synthetic cards: small card at 0x0, big card at 0x20000 to the end.
    {
        constexpr uint32_t palm = 0x50616c6d, aZ72 = 0x615a3732, hspr = 0x68737072;
        constexpr size_t zireSize = 14942208;
        auto card = [](std::vector<uint8_t>& nor, size_t at, uint32_t hal, size_t length) {
            put32(nor, at + 8, 0xfeedbeef);
            std::memcpy(nor.data() + at + 0x10, "PalmCard", 9);
            put32(nor, at + 0x6c, length);
            put32(nor, at + 0x80, palm);
            put32(nor, at + 0x84, hal);
            std::memcpy(nor.data() + at + 0x8c, "Synthetic", 10);
        };
        auto zire = [&](uint32_t smallHal, uint32_t bigHal, size_t bigLength) {
            std::vector<uint8_t> nor(zireSize);
            card(nor, 0, smallHal, 0x10000);
            card(nor, 0x20000, bigHal, bigLength);
            return nor;
        };
        auto check = [](const std::vector<uint8_t>& nor, DeviceType5 type, bool layout) {
            const RomInfo5 info(nor.data(), nor.size());
            assert(info.IsValid() && info.GetDeviceType() == type && info.HasSmallAndBigCards() == layout);
            return info.GetHeaderOffset();
        };
        auto good = zire(aZ72, aZ72, zireSize - 0x20000);
        assert(check(good, deviceTypeZire72, true) == 0);
        {
            const RomInfo5 info(good.data(), good.size());
            assert(!info.NeedsNand() && !info.NeedsNandPatch() && info.GetRecommendedRamSize() == (32u << 20));
        }
        // Cards that disagree, a wrong big card size, or a damaged small card are not the layout.
        assert(check(zire(aZ72, hspr, zireSize - 0x20000), deviceTypeZire72, false) == 0);
        assert(check(zire(aZ72, 0x12345678, zireSize - 0x20000), deviceTypeZire72, false) == 0);
        assert(check(zire(aZ72, aZ72, zireSize - 0x20000 - 1), deviceTypeZire72, false) == 0);
        assert(check(zire(aZ72, aZ72, zireSize - 0x20000 + 1), deviceTypeZire72, false) == 0);
        assert(check(zire(0x12345678, aZ72, zireSize - 0x20000), deviceTypeInvalid, false) == 0);
        auto noSmall = good;
        put32(noSmall, 8, 0);
        assert(check(noSmall, deviceTypeZire72, false) == 0x20000);
        auto otherName = good;
        std::memcpy(otherName.data() + 0x20010, "OtherCard", 10);
        assert(check(otherName, deviceTypeZire72, false) == 0);
        auto otherCompany = good;
        put32(otherCompany, 0x20080, 0x58585858);
        assert(check(otherCompany, deviceTypeZire72, false) == 0);
        // Both cards changed to the E2 or an unknown HAL: not a Zire, and no Zire layout.
        assert(check(zire(hspr, hspr, zireSize - 0x20000), deviceTypeE2, true) == 0);
        assert(check(zire(0x12345678, 0x12345678, zireSize - 0x20000), deviceTypeInvalid, true) == 0);
        // The E2 layout (small card at 0x60, big at 0x20000) is not the Zire's.
        std::vector<uint8_t> e2(8u << 20);
        card(e2, 0x20000, hspr, (8u << 20) - 0x20000);
        assert(check(e2, deviceTypeE2, false) == 0x20000);
        // A session holding another device's NOR is for a different device than the ROM says.
        assert(RomInfo5(e2.data(), e2.size()).GetDeviceType() != deviceTypeZire72);
        assert(RomInfo5(good.data(), good.size()).GetDeviceType() != deviceTypeE2);
        assert(!RomInfo5(good.data(), 0).IsValid());
        assert(!RomInfo5(good.data(), 0x20000).HasSmallAndBigCards());
        const auto* zire72 = deviceProfileNamed5("palm-zire-72");
        assert(zire72->deviceType == deviceTypeZire72);
        const auto& caps = zire72->capabilities;
        assert(caps.keyboard && caps.usb && caps.infrared && caps.bluetooth);
        assert(!caps.serial && !caps.network && caps.fastAudio);
    }

    for (const auto& p : deviceProfiles5)
        assert(p.capabilities.microphone == (p.deviceType == deviceTypeTG50 || p.deviceType == deviceTypeZire72));
    for (const auto& p : deviceProfiles5) assert(p.capabilities.backgroundPanel == (p.deviceType == deviceTypeTG50));
    for (const auto& p : deviceProfiles5) assert(p.capabilities.modem == (p.deviceType == deviceTypeIIIx));
    for (const auto& p : deviceProfiles5)
        assert(p.capabilities.irPhone == (p.deviceType == deviceTypeIIIx || p.deviceType == deviceTypeTX ||
                                          p.deviceType == deviceTypeTG50 || p.deviceType == deviceTypeE2 ||
                                          p.deviceType == deviceTypeFrankenE2 || p.deviceType == deviceTypeZire72));
    // TG50 has NOR-only firmware, physical 16 MiB RAM and a qualified native board.
    {
        const auto* p = deviceProfileNamed5("sony-clie-tg50");
        assert(p && p->deviceType == deviceTypeTG50 && p->support == DeviceSupport5::experimentalDesktop);
        auto tg = deviceConfigurationDisplayConfigForDevice(p->deviceType, p->defaultDisplay);
        assert(tg.width == 320 && tg.height == 320 && tg.graffitiHeight == 0);
        assert(deviceConfigurationDisplayConfigForDevice(deviceTypeE2, DisplayMode::mode_320x320).graffitiHeight == 120);
        assert(deviceConfigurationDisplayConfigForDevice(deviceTypePV, DisplayMode::mode_320x320).graffitiHeight == 120);
        assert(p->art == DeviceArt::tg50 && p->board == DeviceBoard5::pxa250SonyTG50);
        assert(p->frontend.available && deviceProfileRunnerEligible5(p->deviceType));
        assert(p->frontend.sideTabWidth == 38 && p->frontend.stripHeight == 251 && p->frontend.holdPresent && p->frontend.holdSupported);
        assert(p->frontend.homeKey == 1073741887 && p->frontend.homeX == -1 && p->frontend.homeY == -1);
        assert(deviceProfileStorageSlot5(*p) == DeviceStorageSlot5::memoryStick);
        assert(p->frontend.memoryStick && !p->frontend.sdSlot && !p->frontend.camera && p->capabilities.sound);
        const auto& caps = p->capabilities;
        assert(!caps.network && caps.keyboard && caps.serial && caps.microphone && caps.usb &&
               !caps.fastAudio && caps.infrared && caps.bluetooth);
        assert(!deviceProfileSdSizeValid5(p->deviceType, 512));
        std::vector<uint8_t> nor(p->norSize);
        for (size_t at : {size_t(0), size_t(0x20000)}) {
            put32(nor, at + 8, 0xfeedbeef);
            std::memcpy(nor.data() + at + 0x10, "PalmCard", 9);
            put32(nor, at + 0x80, p->companyId);
            put32(nor, at + 0x84, p->halId);
            put32(nor, at + 0x6c, nor.size() - at);
        }
        const RomInfo5 info(nor.data(), nor.size());
        assert(info.IsValid() && info.GetDeviceType() == deviceTypeTG50 && info.HasSmallAndBigCards());
        assert(!info.NeedsNand() && !info.NeedsNandPatch() && !info.SupportsNand());
        assert(info.GetRecommendedRamSize() == (16u << 20));
        for (bool session : {false, true}) {
            for (size_t size : {size_t(0xc6ffff), size_t(0xc70001), size_t(16u << 20)})
                assert(deviceProfileValidateFirmware5(p->deviceType, size, 0, session, false) == DeviceFirmwareError5::norSize);
            assert(deviceProfileValidateFirmware5(p->deviceType, p->norSize, 1, session, true) == DeviceFirmwareError5::nandSize);
            assert(deviceProfileValidateNorLayout5(p->deviceType, 0x20000, session, true) == DeviceFirmwareError5::norLayout);
            assert(deviceProfileValidateNorLayout5(p->deviceType, 0, session, false) == DeviceFirmwareError5::norLayout);
        }
        // Unknown Sony identities and Sony/Palm HAL mismatches cannot select the E2.
        for (uint32_t hal : {0x12345678u, 0x68737072u, 0x615a3732u}) {
            put32(nor, 0x84, hal);
            assert(RomInfo5(nor.data(), nor.size()).GetDeviceType() == deviceTypeInvalid);
        }
        for (int type : {deviceTypeE2, deviceTypeTX, deviceTypeZire72, deviceTypeFrankenE2, deviceTypePV})
            assert(deviceProfileRunnerEligible5(type));
        for (const char* name : {"palm-iiix", "palm-m515"})
            assert(!deviceProfileRunnerEligible5(deviceProfileNamed5(name)->deviceType));
    }

    palmIIIx();
    originalMips();

    // Public header layout and IDs from RomInfo5, no dump data.
    for (auto hal : {0x68737072u, 0x446d4752u, 0x50765076u, 0x615a3732u, 0x12345678u}) {
        std::vector<uint8_t> header(256);
        put32(header, 8, 0xfeedbeef);
        put32(header, 0x80, 0x50616c6d); // Palm
        put32(header, 0x84, hal);
        std::memcpy(header.data() + 0x8c, "Synthetic", 10);
        const DeviceType5 expected = hal == 0x68737072 ? deviceTypeE2 : hal == 0x446d4752 ? deviceTypeFrankenE2
            : hal == 0x50765076 ? deviceTypePV : hal == 0x615a3732 ? deviceTypeZire72 : deviceTypeInvalid;
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
        assert(source.SerializeAs(6));
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
    {
        const uint64_t MiB = 1u << 20;
        for (auto type : {deviceTypeTX, deviceTypeZire72}) {
            assert(deviceProfileSdSizeValid5(type, 32 * MiB) && deviceProfileSdSizeValid5(type, 2048 * MiB));
            assert(!deviceProfileSdSizeValid5(type, 2048 * MiB + 512) && !deviceProfileSdSizeValid5(type, 4096 * MiB));
            assert(!deviceProfileSdSizeValid5(type, 16 * MiB));
        }
        for (auto type : {deviceTypeE2, deviceTypeFrankenE2}) {
            assert(deviceProfileSdSizeValid5(type, 512) && deviceProfileSdSizeValid5(type, 1024 * MiB));
            assert(!deviceProfileSdSizeValid5(type, 0) && !deviceProfileSdSizeValid5(type, 513));
            assert(!deviceProfileSdSizeValid5(type, 1024 * MiB + 512) && !deviceProfileSdSizeValid5(type, 2048 * MiB));
        }
    }
    std::cout << "PASS: device names, cohort inventory, geometry, E2-board NOR size/layout, header bounds and v6 named sessions\n";
}

// A synthetic 68k ROM: a big-endian card header at 0, the rest zero.
static std::vector<uint8_t> rom68k(size_t size, uint16_t version, uint16_t flags, uint32_t company, uint32_t hal,
                                   uint32_t reset, uint32_t cardBytes) {
    std::vector<uint8_t> rom(size);
    auto be = [&](size_t at, uint32_t value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) rom[at + i] = uint8_t(value >> (8 * (bytes - 1 - i)));
    };
    if (size < 0x100) return rom;
    be(0, 0x3000, 4);
    be(4, reset, 4);
    be(8, 0xfeedbeef, 4);
    be(0x0c, version, 2);
    be(0x0e, flags, 2);
    std::memcpy(rom.data() + 0x10, "PalmCard", 9);
    std::memcpy(rom.data() + 0x30, "Synthetic", 10);
    be(0x6c, cardBytes, 4);
    if (version >= 5) be(0x7e, company, 4), be(0x82, hal, 4);
    return rom;
}

static Device68kRomInfo5 inspect68k(const std::vector<uint8_t>& rom) {
    return deviceProfileInspect68kRom5(rom.size(), [&](uint64_t at, uint8_t* out, size_t count) {
        if (at > rom.size() || count > rom.size() - at) return false;
        std::memcpy(out, rom.data() + at, count);
        return true;
    });
}

// Original MIPS: 150 on the 312 MHz PXA270s, scaled by clock for the 200 MHz PXA25x devices, 100 for the rest.
static void originalMips() {
    for (const auto& p : deviceProfiles5) {
        const bool pxa270 = p.deviceType == deviceTypeTX || p.deviceType == deviceTypeZire72;
        const bool pxa25x = p.deviceType == deviceTypeE2 || p.deviceType == deviceTypeFrankenE2 || p.deviceType == deviceTypeTG50;
        assert(deviceProfileOriginalMips5(p.deviceType) == (pxa270 ? 150u : pxa25x ? 96u : 100u));
    }
    assert(deviceProfileOriginalMips5(deviceTypeInvalid) == 100 && deviceProfileOriginalMips5(deviceTypeIIIx) == 100);
}

// Palm IIIx: an ez328 board for frond-68k on the desktop, told from its ROM's big-endian card header alone.
static void palmIIIx() {
    const auto* p = deviceProfileNamed5("palm-iiix");
    assert(p && p->deviceType == deviceTypeIIIx && deviceProfile5(deviceTypeIIIx) == p);
    assert(p->board == DeviceBoard5::ez328 && p->detect == DeviceDetect5::romHeader68k && p->art == DeviceArt::iiix);
    assert(p->support == DeviceSupport5::experimentalDesktop && p->frontend.available &&
           deviceProfileSupportsDesktop5(p->deviceType));
    assert(deviceProfileRuns68k5(p->deviceType) && !deviceProfileRunnerEligible5(p->deviceType));
    for (const auto& other : deviceProfiles5)
        if (&other != p) assert(!deviceProfileRuns68k5(other.deviceType));
    assert(p->cpuHz == 16580608);
    assert(deviceProfileSupportsRam5(p->deviceType, 4u << 20) && !deviceProfileSupportsRam5(p->deviceType, 8u << 20));
    assert(p->defaultDisplay == DisplayMode::mode_160x160 && p->displayModes == (1u << 1));
    const auto display = deviceConfigurationDisplayConfigForDevice(p->deviceType, p->defaultDisplay);
    assert(display.width == 160 && display.height == 160 && display.graffitiHeight == 60);
    assert(p->frontend.width == 160 && p->frontend.height == 160 && p->frontend.silkscreenHeight == 60);
    assert(std::string(p->frontend.appNames) == "Date Book|Address|To Do List|Memo Pad");
    assert(std::string(p->frontend.buttons) == "up|down|app1|app2|app3|app4|power");
    assert(!p->nandSize && !p->norSize && !deviceProfileSdSizeValid5(p->deviceType, 512));
    assert(deviceProfileStorageSlot5(*p) == DeviceStorageSlot5::none && !p->msSizes && !p->msProMaxMib);
    const auto& caps = p->capabilities;
    // Only the hardware a IIIx has: the piezo, the cradle serial port and IrDA.
    assert(!caps.network && !caps.keyboard && caps.serial && !caps.usb && !caps.fastAudio && caps.infrared &&
           !caps.bluetooth && caps.sound && !caps.microphone && !caps.backgroundPanel);
    for (const auto& other : deviceProfiles5)
        if (&other != p) assert(other.detect != DeviceDetect5::romHeader68k && other.board != DeviceBoard5::ez328);

    constexpr uint32_t base = 0x10c00000, palm = 0x70616c6d, sumo = 0x73756d6f, ez = 0x0020;
    // The shapes of the IIIx 3.1, generic OS 3.5 EZ and IIIx/V 4.1 ROMs.
    const auto v31 = rom68k(0x128000, 3, ez, 0, 0, base + 0x822a, 0x128000);
    const auto v35 = rom68k(0x178000, 4, ez, 0, 0, base + 0x90e0, 0x178000);
    const auto v41 = rom68k(0x1a8000, 5, ez, palm, sumo, base + 0xc6ea, 0x1a8000);
    for (const auto* rom : {&v31, &v35, &v41}) {
        const auto info = inspect68k(*rom);
        assert(info.profile == p && info.hasHeader);
        // Not an ARM card header, so the ARM readers don't take it.
        assert(RomInfo5(rom->data(), rom->size()).GetDeviceType() == deviceTypeInvalid);
        const auto arm = deviceProfileInspectNor5(rom->size(), [&](uint64_t at, uint8_t* out, size_t count) {
            if (at > rom->size() || count > rom->size() - at) return false;
            std::memcpy(out, rom->data() + at, count);
            return true;
        });
        assert(!arm.profile && !arm.hasHeader);
    }
    assert(inspect68k(v31).headerVersion == 3 && !inspect68k(v31).companyId);
    assert(inspect68k(v41).companyId == palm && inspect68k(v41).halId == sumo);
    // A ROM padded to the full 2 MiB part, or with a card smaller than the file, still reads.
    assert(inspect68k(rom68k(2u << 20, 3, ez, 0, 0, base + 0x822a, 0x4000)).profile == p);

    auto refused = [&](const std::vector<uint8_t>& rom, bool header) {
        const auto info = inspect68k(rom);
        assert(!info.profile && info.hasHeader == header);
    };
    // Wrong size: over 2 MiB, not whole 4 KiB pages, or shorter than its card says (truncated).
    refused(rom68k((2u << 20) + 0x1000, 3, ez, 0, 0, base + 0x822a, 0x128000), true);
    refused(rom68k(0x128001, 3, ez, 0, 0, base + 0x822a, 0x128000), true);
    refused(rom68k(0x100000, 3, ez, 0, 0, base + 0x822a, 0x128000), true);
    {
        auto truncated = v31;
        truncated.resize(0x127000);
        refused(truncated, true);
    }
    // Truncated inside the header, or empty.
    for (size_t size : {size_t(0), size_t(12), size_t(0xff)}) {
        auto truncated = v31;
        truncated.resize(size);
        refused(truncated, false);
    }
    // A read that fails is no ROM at all.
    assert(!deviceProfileInspect68kRom5(v31.size(), [](uint64_t, uint8_t*, size_t) { return false; }).hasHeader);
    // A little-endian ARM card header (the E2's layout at 0) isn't a 68k ROM.
    {
        std::vector<uint8_t> arm(0x128000);
        put32(arm, 8, 0xfeedbeef);
        std::memcpy(arm.data() + 0x10, "PalmCard", 9);
        put32(arm, 0x80, deviceCompanyPalm5);
        put32(arm, 0x84, 0x68737072);
        refused(arm, false);
    }
    // 68328-only, RAM-only, header versions 2 and 6, a reset vector outside the ROM, other 4.1 IDs.
    refused(rom68k(0x128000, 3, 0x0010, 0, 0, base + 0x822a, 0x128000), true);
    refused(rom68k(0x128000, 3, ez | 0x0001, 0, 0, base + 0x822a, 0x128000), true);
    refused(rom68k(0x128000, 2, ez, 0, 0, base + 0x822a, 0x128000), true);
    refused(rom68k(0x128000, 6, ez, palm, sumo, base + 0x822a, 0x128000), true);
    refused(rom68k(0x128000, 3, ez, 0, 0, 0x00c0822a, 0x128000), true);
    refused(rom68k(0x128000, 3, ez, 0, 0, base + 0x128000, 0x128000), true);
    refused(rom68k(0x1a8000, 5, ez, palm, 0x6173746e /* astn */, base + 0xc6ea, 0x1a8000), true);
    refused(rom68k(0x1a8000, 5, ez, 0, 0, base + 0xc6ea, 0x1a8000), true);
    {
        auto wrongName = v31;
        wrongName[0x10] = 'X';
        refused(wrongName, false);
    }
}
