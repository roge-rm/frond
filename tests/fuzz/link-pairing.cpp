// The pairing exchange from the side that shows the code, fed what a peer sends before any key
// is trusted.
#include <fuzzer/FuzzedDataProvider.h>

#include "pairing.h"

using namespace frond::link;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    static const LocalInfo local = [] {
        LocalInfo info;
        Key32 secret{};
        secret[0] = 7;
        identityFromSecret(secret, info.identity);
        info.name = "fuzz";
        return info;
    }();
    FuzzedDataProvider data(bytes, size);
    PairCode code("482913", 0);
    Pairing pairing(local, code, 0);
    Key32 y{};
    y[0] = 9;
    pairing.setTestScalar(y);
    pairing.start();
    uint64_t now = 0;
    while (data.remaining_bytes() && pairing.result() == PairResult::Pending) {
        now += data.ConsumeIntegralInRange<uint64_t>(0, 1000);
        const auto chunk = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange(1, 300));
        pairing.receive(chunk.data(), chunk.size(), now);
        pairing.tick(now);
        pairing.takeOutput();
    }
    return 0;
}
