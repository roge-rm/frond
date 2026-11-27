// The savestate container every core loads (table of chunks, then 32-bit words) and the chunk
// reads the device loaders make. The input's last bytes pick up to 64 reads (chunk, kind, size);
// the rest is the savestate, given at an odd address now and then for the copy path.
#include <fuzzer/FuzzedDataProvider.h>

#include <cstring>

#include "savestate/ChunkHelper.h"
#include "savestate/SavestateLoader.h"

namespace {
struct Read {
    uint8_t chunk, kind;
    uint16_t size;
};

struct Target {
    std::vector<Read> reads;
    void Load(SavestateLoader<uint32_t>& loader) {
        std::vector<uint8_t> buffer(0x10000 * 8);
        for (const auto& r : reads) {
            const uint32_t type = r.chunk & 7;
            Chunk* chunk = (r.chunk & 8) ? loader.GetChunkOrFail(type, r.size, "fuzz") : loader.GetChunk(type);
            if (!chunk) {
                loader.HasChunk(type);
                continue;
            }
            LoadChunkHelper<Chunk> helper(*chunk);
            uint8_t v8;
            uint16_t v16;
            uint32_t v32;
            uint64_t v64;
            bool flag;
            double real;
            for (unsigned i = 0; i < 4; ++i) switch ((r.kind + i) % 14) {
                case 0: chunk->Get8(); break;
                case 1: chunk->Get16(); break;
                case 2: chunk->Get32(); break;
                case 3: chunk->Get64(); break;
                case 4: chunk->GetBool(), chunk->GetDouble(); break;
                case 5: chunk->GetBuffer(buffer.data(), r.size); break;
                case 6: chunk->GetBuffer16(reinterpret_cast<uint16_t*>(buffer.data()), r.size); break;
                case 7: chunk->GetBuffer32(reinterpret_cast<uint32_t*>(buffer.data()), r.size); break;
                case 8: chunk->GetBuffer64(reinterpret_cast<uint64_t*>(buffer.data()), r.size); break;
                case 12: {
                    std::string text;
                    chunk->GetString(r.size & 0xff);
                    helper.DoString(text, r.size >> 8);
                    break;
                }
                case 9: chunk->GetBool(); break;
                case 10: helper.Do8(v8).Do16(v16).Do32(v32).Do64(v64).DoBool(flag).DoDouble(real); break;
                case 11: helper.DoBuffer(buffer.data(), r.size); break;
                default: chunk->IsAtEnd(), chunk->HasError(); break;
            }
        }
    }
};
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    FuzzedDataProvider data(bytes, size);
    Target target;
    const unsigned count = data.ConsumeIntegralInRange(0, 64);
    for (unsigned i = 0; i < count; ++i)
        target.reads.push_back({data.ConsumeIntegral<uint8_t>(), data.ConsumeIntegral<uint8_t>(), data.ConsumeIntegral<uint16_t>()});
    const bool odd = data.ConsumeBool();
    const auto state = data.ConsumeRemainingBytes<uint8_t>();
    std::vector<uint8_t> copy(state.size() + 8);
    uint8_t* at = copy.data() + (odd ? 1 : 0);
    if (!state.empty()) std::memcpy(at, state.data(), state.size());
    SavestateLoader<uint32_t> loader;
    loader.Load(at, state.size(), target);
    return 0;
}
