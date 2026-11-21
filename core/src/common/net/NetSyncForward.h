#ifndef FROND_NET_SYNC_FORWARD_H
#define FROND_NET_SYNC_FORWARD_H

#include <arpa/inet.h>
#include <libslirp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../sync/SyncManager.h"

namespace frondNet {

// Keep Network HotSync at the virtual host address. slirp reserves that address, so map
// the HotSync TCP flow to a private guest forward on the same subnet.
class NetSyncForward {
   public:
    void Configure(Slirp* slirp, uint32_t host, uint32_t network) {
        this->slirp = slirp;
        this->host = host;
        server.s_addr = htonl(network | 4);
    }

    bool Enabled() const { return slirp && (registered || frondSync::SyncManager::Get().NetworkEnabled()); }

    bool Input(std::vector<uint8_t>& frame) {
        UpdateRegistration();
        auto* tcp = Translate(frame, true);
        if (!tcp) return true;
        const uint16_t source = Get16(tcp);
        if ((tcp[13] & 0x12) == 0x02 && source != peer) {
            if (peer && endpoint.Running() && endpoint.Finished() && !endpoint.Pipe()->PendingToDevice())
                Release();
            if (!peer) {
                peer = source;
                lastProgress = Clock::now();
            }
        }
        if (source != peer) return false;  // One HotSync connection per guest.
        MarkClosed(tcp[13]);
        return true;
    }

    void Output(std::vector<uint8_t>& frame) {
        const auto* tcp = Translate(frame, false);
        if (!tcp || Get16(tcp + 2) != peer) return;
        MarkClosed(tcp[13]);
        if (!reset && (tcp[13] & 0x10)) RememberReset(frame.data(), tcp);
    }

    void Poll() {
        UpdateRegistration();
        if (!peer) return;
        if (reset || (closing && !endpoint.Running())) {
            Release();
            return;
        }
        if (endpoint.Running()) {
            if (closing) endpoint.Pipe()->FinishInput();
            const auto room = slirp_socket_can_recv(slirp, server, port);
            std::array<uint8_t, 4096> bytes;
            const auto count = endpoint.Pipe()->HostToDevice(bytes.data(), std::min(room, bytes.size()));
            if (count) {
                slirp_socket_recv(slirp, server, port, bytes.data(), int(count));
                lastProgress = Clock::now();
            }
        }
        const bool drained = !endpoint.Running() || !endpoint.Pipe()->PendingToDevice();
        if ((closing && drained && endpoint.Finished()) || Clock::now() - lastProgress > idleLimit)
            Release();
    }

   private:
    using Clock = std::chrono::steady_clock;
    static constexpr auto idleLimit = std::chrono::seconds(60);
    static constexpr uint16_t port = 14238;
    static constexpr size_t ethernetSize = 14, headerSize = 20;
    Slirp* slirp{};
    in_addr server{};
    uint32_t host{};
    uint16_t peer{};
    Clock::time_point lastProgress{};
    bool registered{false};
    bool closing{false}, reset{false}, haveResetPacket{false};
    std::array<uint8_t, ethernetSize + 2 * headerSize> resetPacket{};
    frondSync::SyncEndpoint endpoint;

    void UpdateRegistration() {
        if (!slirp) return;
        const bool enabled = frondSync::SyncManager::Get().NetworkEnabled();
        if (enabled && !registered) {
            registered = slirp_add_guestfwd(slirp, Write, this, &server, port) == 0;
        } else if (!enabled && registered) {
            if (peer) Release();
            slirp_remove_guestfwd(slirp, server, port);
            registered = false;
        }
    }

    void MarkClosed(uint8_t flags) {
        if (flags & 0x01) closing = true;
        if (flags & 0x04) reset = true;
    }

    void Release() {
        const bool closeSocket = haveResetPacket && !reset;
        closing = reset = haveResetPacket = false;
        peer = 0;
        endpoint.Stop();
        // The guest-forward API finds sockets by service address and port. Retire this
        // socket before a new guest connection uses the same service.
        if (closeSocket) slirp_input(slirp, resetPacket.data(), int(resetPacket.size()));
    }

    void RememberReset(const uint8_t* frame, const uint8_t* reply) {
        resetPacket.fill(0);
        auto* ethernet = resetPacket.data();
        auto* ip = ethernet + ethernetSize;
        auto* tcp = ip + headerSize;
        std::copy_n(frame + 6, 6, ethernet);
        std::copy_n(frame, 6, ethernet + 6);
        Set16(ethernet + 12, 0x0800);
        ip[0] = 0x45;
        Set16(ip + 2, 2 * headerSize);
        ip[8] = 64;
        ip[9] = 6;
        std::copy_n(frame + ethernetSize + 16, 4, ip + 12);
        const uint32_t alias = ntohl(server.s_addr);
        Set16(ip + 16, uint16_t(alias >> 16));
        Set16(ip + 18, uint16_t(alias));
        Set16(ip + 10, Checksum(ip, headerSize));
        Set16(tcp, peer);
        Set16(tcp + 2, port);
        std::copy_n(reply + 8, 4, tcp + 4);  // The next guest sequence number.
        tcp[12] = 5 << 4;
        tcp[13] = 0x04;
        uint32_t pseudo = 6 + headerSize;
        for (size_t i = 12; i < headerSize; i += 2) pseudo += Get16(ip + i);
        Set16(tcp + 16, Checksum(tcp, headerSize, pseudo));
        haveResetPacket = true;
    }

    static slirp_ssize_t Write(const void* bytes, size_t size, void* opaque) {
        auto& self = *static_cast<NetSyncForward*>(opaque);
        if (!self.peer || self.reset || (self.endpoint.Running() && self.endpoint.Finished())) return size;
        if (!self.endpoint.Running()) self.endpoint.Start(frondSync::Transport::network);
        const auto count = self.endpoint.Pipe()->DeviceToHost(static_cast<const uint8_t*>(bytes), size);
        if (count) self.lastProgress = Clock::now();
        return count;
    }

    static uint16_t Get16(const uint8_t* bytes) { return (uint16_t(bytes[0]) << 8) | bytes[1]; }
    static uint32_t Get32(const uint8_t* bytes) { return (uint32_t(Get16(bytes)) << 16) | Get16(bytes + 2); }
    static void Set16(uint8_t* bytes, uint16_t value) {
        bytes[0] = uint8_t(value >> 8);
        bytes[1] = uint8_t(value);
    }

    static uint16_t Checksum(const uint8_t* data, size_t size, uint32_t sum = 0) {
        for (size_t i = 0; i < size; i += 2) sum += Get16(data + i);
        while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
        return uint16_t(~sum);
    }

    // RFC 1624: update the checksum for each replaced address word.
    static void RewriteChecksum(uint8_t* checksum, uint32_t from, uint32_t to) {
        uint32_t sum = uint16_t(~Get16(checksum)) + uint16_t(~(from >> 16)) + uint16_t(~from) +
                       uint16_t(to >> 16) + uint16_t(to);
        sum = (sum & 0xffff) + (sum >> 16);
        sum = (sum & 0xffff) + (sum >> 16);
        Set16(checksum, uint16_t(~sum));
    }

    uint8_t* Translate(std::vector<uint8_t>& frame, bool toServer) {
        if (!registered || frame.size() < ethernetSize + 40 || Get16(frame.data() + 12) != 0x0800)
            return nullptr;
        auto* ip = frame.data() + ethernetSize;
        const size_t ipHeader = (ip[0] & 15) * 4;
        const size_t total = Get16(ip + 2);
        if ((ip[0] >> 4) != 4 || ipHeader < 20 || total < ipHeader + 20 ||
            total > frame.size() - ethernetSize || ip[9] != 6 || (Get16(ip + 6) & 0x3fff))
            return nullptr;
        auto* tcp = ip + ipHeader;
        const auto alias = ntohl(server.s_addr);
        const auto from = toServer ? host : alias;
        const auto to = toServer ? alias : host;
        auto* address = ip + (toServer ? 16 : 12);
        if (Get32(address) != from || Get16(tcp + (toServer ? 2 : 0)) != port) return nullptr;
        Set16(address, uint16_t(to >> 16));
        Set16(address + 2, uint16_t(to));
        RewriteChecksum(ip + 10, from, to);
        RewriteChecksum(tcp + 16, from, to);
        return tcp;
    }
};

}  // namespace frondNet

#endif
