// A whole HotSync (DLP session) against a scratch sync folder, the handheld's answers from the
// input. Each answer gets the request's function code so the session gets past the first call.
#include <fuzzer/FuzzedDataProvider.h>

#include "Scratch.h"
#include "sync/HotSync.h"

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
        if (!message.empty() && data.ConsumeBool()) message[0] = uint8_t(function | 0x80);
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
    const auto root = FuzzScratch("hotsync-session", runs);
    FuzzedDataProvider data(bytes, size);
    SyncOptions options;
    options.root = root;
    options.machineKey = "fuzz";
    Handheld handheld(data);
    RunHotSync(handheld, options, root, Transport::serial, SyncManager::Get());
    return 0;
}
