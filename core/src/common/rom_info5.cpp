#include "rom_info5.h"

#include <cstring>
#include <iostream>
#include <string>

#include "device_profile5.h"

#pragma GCC diagnostic ignored "-Wmultichar"

using namespace std;

namespace {
    constexpr uint32_t HAL_ID_DMITRY_TUNGSTEN_E3 = 'DmGR';
    constexpr uint32_t HAL_ID_TUNGSTEN_E2 = 'hspr';
    constexpr uint32_t HAL_ID_PV = 'PvPv';
    constexpr uint32_t HAL_ID_ZIRE_72 = 'aZ72';
    constexpr size_t ZIRE_72_BIG_CARD = 0x20000;

    constexpr uint32_t SIGNATURE = 0xfeedbeef;

    std::string formatMultichar(uint32_t multichar) {
        char str[5] = "????";

        for (int8_t i = 3; i >= 0; i--) {
            char c = multichar & 0xff;
            multichar >>= 8;

            if (c >= 0x21 && c <= 0x7e) str[i] = c;
        }

        return str;
    }

    std::string describeDeviceType(DeviceType5 deviceType) {
        const auto* profile = deviceProfile5(deviceType);
        return profile ? std::string(profile->name) + " (" + profile->displayName + ")" : "unknown";
    }
}  // namespace

RomInfo5::RomInfo5(const void* buffer, size_t size)
    : buffer(static_cast<const uint8_t*>(buffer)), size(size) {
    isValid = Parse();
}

bool RomInfo5::IsValid() const { return isValid; }

const char* RomInfo5::GetCardName() const { return cardName.c_str(); }

const char* RomInfo5::GetManufacturer() const { return manufacturer.c_str(); }

const char* RomInfo5::GetRomName() const { return romName.c_str(); }

bool RomInfo5::IsZire72() const { return GetHalId() == HAL_ID_ZIRE_72; }

bool RomInfo5::IsDmitryPatched() const {
    return GetDeviceType() != deviceTypeTG50 && !IsZire72() && (GetCardName() != string("PalmCard") || GetHalId() != HAL_ID_TUNGSTEN_E2 ||
            GetHalId() == HAL_ID_PV);
}

bool RomInfo5::NeedsNand() const { return GetDeviceType() != deviceTypeTG50 && !IsZire72() && !IsDmitryPatched(); }

bool RomInfo5::NeedsNandPatch() const { return IsDmitryPatched(); }

bool RomInfo5::SupportsNand() const { return GetHalId() == HAL_ID_PV; }

uint32_t RomInfo5::GetCompanyId() const { return companyId; }

uint32_t RomInfo5::GetHalId() const { return halId; }

size_t RomInfo5::GetHeaderOffset() const { return headerOffset; }

uint32_t RomInfo5::GetRecommendedRamSize() const {
    if (GetDeviceType() == deviceTypeTG50) return 16u << 20;
    return IsDmitryPatched() || IsZire72() ? (32ul << 20) : (16ul << 20);
}

DeviceType5 RomInfo5::GetDeviceType() const {
    if (!isValid) return DeviceType5::deviceTypeInvalid;

    for (const auto& profile : deviceProfiles5)
        if (profile.detect == DeviceDetect5::cardHeader && profile.halId == halId &&
            (!profile.companyId || profile.companyId == companyId))
            return profile.deviceType;

    return DeviceType5::deviceTypeInvalid;
}

const DeviceFirmwareVariant5* RomInfo5::GetFirmwareVariant() const {
    if (!isValid) return nullptr;
    const auto info = deviceProfileInspectNor5(size, [&](uint64_t at, uint8_t* out, size_t count) {
        if (at > size || count > size - at) return false;
        std::memcpy(out, buffer + at, count);
        return true;
    });
    return info.variant;
}

bool RomInfo5::HasSmallAndBigCards() const {
    if (!isValid || headerOffset != 0) return false;

    bool error = false;
    const size_t big = ZIRE_72_BIG_CARD;
    if (Read32(big + 8, error) != SIGNATURE) return false;

    const std::string bigName = ReadString(big + 0x10, 32, error);
    const uint32_t bigCompany = Read32(big + 0x80, error);
    const uint32_t bigHal = Read32(big + 0x84, error);
    const uint32_t bigLength = Read32(big + 0x6c, error);

    return !error && cardName == "PalmCard" && bigName == cardName && bigCompany == companyId &&
           bigHal == halId && big + bigLength == size;
}

bool RomInfo5::Parse() {
    size_t offset = 0;
    bool signatureFound = false;
    bool error = false;

    for (offset = 0; offset + 12 <= size && !signatureFound && !error; offset += 32 * 1024) {
        uint32_t sig = Read32(offset + 8, error);
        signatureFound = sig == SIGNATURE;

        if (!signatureFound) continue;

        if (ParseAt(offset)) return true;
    }

    return false;
}

bool RomInfo5::ParseAt(size_t offset) {
    bool error = false;

    cardName = ReadString(offset + 0x10, 32, error);
    manufacturer = ReadString(offset + 0x30, 32, error);
    romName = ReadString(offset + 0x8c, 32, error);

    companyId = Read32(offset + 0x80, error);
    halId = Read32(offset + 0x84, error);
    headerOffset = offset;

    return !error;
}

uint32_t RomInfo5::Read32(size_t offset, bool& error) const {
    if (error) return 0;

    if (offset + 4 > size) {
        error = true;
        return 0;
    }

    return uint32_t(buffer[offset]) | (uint32_t(buffer[offset + 1]) << 8) |
           (uint32_t(buffer[offset + 2]) << 16) | (uint32_t(buffer[offset + 3]) << 24);
}

std::string RomInfo5::ReadString(size_t offset, uint8_t maxLen, bool& error) const {
    if (error || offset > size) {
        error = true;
        return "";
    }

    char str[256];
    bool terminated = false;

    for (size_t i = 0; i < maxLen && !terminated; i++) {
        if (i >= size - offset) {
            error = true;
            return "";
        }
        str[i] = buffer[offset + i];
        terminated = str[i] == '\0';
    }

    if (!terminated) {
        error = true;
        return "";
    }

    return str;
}

std::ostream& operator<<(std::ostream& stream, RomInfo5& info) {
    if (!info.IsValid()) {
        stream << "invalid ROM" << std::endl;
        return stream;
    }

    stream << "device: " << describeDeviceType(info.GetDeviceType()) << std::endl
           << "card name: " << info.GetCardName() << std::endl
           << "ROM version: " << info.GetRomName() << std::endl
           << "manufacturer: " << info.GetManufacturer() << std::endl
           << "HAL ID: " << formatMultichar(info.GetHalId()) << std::endl
           << "company ID: " << formatMultichar(info.GetCompanyId()) << std::endl;

    return stream;
}
