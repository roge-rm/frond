// The link's channel mux fed decoded messages from a peer, with our side opening, accepting,
// sending and consuming in between.
#include <fuzzer/FuzzedDataProvider.h>

#include "mux.h"

using namespace frond::link;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    FuzzedDataProvider data(bytes, size);
    Mux mux(data.ConsumeBool());
    uint8_t buffer[256] = {};
    while (data.remaining_bytes()) {
        switch (data.ConsumeIntegralInRange(0, 9)) {
            case 0: mux.open(ChannelKind(data.ConsumeIntegralInRange(1, 3)), "m", "d"); break;
            case 1: mux.accept(data.ConsumeIntegral<uint16_t>(), "m", "d"); break;
            case 2: mux.send(data.ConsumeIntegral<uint16_t>(), buffer, data.ConsumeIntegralInRange<size_t>(0, sizeof(buffer))); break;
            case 3: mux.consumed(data.ConsumeIntegral<uint16_t>(), data.ConsumeIntegralInRange<size_t>(0, 70000)); break;
            case 4: mux.close(data.ConsumeIntegral<uint16_t>()); break;
            case 5: {
                Message out;
                while (mux.next(out)) {}
                mux.events.clear();
                break;
            }
            default: {
                Message m;
                m.type = MsgType(data.ConsumeIntegralInRange(0, 13));
                m.channel = data.ConsumeIntegralInRange<uint16_t>(0, 70);
                m.payload = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange(0, 64));
                if (!mux.receive(m)) mux.reset(CloseReason::Protocol);
            }
        }
    }
    return 0;
}
