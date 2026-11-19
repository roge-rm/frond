#ifndef FROND_DESKTOP_LINK_H
#define FROND_DESKTOP_LINK_H

// The desktop's end of a handheld's USB network link (RNDIS), as a PC runs it for ActiveSync. The handheld is
// the DHCP server (WM5's dtpt_srv at 169.254.2.1), so this asks it for an address like the PC's DHCP client
// (DISCOVER, OFFER, REQUEST, ACK, a DISCOVER again every 4 s until then) and then puts libslirp behind that
// address (169.254.2.2 from WM5). The handheld's connections to it go to the same ports on the host's
// loopback, where the desktop's sync program listens (ActiveSync's 990, DTPT's 5721 and the rest), and
// anything else goes out through the host like the Wi-Fi router. WM5 only opens 990 once the desktop asks with
// one UDP byte (0x7f) to its port 5679, as SynCE's dccm and ActiveSync do, so UDP to the host's
// 127.0.0.1:5679 is passed to the handheld's 5679. Whole Ethernet frames in and out;
// single-threaded; time is guest nanoseconds. Built into libcommon-net.a so only the cores link libslirp.

#ifndef __EMSCRIPTEN__
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace frondNet {

class WifiRouter;

class DesktopLink {
   public:
    static constexpr uint64_t discoverNs = 4000000000;  // DISCOVER again after this without an OFFER
    static constexpr uint64_t requestNs = 2000000000;   // DISCOVER again after this without an ACK
    static constexpr uint16_t connectPort = 5679;       // the handheld's UDP port for the desktop's connect request

    // mac: the desktop's MAC address. An RNDIS device gives it as its permanent address (OID_802_3_PERMANENT_ADDRESS)
    // and WM5's DHCP server only answers it. Frames from slirp go out from it too. hostConnectPort: the host's
    // UDP port on 127.0.0.1 that goes to the handheld's connectPort once bound (0 for none).
    explicit DesktopLink(const uint8_t* mac, uint16_t hostConnectPort = connectPort);
    ~DesktopLink();
    // One Ethernet frame from the handheld.
    void Input(const uint8_t* frame, size_t size, uint64_t now);
    // Runs the DHCP client's timers, then slirp's timers and host sockets.
    void Poll(uint64_t now);
    // The next Ethernet frame for the handheld, if there is one.
    bool Receive(std::vector<uint8_t>& frame);
    // The address the handheld gave the desktop, and the handheld's (its DHCP server), 0 until then.
    uint32_t Address() const { return address; }
    uint32_t Server() const { return server; }

    // DHCP frames, public for tests: a DISCOVER or REQUEST from mac, and the type (2 OFFER, 5 ACK, 6 NAK, 0 none)
    // of a frame that answers xid for mac, with its yiaddr, server ID and netmask.
    static std::vector<uint8_t> DhcpFrame(const uint8_t* mac, uint8_t type, uint32_t xid, uint32_t requested, uint32_t serverId);
    struct Answer {
        uint8_t type{0};
        uint32_t yiaddr{0}, serverId{0}, netmask{0};
    };
    static Answer ParseDhcp(const uint8_t* frame, size_t size, const uint8_t* mac, uint32_t xid);

   private:
    enum class State { discover, request, bound };
    void Send(std::vector<uint8_t> frame);
    void Bind(uint32_t desktop, uint32_t handheld, uint32_t mask);
    State state{State::discover};
    uint32_t xid{0x46726e64}, offered{0}, offerServer{0}, address{0}, server{0}, netmask{0};
    uint64_t retryAt{0};
    uint16_t hostConnectPort{0};
    std::deque<std::vector<uint8_t>> outgoing;
    std::unique_ptr<WifiRouter> router;
    uint8_t mac[6]{};
};

}  // namespace frondNet

#endif
#endif
