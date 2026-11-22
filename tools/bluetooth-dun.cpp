// The Bluetooth phone's modem for bluetooth-opp-peer --dun, see bluetooth-dun.h.
#include "bluetooth-dun.h"

#include <cstdio>
#include <memory>
#include <string>

#include "modem/HayesModem.h"
#include "modem/PppServer.h"
#include "net/SlirpIp.h"
#include "sync/SyncManager.h"

using frondModem::HayesModem;

struct frond_dun {
    frondNet::SlirpIp network;
    frondModem::PppServer ppp{network};
    HayesModem modem{ppp};
    bool open{false};
    std::string state{"off"};
};

extern "C" {

struct frond_dun* frond_dun_create(const char* sync_folder, const char* sync_profile) {
    auto dun = std::make_unique<frond_dun>();
    if (!dun->network.Valid()) return nullptr;
    if (sync_folder) {
        dun->network.EnableZodiacNetSyncWakeup();
        frondSync::SyncOptions options;
        options.root = sync_folder;
        if (sync_profile) options.profile = sync_profile;
        options.network = true;
        // Allow the Palm's TCP retransmission backoff on the 33.6 kbit/s Bluetooth modem.
        options.networkReplyIdleMs = 45000;
        auto& manager = frondSync::SyncManager::Get();
        manager.Configure(options);
        manager.SetListener([](const std::string& status) {
            std::fprintf(stderr, "Network HotSync: %s\n", status.c_str());
        });
    }
    return dun.release();
}

void frond_dun_destroy(struct frond_dun* dun) { delete dun; }

// The modem is on while the channel is open, as a phone's is while its DUN port is in use.
void frond_dun_open(struct frond_dun* dun, uint64_t now) {
    dun->open = true;
    dun->modem.SetPower(true, now);
}

void frond_dun_close(struct frond_dun* dun, uint64_t now) {
    dun->open = false;
    dun->modem.SetPower(false, now);
}

void frond_dun_input(struct frond_dun* dun, const uint8_t* data, size_t size, uint64_t now) {
    if (dun->open) dun->modem.Write(data, size, now);
}

size_t frond_dun_output(struct frond_dun* dun, uint8_t* data, size_t max, uint64_t now) {
    dun->modem.Service(now);
    return dun->open ? dun->modem.Read(data, max) : 0;
}

const char* frond_dun_state(const struct frond_dun* dun) {
    auto& s = const_cast<frond_dun*>(dun)->state;
    switch (dun->modem.GetState()) {
        case HayesModem::State::Off: s = "off"; break;
        case HayesModem::State::Dialling: s = "dialling"; break;
        case HayesModem::State::Online:
        case HayesModem::State::OnlineCommand: s = "online " + std::to_string(dun->modem.LineSpeed()); break;
        default: s = "idle"; break;
    }
    return s.c_str();
}

}  // extern "C"
