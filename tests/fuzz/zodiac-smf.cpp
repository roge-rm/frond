// The Zodiac's Standard MIDI File reader (games hand it songs from their own data): the length scan
// and the full parse. Every sysex or meta payload it reports must lie inside the file, as the
// player reads it from there.
#include "zodiac_smf.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    const size_t length = zodiac_smf::smfLength(bytes, size);
    if (length > size) __builtin_trap();
    zodiac_smf::Song song;
    if (zodiac_smf::parseSmf(bytes, size, song, 1u << 16) == zodiac_smf::Error::none)
        for (const auto& event : song.events)
            if ((event.status == 0xff || event.status == 0xf0 || event.status == 0xf7) &&
                (event.offset > size || event.length > size - event.offset))
                __builtin_trap();
    return 0;
}
