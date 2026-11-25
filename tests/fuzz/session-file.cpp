// Frond session files (.frond) and the RLE chunks of their memory images.
#include <vector>

#include "session/rle.h"
#include "session/session_file5.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    SessionFile5::IsSessionFile(size, bytes);
    SessionFile5 session;
    if (session.Deserialize(size, bytes)) {
        session.GetDeviceName();
        session.GetMediaCount();
    }
    bool ok = false;
    const size_t chunk = rle_get_chunk_size(size, bytes, ok);
    if (ok && chunk <= (1u << 20)) {
        std::vector<uint8_t> out(chunk);
        rle_decode_chunk(size, bytes, out.size(), out.data());
    }
    return 0;
}
