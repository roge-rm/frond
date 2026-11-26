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
        // Written again, the sections it read stay valid; kept to small ones.
        if (session.GetMemorySize() + session.GetNorSize() + session.GetNandSize() + session.GetSavestateSize() <
            (1u << 20)) {
            session.Serialize();
            const auto* memory = static_cast<const uint8_t*>(session.GetMemory());
            for (size_t i = 0; memory && i < session.GetMemorySize(); i += 64) (void)memory[i];
        }
    }
    bool ok = false;
    const size_t chunk = rle_get_chunk_size(size, bytes, ok);
    if (ok && chunk <= (1u << 20)) {
        std::vector<uint8_t> out(chunk);
        rle_decode_chunk(size, bytes, out.size(), out.data());
    }
    return 0;
}
