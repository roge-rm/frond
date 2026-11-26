// The Palm conduit host's requests (PalmConduit.cpp Runner::Handle, framed as PalmConduitProtocol.h)
// with the handheld's DLP answers from the input, and the conduit registration files
// (LoadRegistration). The conduit is a Windows DLL under Wine, so its requests are outside data.
#include <fuzzer/FuzzedDataProvider.h>

#include <fstream>

#include "../../core/src/common/sync/PalmConduit.cpp"
#include "Scratch.h"

namespace {
class Handheld : public frondSync::PacketLink {
   public:
    explicit Handheld(FuzzedDataProvider& data) : data(data) {}
    bool Send(const frondSync::Bytes& message) override {
        function = message.empty() ? 0 : message[0];
        return true;
    }
    bool Receive(frondSync::Bytes& message, int) override {
        if (!data.remaining_bytes()) return error = "closed", false;
        message = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(0, 600));
        if (message.size() >= 4 && data.ConsumeBool()) message[0] = uint8_t(function | 0x80), message[2] = message[3] = 0;
        return true;
    }
    std::string Name() const override { return "fuzz"; }

   private:
    FuzzedDataProvider& data;
    uint8_t function{0};
};
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    using namespace frondSync;
    static unsigned runs = 0;
    if (size > 262144) return 0;
    FuzzedDataProvider data(bytes, size);
    if (data.ConsumeBool()) {
        // A registration file, named for its creator.
        const auto folder = fs::path(FuzzScratch("conduit-ipc", runs));
        const auto path = folder / (data.ConsumeBool() ? "6d656d6f.conf" : "00000000.conf");
        std::ofstream(path, std::ios::binary) << data.ConsumeRemainingBytesAsString();
        Registration registration;
        LoadRegistration(path, registration);
        return 0;
    }
    Handheld handheld(data);
    DlpClient dlp(handheld, 10);
    UserInfo user;
    user.name = "Fuzz";
    const ConduitAsset saveRecovery = [&](const std::string&, uint8_t, std::string& problem) {
        problem = "no";
        return data.ConsumeBool();
    };
    const ConduitLog log = [](const std::string&) {};
    Runner runner{dlp, user, saveRecovery, log, {}, {}};
    // Requests come framed: a 32-bit length then the body, cut off where the input ends.
    const auto stream = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(0, 65536));
    size_t at = 0;
    const auto read = [&](void* out, size_t length) {
        if (length > stream.size() - at) return false;
        std::memcpy(out, stream.data() + at, length);
        at += length;
        return true;
    };
    Bytes frame;
    for (unsigned n = 0; n < 512 && !runner.completed && frondConduit::ReadFrame(read, frame); ++n) {
        Writer payload, reply;
        const auto result = runner.Handle(frame, payload);
        reply.Put(result);
        if (result == frondConduit::success) reply.data.insert(reply.data.end(), payload.data.begin(), payload.data.end());
    }
    return 0;
}
