// The host side of USB RNDIS: the device's control replies, interrupt notifications and bulk IN
// messages come from the input, in an order it picks, with the host's own commands and frames between.
#include <fuzzer/FuzzedDataProvider.h>

#include "RndisHost.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    FuzzedDataProvider data(bytes, size);
    RndisHost host;
    size_t frames = 0;
    host.frameFromDevice = [&](const uint8_t*, size_t n) { frames += n; };
    uint64_t now = 0;
    host.Start(data.ConsumeIntegralInRange<unsigned>(8, 512), now);
    std::vector<uint8_t> message;
    uint8_t packet[1024];
    while (data.remaining_bytes()) {
        now += data.ConsumeIntegralInRange<uint64_t>(0, 6000000000ull);
        switch (data.ConsumeIntegralInRange(0, 8)) {
            case 0:
                if (host.NextCommand(message, now)) host.CommandSent(data.ConsumeBool(), now);
                break;
            case 1: {
                const auto reply = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(0, 1100));
                if (host.WantResponse(now) || data.ConsumeBool()) host.Response(data.ConsumeBool(), reply.data(), reply.size(), now);
                break;
            }
            case 2: {
                const auto note = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(0, 16));
                host.Notification(note.data(), note.size(), now);
                break;
            }
            case 3: {
                const auto in = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(0, 512));
                host.BulkInPacket(in.data(), in.size());
                break;
            }
            case 4: {
                const auto in = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(0, 0x4000));
                host.BulkInTransfer(in.data(), in.size());
                break;
            }
            case 5: {
                unsigned sent = 0;
                const unsigned max = data.ConsumeIntegralInRange<unsigned>(1, sizeof(packet));
                for (int i = 0; i < 64 && host.BulkOutPacket(packet, max, sent); ++i) {}
                break;
            }
            case 6: {
                const auto frame = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(0, 1600));
                host.FrameToDevice(frame.data(), frame.size());
                break;
            }
            case 7:
                host.NextWake();
                host.TakeMessages();
                break;
            default:
                host.Stop();
                host.Start(data.ConsumeIntegralInRange<unsigned>(8, 512), now);
                break;
        }
    }
    return 0;
}
