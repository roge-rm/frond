#ifndef __EMSCRIPTEN__
#include "UsbHostBridge.h"
#include <algorithm>



struct UsbHostBridge::Impl {
    Backend soc;
    Config config;
    HostByteLink link;
    std::unique_ptr<UsbBusHost> host;
    RndisHost rndis;
    uint64_t lastNow{0}, netPollAt{0};
    static constexpr uint64_t netPollNs = 1000000;
    bool replaced{false}, plugPending{false}, lastEnabled{false}, linkKept{false};
    void StopBridge(bool keepLink);
    uint64_t plugAt{0};
    std::vector<std::string> messages;
    UsbBusHost::Phase lastPhase{UsbBusHost::Phase::detached};

    // Peripheral batch, on emulated time: unplug after a load or reset, plug in when
    // due, then the host's own work.
    static void Tick(void* context, uint64_t now) {
        auto& self = *static_cast<Impl*>(context);
        if (!self.host) return;
        if (self.replaced) {
            self.replaced = false;
            self.Unplug();
            self.plugPending = true;
            self.plugAt = now + replugNs;
        }
        if (self.plugPending && now >= self.plugAt) {
            self.plugPending = false;
            // Refused (the game controller is in): the cable stays out and the host isn't attached.
            if (self.soc.canPlug && !self.soc.canPlug()) { self.soc.cable(false); return; }
            self.soc.cable(true);
            self.host->Attach();
        }
        self.lastNow = now;
        // RNDIS: frames from the host network enter once a millisecond.
        if (self.host->Rndis() && self.rndis.Up() && self.soc.ethernetPoll && now >= self.netPollAt) {
            self.soc.ethernetPoll(now, self.rndis.Mac(), self.rndis.GetCounters().ups);
            std::vector<uint8_t> frame;
            for (unsigned n = 0; n < 64 && self.soc.ethernetIn(frame); ++n) self.rndis.FrameToDevice(frame.data(), frame.size());
            self.netPollAt = (now / netPollNs + 1) * netPollNs;
        }
        self.host->Tick(now);
    }
    static uint64_t Wake(void* context) {
        auto& self = *static_cast<Impl*>(context);
        if (!self.host) return UINT64_MAX;
        if (self.replaced) return 0;
        uint64_t wake = self.host->NextWake();
        if (self.host->Rndis() && self.rndis.Up() && self.soc.ethernetPoll) wake = std::min(wake, self.netPollAt);
        return self.plugPending ? std::min(wake, self.plugAt) : wake;
    }
    // Reset or load: the host forgets the device at once; the next tick unplugs the cable.
    // Off: a session saved while the bridge had the cable out gets it back in.
    static void Replaced(void* context) {
        auto& self = *static_cast<Impl*>(context);
        self.replaced = true;
        if (self.host) self.host->Detach();
        self.link.Clear();
    }
    // Any guest write to the UDC.
    static void Notify(void* context) {
        auto& self = *static_cast<Impl*>(context);
        if (!self.host || self.soc.paused()) return;
        self.host->Poke();
        self.soc.kick();
    }
    void Unplug() {
        if (soc.attached()) soc.cable(false);
    }
};

UsbHostBridge::UsbHostBridge() : impl(std::make_unique<Impl>()) {}
UsbHostBridge::~UsbHostBridge() { Stop(); }

bool UsbHostBridge::Start(Backend soc, const Config& config, std::string& error) {
    const bool reuse = impl->linkKept && impl->link.IsOpen() && config.kind != HostSerial::Kind::off &&
                       HostSerial::Format(impl->link.GetConfig()) == HostSerial::Format(config);
    impl->StopBridge(reuse);
    impl->linkKept = false;
    if (!soc.port || !soc.port()) {
        if (config.kind == HostSerial::Kind::off) return true;
        error = "This device has no USB port.";
        return false;
    }
    // Restore the board's disconnected-host default, also after loading a session
    // saved with a different cable level: TX GPIO13 high; E2 GPIO35 high.
    auto off = [&] {
        impl->soc = soc;
        soc.cable(soc.defaultCable());
        const UsbHostHooks hooks{impl.get(), nullptr, nullptr, Impl::Replaced};
        soc.hooks(&hooks);
    };
    if (config.kind == HostSerial::Kind::off) {
        off();
        return true;
    }
    if (reuse) impl->link.Clear();
    if (!reuse && !impl->link.Open(config, error)) {
        off();
        return false;
    }
    impl->soc = soc;
    const bool wasPlugged = soc.attached();
    impl->Unplug();
    impl->config = impl->link.GetConfig();
    auto* link = &impl->link;
    impl->host = std::make_unique<UsbBusHost>(
        [soc] { return soc.port(); },
        UsbBusHost::Link{[link] { return link->SendSpace(); },
                         [link](const uint8_t* data, size_t size) { return link->Send(data, size); },
                         [link](uint8_t* data, size_t size) { return link->Receive(data, size); },
                         [link] { return link->PendingReceive(); }});
    // A cable the session left plugged in comes out first, so the TX sees a new plug.
    impl->plugPending = true;
    impl->plugAt = soc.time() + (wasPlugged ? replugNs : 0);
    impl->lastPhase = UsbBusHost::Phase::detached;
    if (soc.ethernetOut) {
        auto* self = impl.get();
        impl->rndis.frameFromDevice = [self](const uint8_t* frame, size_t size) {
            if (self->soc.ethernetOut) self->soc.ethernetOut(frame, size, self->lastNow);
        };
        impl->host->SetRndis(&impl->rndis);
    }
    const UsbHostHooks hooks{impl.get(), Impl::Tick, Impl::Wake, Impl::Replaced, Impl::Notify};
    soc.hooks(&hooks);
    soc.port().SetNotify(Impl::Notify, impl.get());
    soc.kick();
    return true;
}

void UsbHostBridge::Stop() { impl->StopBridge(false); }
void UsbHostBridge::Suspend() { impl->StopBridge(impl->link.IsOpen()); }

void UsbHostBridge::Impl::StopBridge(bool keepLink) {
    auto* impl = this;
    if (!keepLink) linkKept = false;
    if (!impl->soc.port) {
        if (!keepLink && impl->link.IsOpen()) impl->link.Close();
        return;
    }
    impl->soc.hooks(nullptr);
    if (impl->soc.port()) impl->soc.port().SetNotify(nullptr, nullptr);
    // Restore the board default with no host on the bus.
    if (impl->host) impl->host->Detach();
    impl->soc.cable(impl->soc.defaultCable());
    impl->host.reset();
    impl->rndis.Stop();
    impl->rndis.frameFromDevice = nullptr;
    if (keepLink) linkKept = true; else impl->link.Close();
    impl->soc = {};
    impl->config = {};
    impl->replaced = impl->plugPending = false;
}

void UsbHostBridge::Service() {
    auto& soc = impl->soc;
    if (!soc.port || soc.paused()) return;
    if (!impl->host) {
        // Off: restore the board cable default after a load (see Start).
        if (impl->replaced) {
            impl->replaced = false;
            soc.cable(soc.defaultCable());
        }
        return;
    }
    // The emulated-time work happens in the peripheral batch; this only wakes it when
    // a load dropped its schedule, the host side has bytes or room, or it is overdue.
    const bool enabled=soc.port().Enabled();
    if(enabled!=impl->lastEnabled) {
        impl->lastEnabled=enabled;
        impl->host->Poke();
    }
    if (impl->host->LinkWaiting()) impl->host->Poke();
    const uint64_t wake = Impl::Wake(impl.get());
    if (wake <= soc.time()) soc.kick();
    for (auto& message : impl->host->TakeMessages()) impl->messages.push_back(std::move(message));
    for (auto& message : impl->rndis.TakeMessages()) impl->messages.push_back(std::move(message));
    const auto phase = impl->host->GetPhase();
    if (phase != impl->lastPhase) {
        impl->lastPhase = phase;
        impl->messages.push_back(std::string("usb: ") + UsbBusHost::PhaseName(phase));
    }
}

UsbHostBridge::Config UsbHostBridge::GetConfig() const { return impl->config; }
std::string UsbHostBridge::Endpoint() const { return impl->link.Endpoint(); }

std::string UsbHostBridge::Describe() const {
    if (!impl->host) return "off";
    return std::string(impl->config.kind == HostSerial::Kind::pty ? "pty " : impl->config.kind == HostSerial::Kind::unixSocket ? "unix " : "tcp ") + impl->link.Endpoint() +
           (impl->link.Connected() ? " connected " : " waiting ") + UsbBusHost::PhaseName(impl->host->GetPhase()) +
           (impl->host->Rndis() ? impl->rndis.Up() ? " rndis up" : " rndis starting" : "");
}

const UsbBusHost* UsbHostBridge::Host() const { return impl->host.get(); }
std::vector<std::string> UsbHostBridge::TakeMessages() { return std::move(impl->messages); }
#endif
