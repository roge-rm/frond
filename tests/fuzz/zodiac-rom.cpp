// The Zodiac ROM import: Tapwave's 1.1 update as a zip, the PRC inside it or the PRC alone, its
// ROMZ resource and the gzip stream in that. Half the runs expect a small made-up ROM (64 zero
// bytes) so a whole import can succeed; the rest expect the real one.
#include "ZodiacRom.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    if (!size) return 0;
    static const std::vector<uint8_t> zeros(64);
    static const std::string zerosHash = frondZodiacRom::sha256Hex(zeros.data(), zeros.size());
    const frondZodiacRom::Expected small{zeros.size(), zerosHash.c_str()};
    const auto result = frondZodiacRom::extract(bytes + 1, size - 1, (bytes[0] & 1) ? small : frondZodiacRom::zodiac());
    if (result.status == frondZodiacRom::Result::Status::ok && result.rom.size() != ((bytes[0] & 1) ? 64 : 8192000)) __builtin_trap();
    return 0;
}
