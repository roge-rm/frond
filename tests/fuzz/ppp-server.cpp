// The PPP answering end behind the emulated modem: login prompt, HDLC framing, LCP, PAP and IPCP.
#include <fuzzer/FuzzedDataProvider.h>

#include "modem/PppServer.h"

namespace {
class Network : public frondNet::IpNetwork {
   public:
    void Input(const uint8_t*, size_t size, uint64_t) override { got += size; }
    void Poll(uint64_t) override {}
    bool Receive(std::vector<uint8_t>& packet) override {
        if (!send) return false;
        send = false;
        packet.assign(60, 0x45);
        return true;
    }
    size_t got{0};
    bool send{true};
};
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    FuzzedDataProvider data(bytes, size);
    Network network;
    frondModem::PppServer::Options options;
    options.loginPrompt = data.ConsumeBool();
    options.askPap = data.ConsumeBool();
    frondModem::PppServer ppp(network, options);
    uint64_t now = 0;
    ppp.Answer(now);
    uint8_t out[512];
    while (data.remaining_bytes()) {
        now += data.ConsumeIntegralInRange<uint64_t>(0, 4000000000ull);
        const auto chunk = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange(1, 256));
        ppp.FromModem(chunk.data(), chunk.size(), now);
        ppp.Service(now);
        while (ppp.ToModem(out, sizeof(out), now)) {}
        if (data.ConsumeIntegralInRange(0, 31) == 0) network.send = true;
    }
    ppp.HangUp();
    return 0;
}
