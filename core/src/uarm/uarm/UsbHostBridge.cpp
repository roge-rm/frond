#include "UsbHostBridge.h"
#include "soc_PXA.h"
#include "soc_IMX1.h"
#include "soc_S3C2442.h"
#include "net/DesktopLink.h"
#include <cstdio>
#include <cstring>
#include "../../common/UsbHostBridge.cpp"
static UsbHostBridge::Backend PxaBackend(SocPXA* soc, UsbBusHost::Mode mode) {
    return {
        [soc, mode] {
            return mode != UsbBusHost::Mode::normal ? UsbDevicePort(soc->GetUsbController()) : soc->GetUsbPort();
        },
        [soc](bool attached) { soc->SetUsbTestCable(attached); },
        [soc] { return soc->UsbTestCableAttached(); },
        [soc] { return soc->UsbDefaultCableAttached(); },
        [soc] { return soc->UsbHostIoPaused(); },
        [soc] { return soc->GetTime(); },
        [soc] { soc->KickUsbHost(); },
        [soc](const UsbHostHooks* hooks) { soc->SetUsbHostHooks(hooks); },
        [soc] { return !soc->GamepadAttached() && !soc->SpitzUsbDriveAttached(); },
        {}, {}, {},
        mode
    };
}

bool UsbHostBridge::Start(SocPXA* soc, const Config& config, std::string& error) {
    if (!soc) return Start(Backend{}, config, error);
    if (config.kind != HostSerial::Kind::off && soc->GamepadAttached()) {
        error = "The game controller is attached; detach it first.";
        return false;
    }
    return Start(PxaBackend(soc, UsbBusHost::Mode::normal), config, error);
}

bool UsbHostBridge::StartZaurusProbe(SocPXA* soc, bool readStorage, std::string& error) {
    if (!soc || soc->GetDeviceType() != deviceTypeSpitz) {
        error = "USB enumeration probe needs an SL-C3000.";
        return false;
    }
    if (soc->SpitzUsbDriveAttached()) {
        error = "Eject the virtual USB drive first.";
        return false;
    }
    const auto mode = readStorage ? UsbBusHost::Mode::storageProbe : UsbBusHost::Mode::enumerationOnly;
    return Start(PxaBackend(soc, mode), {}, error);
}

bool UsbHostBridge::StartZaurusNetworkProbe(SocPXA* soc, std::string& error, uint16_t filesPort,
                                           std::shared_ptr<ZaurusFilesEndpoint> files) {
    if (!soc || soc->GetDeviceType() != deviceTypeSpitz) {
        error = "USB network probe needs an SL-C3000.";
        return false;
    }
    if (soc->SpitzUsbDriveAttached()) {
        error = "Eject the virtual USB drive first.";
        return false;
    }
    struct Desktop {
        uint32_t generation{0};
        std::unique_ptr<frondNet::DesktopLink> link;
    };
    auto desktop = std::make_shared<Desktop>();
    auto backend = PxaBackend(soc, UsbBusHost::Mode::zaurusNetwork);
    backend.ethernetPoll = [desktop, files](uint64_t now, const uint8_t*, uint32_t generation) {
        if (!desktop->link || desktop->generation != generation) {
            if (files) files->status = 0;
            constexpr uint8_t mac[]{0x02, 0x46, 0x52, 0x4f, 0x4e, 0x44};
            desktop->link = std::make_unique<frondNet::DesktopLink>(mac, 0);
            desktop->generation = generation;
        }
        desktop->link->Poll(now);
    };
    backend.ethernetOut = [desktop, filesPort, files](const uint8_t* frame, size_t size, uint64_t now) {
        if (!desktop->link) return;
        const auto before = desktop->link->Address();
        desktop->link->Input(frame, size, now);
        const auto address = desktop->link->Address();
        if (before != address && address) {
            const auto guest = desktop->link->Server();
            std::fprintf(stderr, "usb: Sharp PC Link DHCP host %u.%u.%u.%u guest %u.%u.%u.%u\n",
                         address >> 24, address >> 16 & 255, address >> 8 & 255, address & 255,
                         guest >> 24, guest >> 16 & 255, guest >> 8 & 255, guest & 255);
            constexpr uint16_t netbiosSession = 139;
            if (files) {
                const int result = desktop->link->AddUnixTcpForward(files->path, netbiosSession);
                files->status = result ? -result : 1;
            }
            if (filesPort) {
                const int result = desktop->link->AddTcpForward(filesPort, netbiosSession);
                if (result)
                    std::fprintf(stderr, "usb: Sharp PC Link file port %u: %s\n", unsigned(filesPort), std::strerror(result));
                else
                    std::fprintf(stderr, "usb: forwarding 127.0.0.1:%u to Sharp PC Link TCP 139\n", unsigned(filesPort));
            }
        }
    };
    backend.ethernetDown = [desktop, files] {
        if (files) files->status = 0;
        desktop->link.reset();
    };
    backend.ethernetIn = [desktop](std::vector<uint8_t>& frame) {
        return desktop->link && desktop->link->Receive(frame);
    };
    return Start(std::move(backend), {}, error);
}

bool UsbHostBridge::Start(SoC* soc, const Config& config, std::string& error) {
    if (auto* pxa = dynamic_cast<SocPXA*>(soc)) return Start(pxa, config, error);
    if (auto* rx = dynamic_cast<SocS3C2442*>(soc)) {
        // The rx1950's USB device controller; the cable is VBUS on GPG5.
        Backend backend{
            [rx] { return rx->GetUsbPort(); },
            [rx](bool attached) { rx->SetUsbCable(attached); },
            [rx] { return rx->UsbCableAttached(); },
            [] { return false; },
            [rx] { return rx->UsbHostIoPaused(); },
            [rx] { return rx->GetTime(); },
            [] {},  // the host ticks on guest time, at least every poll step (SocS3C2442::ScheduleHost)
            [rx](const UsbHostHooks* hooks) { rx->SetUsbHostHooks(hooks); },
            [] { return true; },
            // WM5 runs ActiveSync over RNDIS: Frond's host network behind it.
            [rx](const uint8_t* frame, size_t size, uint64_t now) { rx->UsbEthernetOut(frame, size, now); },
            [rx](uint64_t now, const uint8_t* mac, uint32_t link) { rx->UsbEthernetPoll(now, mac, link); },
            [rx](std::vector<uint8_t>& frame) { return rx->UsbEthernetIn(frame); }
        };
        return Start(std::move(backend), config, error);
    }
    auto* zodiac = dynamic_cast<SocIMX1*>(soc);
    if (!zodiac) return Start(Backend{}, config, error);
    // The Zodiac's USB comes through the cradle: the cable plugs into the cradle and only reaches the
    // Zodiac while the user has it in the cradle. Off leaves the cradle as it is.
    Backend backend{
        [zodiac] { return zodiac->GetUsbPort(); },
        [zodiac](bool attached) { zodiac->SetUsbCable(attached); },
        [zodiac] { return zodiac->UsbCableAttached(); },
        [] { return false; },
        [zodiac] { return zodiac->UsbHostIoPaused(); },
        [zodiac] { return zodiac->GetTime(); },
        [] {},  // the host ticks at every run slice end
        [zodiac](const UsbHostHooks* hooks) { zodiac->SetUsbHostHooks(hooks); },
        [] { return true; },
        {}, {}, {}
    };
    return Start(std::move(backend), config, error);
}
bool UsbHostBridge::NetworkLink(SoC* soc) { return dynamic_cast<SocS3C2442*>(soc) != nullptr; }
bool UsbHostBridge::StartCable(SoC* soc, std::string& error) {
    if (!NetworkLink(soc)) {
        error = "This device's USB needs a pty, tcp or unix endpoint.";
        return false;
    }
    Config config;
    config.kind = HostSerial::Kind::pty;  // not opened: only says the cable is in
    impl->cableRequest = true;
    return Start(soc, config, error);
}
