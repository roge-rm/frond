// HotSync framing from the handheld: SLP/PADP on serial and USB, NetSync on the network.
#include "FuzzStream.h"
#include "sync/Link.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    using namespace frondSync;
    FuzzedDataProvider data(bytes, size);
    const auto transport = Transport(data.ConsumeIntegralInRange(0, 3));
    FuzzStream stream(data);
    std::atomic<bool> stop{false};
    std::string error;
    auto link = AcceptHandheld(stream, transport, stop, error);
    if (!link) return 0;
    Bytes message;
    for (int i = 0; i < 16 && link->Receive(message, 50); ++i) {
        if (!link->Send(Bytes(message.begin(), message.begin() + std::min<size_t>(message.size(), 40)))) break;
    }
    link->Finish();
    return 0;
}
