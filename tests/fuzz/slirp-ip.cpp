// libslirp behind an IP-only link (PPP and the IrDA phone): IPv4 packets wrapped in Ethernet for
// slirp, with the Network HotSync rewrite and the Zodiac wakeup when the input turns them on.
// Runs in its own network namespace (see Isolated.h) since slirp opens host sockets.
#include <fuzzer/FuzzedDataProvider.h>

#include "Isolated.h"
#include "NetFrames.h"
#include "Scratch.h"
#include "SlirpIp.h"
#include "sync/SyncManager.h"

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    RequireIsolatedNetwork();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    using namespace frondSync;
    static unsigned runs = 0;
    FuzzedDataProvider data(bytes, size);
    SyncOptions options;
    options.root = FuzzScratch("slirp-ip", runs);
    options.machineKey = "fuzz";
    SyncManager::Get().Configure(options);
    SyncManager::Get().SetNetworkEnabled(data.ConsumeBool());
    {
        frondNet::SlirpIp ip;
        if (data.ConsumeBool()) ip.EnableZodiacNetSyncWakeup();
        uint64_t now = 1000000;
        std::vector<uint8_t> out;
        while (data.remaining_bytes()) {
            auto packet = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(20, 1500));
            if (data.ConsumeBool()) FuzzFixIpv4(packet.data(), packet.size());
            ip.Input(packet.data(), packet.size(), now);
            now += data.ConsumeIntegralInRange<uint64_t>(0, 2000000000ull);
            ip.Poll(now);
            while (ip.Receive(out)) {}
            if (data.ConsumeIntegralInRange(0, 63) == 0) ip.Reset();
        }
    }
    SyncManager::Get().SetNetworkEnabled(false);
    return 0;
}
