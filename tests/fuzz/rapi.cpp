// RAPI 2 replies from the device (SynCE's buffer and file calls behind FrondRapi).
#include <fuzzer/FuzzedDataProvider.h>

#include <algorithm>

#include "sync/SynceRapi.h"

namespace {
struct Input {
    FuzzedDataProvider* data;
    bool closed{false};
};
bool Read(void* context, void* out, size_t size, int) {
    auto& input = *static_cast<Input*>(context);
    if (input.closed || input.data->remaining_bytes() < size) return false;
    const auto bytes = input.data->ConsumeBytes<uint8_t>(size);
    std::copy(bytes.begin(), bytes.end(), static_cast<uint8_t*>(out));
    return true;
}
bool Write(void* context, const void*, size_t) { return !static_cast<Input*>(context)->closed; }
void Close(void* context) { static_cast<Input*>(context)->closed = true; }
bool Entry(void*, const uint16_t*, size_t, uint32_t, uint64_t) { return true; }
const uint16_t path[] = {'\\', 'a', '.', 'c', 'a', 'b', 0};
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    FuzzedDataProvider data(bytes, size);
    Input input{&data};
    FrondRapi* client = FrondRapiOpen({&input, Read, Write, Close});
    if (!client) return 0;
    uint8_t buffer[512];
    uint16_t reply[256];
    uint32_t result = 0, type = 0, count = 0, length = sizeof(buffer);
    for (int i = 0; i < 12 && data.remaining_bytes(); ++i) {
        switch (data.ConsumeIntegralInRange(0, 8)) {
            case 0: FrondRapiReady(client); break;
            case 1: FrondRapiProvision(client, path, reply, data.ConsumeIntegralInRange<size_t>(0, 256)); break;
            case 2: FrondRapiRegOpenKey(client, 0x80000002u, path, &result); break;
            case 3:
                length = sizeof(buffer);
                FrondRapiRegQueryValue(client, result, path, &type, buffer, &length);
                break;
            case 4: FrondRapiRead(client, FrondRapiOpenFile(client, path), buffer, sizeof(buffer), &count); break;
            case 5: FrondRapiList(client, path, Entry, nullptr); break;
            case 6: FrondRapiSyncStart(client, path, &result); break;
            case 7: FrondRapiCreate(client, path); break;
            default: FrondRapiStartReplication(client, &result); break;
        }
    }
    FrondRapiClose(client);
    return 0;
}
