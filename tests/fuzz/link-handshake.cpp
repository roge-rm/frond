// A link's responder end: the Noise KK handshake and refuse frames from an unauthenticated peer.
#include <fuzzer/FuzzedDataProvider.h>

#include "link.h"

using namespace frond::link;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    static const LinkConfig config = [] {
        LinkConfig c;
        Key32 secret{};
        secret[0] = 7;
        identityFromSecret(secret, c.local.identity);
        c.local.name = "fuzz";
        return c;
    }();
    FuzzedDataProvider data(bytes, size);
    Link link(config, [](const InstallId& id, Key32& key) {
        key.fill(id[0]);
        return id[1] != 0;
    }, 0);
    Key32 ephemeral{};
    ephemeral[0] = 11;
    link.setTestEphemeral(ephemeral);
    link.start();
    uint64_t now = 0;
    while (data.remaining_bytes() && link.state() != LinkState::Down) {
        now += data.ConsumeIntegralInRange<uint64_t>(0, 3000);
        const auto chunk = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange(1, 300));
        link.receive(chunk.data(), chunk.size(), now);
        link.tick(now);
        link.takeOutput();
    }
    return 0;
}
