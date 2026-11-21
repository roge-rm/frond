#ifndef FROND_ACTIVE_SYNC_FORWARD_H
#define FROND_ACTIVE_SYNC_FORWARD_H

#include <arpa/inet.h>
#include <libslirp.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "../sync/ActiveSync.h"

namespace frondNet {

// Map the two device-initiated TCP connections on port 990 to two slirp
// guest-forward addresses. This keeps the built-in endpoint inside Frond.
class ActiveSyncForward {
   public:
    void Configure(Slirp* slirp, uint32_t host, uint32_t network, uint32_t mask, uint32_t guest, uint32_t dns,
                   std::shared_ptr<frondSync::ActiveSyncSession> active) {
        if (!slirp || !active) return;
        this->host = host;
        this->guest = guest;
        this->session = std::move(active);
        uint32_t address = network + 1;
        for (size_t i = 0; i < channels.size(); ++i) {
            auto& channel = channels[i];
            while (address == host || address == guest || address == dns) ++address;
            if ((address & mask) != network || (address | mask) == UINT32_MAX) {
                session->Stop();
                return;
            }
            channel.address.s_addr = htonl(address++);
            channel.pipe = &session->Pipe(i);
            if (slirp_add_guestfwd(slirp, Write, &channel, &channel.address, port) != 0) {
                session->Stop();
                return;
            }
        }
        this->slirp = slirp;
    }

    bool Enabled() const { return slirp; }
    bool ControlConnected() const { return channels[0].peer != 0; }

    bool Input(std::vector<uint8_t>& frame) {
        if (!Enabled()) return true;
        auto* ip = IPv4Tcp(frame);
        if (!ip || Get32(ip + 12) != guest || Get32(ip + 16) != host) return true;
        auto* tcp = ip + (ip[0] & 15) * 4;
        if (Get16(tcp + 2) != port) return true;
        const auto source = Get16(tcp);
        auto match = std::find_if(channels.begin(), channels.end(), [source](const Channel& c) { return c.peer == source; });
        if (match == channels.end() && session->Running() && (tcp[13] & 0x12) == 0x02) {
            match = std::find_if(channels.begin(), channels.end(), [](const Channel& c) { return !c.peer; });
            if (match != channels.end()) match->peer = source;
        }
        // Unmatched sync traffic must not connect to external dccm.
        if (match == channels.end()) return false;
        Rewrite(ip, tcp, ip + 16, host, ntohl(match->address.s_addr));
        // Let slirp consume payload in a FIN packet before ending the stream.
        if (tcp[13] & 0x05) match->closing = true;
        return true;
    }

    void Output(std::vector<uint8_t>& frame) {
        if (!Enabled()) return;
        auto* ip = IPv4Tcp(frame);
        if (!ip || Get32(ip + 16) != guest) return;
        auto* tcp = ip + (ip[0] & 15) * 4;
        if (Get16(tcp) != port) return;
        for (const auto& channel : channels) {
            const auto alias = ntohl(channel.address.s_addr);
            if (channel.peer == Get16(tcp + 2) && alias == Get32(ip + 12)) {
                Rewrite(ip, tcp, ip + 12, alias, host);
                return;
            }
        }
    }

    void Poll() {
        if (!Enabled()) return;
        for (auto& channel : channels) {
            if (!channel.peer) continue;
            if (channel.closing) {
                channel.pipe->Close();
                continue;
            }
            const auto room = slirp_socket_can_recv(slirp, channel.address, port);
            if (!room) continue;
            std::array<uint8_t, 4096> bytes;
            const auto count = channel.pipe->HostToDevice(bytes.data(), std::min(room, bytes.size()));
            if (count) slirp_socket_recv(slirp, channel.address, port, bytes.data(), int(count));
        }
    }

   private:
    struct Channel {
        in_addr address{};
        frondSync::SyncPipe* pipe{};
        uint16_t peer{};
        bool closing{false};
    };
    static constexpr uint16_t port = 990;
    std::shared_ptr<frondSync::ActiveSyncSession> session;
    std::array<Channel, 2> channels;
    Slirp* slirp{};
    uint32_t host{}, guest{};

    static slirp_ssize_t Write(const void* data, size_t size, void* opaque) {
        const auto& channel = *static_cast<Channel*>(opaque);
        return channel.pipe->DeviceToHost(static_cast<const uint8_t*>(data), size);
    }

    static uint16_t Get16(const uint8_t* bytes) { return uint16_t(bytes[0]) << 8 | bytes[1]; }
    static uint32_t Get32(const uint8_t* bytes) { return uint32_t(Get16(bytes)) << 16 | Get16(bytes + 2); }
    static void Set16(uint8_t* bytes, uint16_t value) {
        bytes[0] = uint8_t(value >> 8);
        bytes[1] = uint8_t(value);
    }

    static uint8_t* IPv4Tcp(std::vector<uint8_t>& frame) {
        constexpr size_t ethernetSize = 14;
        if (frame.size() < ethernetSize + 40 || Get16(frame.data() + 12) != 0x0800) return nullptr;
        auto* ip = frame.data() + ethernetSize;
        const size_t header = (ip[0] & 15) * 4;
        const size_t total = Get16(ip + 2);
        if ((ip[0] >> 4) != 4 || header < 20 || total < header + 20 ||
            total > frame.size() - ethernetSize || ip[9] != 6 || (Get16(ip + 6) & 0x3fff)) return nullptr;
        return ip;
    }

    // RFC 1624: update the checksum for each replaced address word.
    static void Checksum(uint8_t* checksum, uint32_t from, uint32_t to) {
        uint32_t sum = uint16_t(~Get16(checksum)) + uint16_t(~(from >> 16)) + uint16_t(~from) +
                       uint16_t(to >> 16) + uint16_t(to);
        sum = (sum & 0xffff) + (sum >> 16);
        sum = (sum & 0xffff) + (sum >> 16);
        Set16(checksum, uint16_t(~sum));
    }
    static void Rewrite(uint8_t* ip, uint8_t* tcp, uint8_t* address, uint32_t from, uint32_t to) {
        Set16(address, uint16_t(to >> 16));
        Set16(address + 2, uint16_t(to));
        Checksum(ip + 10, from, to);
        Checksum(tcp + 16, from, to);
    }
};

}  // namespace frondNet

#endif
