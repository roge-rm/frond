#ifndef FROND_ZODIAC_ROM_H
#define FROND_ZODIAC_ROM_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Makes the Zodiac's ROM from Tapwave's Zodiac 1.1 update (a zip holding a PRC, or the PRC): the
// PRC's ROMZ resource with id 2 is a gzip stream that unpacks to the ROM. Shared by the desktop
// and the Android app (as libfrondcard's JNI), and tools/zodiac-rom.py does the same.
namespace frondZodiacRom {
struct Expected {
    size_t length;
    const char* sha256;  // lower case hex
};
// The ROM Frond runs.
Expected zodiac();

struct Result {
    // notUpdate: not a zip or PRC (maybe the ROM itself); the caller carries on with the input as it is.
    enum class Status { ok, notUpdate, error } status = Status::notUpdate;
    std::vector<uint8_t> rom;  // when ok
    std::string error;         // when error, for the user
};

// Zip, PRC or an already made ROM. A ROM of the right length and hash comes back as ok, a file that is
// neither a zip nor a PRC as notUpdate. 'expected' is for tests.
Result extract(const uint8_t* data, size_t size, const Expected& expected = zodiac());
// The same from a file; read errors are errors.
Result extractFile(const std::string& path, const Expected& expected = zodiac());

std::string sha256Hex(const uint8_t* data, size_t size);
}  // namespace frondZodiacRom
#endif
