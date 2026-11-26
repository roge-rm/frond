// Frond-to-Frond link framing and the plain message bodies, as read from a TCP peer.
#include <fuzzer/FuzzedDataProvider.h>

#include "messages.h"
#include "wire.h"

using namespace frond::link;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    peekFirstFrame(bytes, size);
    FuzzedDataProvider data(bytes, size);
    FrameReader reader(data.ConsumeIntegralInRange<size_t>(0, 70000));
    while (data.remaining_bytes() && !reader.error()) {
        const auto chunk = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange(1, 300));
        reader.feed(chunk.data(), chunk.size());
        Bytes body;
        while (reader.next(body)) {
            uint8_t version;
            FrameMode mode;
            parseHeader(body, version, mode);
            Message m;
            if (!decodeMessage(body.data(), body.size(), m)) continue;
            HelloBody hello;
            OpenBody open;
            MachineBody machine;
            RadioBody radio;
            decodeHello(m.payload, hello);
            decodeOpen(m.payload, open);
            decodeMachine(m.payload, machine);
            decodeRadio(m.payload, radio);
            Bytes again = encodeMessage(m);
        }
    }
    return 0;
}
