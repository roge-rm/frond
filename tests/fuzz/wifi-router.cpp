// The virtual router's guest side: Ethernet frames into libslirp (ARP, DHCP, DNS, ICMP, TCP and UDP),
// with the Network HotSync or ActiveSync TCP rewrite in front of it when the input turns them on.
// Runs in its own network namespace (see Isolated.h) since slirp opens host sockets.
#include <fuzzer/FuzzedDataProvider.h>

#include "Isolated.h"
#include "NetFrames.h"
#include "Scratch.h"
#include "WifiRouter.h"
#include "sync/ActiveSync.h"
#include "sync/SyncManager.h"

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    RequireIsolatedNetwork();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    using namespace frondSync;
    static unsigned runs = 0;
    FuzzedDataProvider data(bytes, size);
    const unsigned mode = data.ConsumeIntegralInRange(0, 2);
    SyncOptions options;
    options.root = FuzzScratch("wifi-router", runs);
    options.machineKey = "fuzz";
    SyncManager::Get().Configure(options);
    SyncManager::Get().SetNetworkEnabled(mode == 1);
    frondNet::RouterLayout layout;
    layout.dhcp = data.ConsumeBool();
    std::shared_ptr<ActiveSyncSession> active;
    if (mode == 2) {
        active = std::make_shared<ActiveSyncSession>();
        if (data.ConsumeBool()) active->Start();
    }
    {
        frondNet::WifiRouter router(layout, active);
        uint64_t now = 1000000;
        std::vector<uint8_t> out;
        while (data.remaining_bytes()) {
            auto frame = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(14, 1518));
            if (frame.size() > 14 && data.ConsumeBool()) FuzzFixIpv4(frame.data() + 14, frame.size() - 14);
            router.Input(frame.data(), frame.size(), now);
            now += data.ConsumeIntegralInRange<uint64_t>(0, 2000000000ull);
            router.Poll(now);
            while (router.Receive(out)) {}
        }
    }
    if (active) active->Stop();
    SyncManager::Get().SetNetworkEnabled(false);
    return 0;
}
