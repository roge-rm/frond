// The ActiveSync (RNDIS dccm) control handshake and device information record. Its parser is
// file local, so the source is built into this target.
#include "FuzzStream.h"
#include "sync/ActiveSync.cpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    FuzzedDataProvider data(bytes, size);
    const auto rapiBytes = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange(0, 8));
    FuzzedDataProvider rapiData(rapiBytes.data(), rapiBytes.size());
    FuzzStream control(data), rapi(rapiData);
    try {
        frondSync::Connect(control, rapi);
    } catch (const std::runtime_error&) {
    }
    return 0;
}
