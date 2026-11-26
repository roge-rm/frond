// ROM image headers read when a firmware file is imported.
#include "rom_info5.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    RomInfo5 info(bytes, size);
    if (info.IsValid()) {
        info.GetCardName();
        info.GetRomName();
        info.GetDeviceType();
        info.GetFirmwareVariant();
        info.NeedsNand();
    }
    return 0;
}
