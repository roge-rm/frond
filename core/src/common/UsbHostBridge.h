#ifndef FROND_USB_HOST_BRIDGE_H
#define FROND_USB_HOST_BRIDGE_H
#ifndef __EMSCRIPTEN__
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../uarm/uarm/HostSerial.h"
#include "UsbHostHooks.h"
#include "UsbBusHost.h"

class SocPXA;

// Connects the TX or E2 USB port to a host PTY or a TCP listener on 127.0.0.1 through
// UsbBusHost: plugs the cable in (TX GPIO13, E2 GPIO35), enumerates the device and moves the bytes of its
// HotSync port both ways. Start and every load or reset unplug the cable and plug it in
// again half a second of emulated time later; checkpoint restores for reads leave it.
// The host runs in the SoC's peripheral batch on emulated time and sleeps between the
// events it needs. Off keeps TX GPIO13 low or E2 GPIO35 high, with no host on
// the bus. Every method runs on the emulator thread.
class UsbHostBridge {
   public:
    using Config = HostSerial::Config;
    static constexpr uint64_t replugNs = 500000000;

    struct Backend {
        std::function<UsbDevicePort()> port;
        std::function<void(bool)> cable;
        std::function<bool()> attached, defaultCable, paused;
        std::function<uint64_t()> time;
        std::function<void()> kick;
        std::function<void(const UsbHostHooks*)> hooks;
    };
    bool Start(Backend backend, const Config& config, std::string& error);
    UsbHostBridge();
    ~UsbHostBridge();
    UsbHostBridge(const UsbHostBridge&) = delete;
    UsbHostBridge& operator=(const UsbHostBridge&) = delete;

    bool Start(SocPXA* soc, const Config& config, std::string& error);
    void Stop();  // unplugs the cable and closes the endpoint
    // Like Stop, but the host endpoint (a PTY's path, a TCP port) stays open for a Start with
    // the same transport.
    void Suspend();
    void Service();  // between emulation slices

    Config GetConfig() const;
    std::string Endpoint() const;
    std::string Describe() const;  // off, or kind, endpoint, peer and USB state
    std::vector<std::string> TakeMessages();
    const UsbBusHost* Host() const;  // nullptr when off

   private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
#endif
#endif
