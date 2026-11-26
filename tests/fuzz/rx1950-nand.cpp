// rx1950 firmware import: the CEBL/CEOS NBF pair built into a NAND image, and the user store
// position read from an MBR.
#include <fuzzer/FuzzedDataProvider.h>

#include "rx1950_nand_image.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    if (size >= 512) rx1950UserStoreBlockFromMbr(bytes);
    if (size >= 528) rx1950UserStoreBlockFromPage(bytes);
    FuzzedDataProvider data(bytes, size);
    const auto eboot = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(0, size));
    const auto nbf = data.ConsumeRemainingBytes<uint8_t>();
    std::vector<uint8_t> flash;
    std::string error;
    rx1950NandImage(eboot.data(), eboot.size(), nbf.data(), nbf.size(), flash, error);
    return 0;
}
