#ifndef _ROM_INFO5_H_
#define _ROM_INFO5_H_

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>

#include "device_type5.h"

struct DeviceFirmwareVariant5;

class RomInfo5 {
   public:
    RomInfo5() = default;
    RomInfo5(const void* buffer, size_t size);

    bool IsValid() const;

    const char* GetCardName() const;
    const char* GetManufacturer() const;
    const char* GetRomName() const;
    bool NeedsNand() const;
    bool NeedsNandPatch() const;
    bool SupportsNand() const;

    uint32_t GetCompanyId() const;
    uint32_t GetHalId() const;

    uint32_t GetRecommendedRamSize() const;

    // Byte offset of the parsed card header within the image.
    size_t GetHeaderOffset() const;

    DeviceType5 GetDeviceType() const;

    const DeviceFirmwareVariant5* GetFirmwareVariant() const;

    // True for a card at 0x0 plus a big card at 0x20000 with the same name and IDs,
    // whose size runs to the end of the image (the Zire 72 layout).
    bool HasSmallAndBigCards() const;

   private:
    bool Parse();
    bool ParseAt(size_t offset);

    bool IsDmitryPatched() const;
    bool IsZire72() const;

    uint32_t Read32(size_t offset, bool& error) const;
    std::string ReadString(size_t offset, uint8_t maxLen, bool& error) const;

   private:
    const uint8_t* buffer{nullptr};
    size_t size{0};

    bool isValid{false};

    std::string cardName;
    std::string manufacturer;
    std::string romName;

    uint32_t companyId{0};
    uint32_t halId{0};
    size_t headerOffset{0};
};

std::ostream& operator<<(std::ostream& stream, RomInfo5& romInfo);

#endif  // _ROM_INFO5_H_
