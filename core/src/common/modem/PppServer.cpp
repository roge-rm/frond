#include "PppServer.h"

#include <algorithm>

namespace frondModem {

namespace {

constexpr uint64_t second = 1000000000ull;
constexpr uint16_t protocolIp = 0x0021;
constexpr uint16_t protocolLcp = 0xc021;
constexpr uint16_t protocolPap = 0xc023;
constexpr uint16_t protocolIpcp = 0x8021;
// Configure-Request resend time and tries (RFC 1661's defaults).
constexpr uint64_t restartTime = 3 * second;
constexpr unsigned maxConfigure = 10;
// A frame longer than this (a 1500-byte MRU plus header and FCS) is dropped.
constexpr size_t frameLimit = 1600;
// IP packets from the network wait while this many bytes are already queued for the modem.
constexpr size_t outLimit = 4096;
// How long the line stays up after a Terminate-Ack has gone out.
constexpr uint64_t dropDelay = second / 2;
// The login: prompt comes again this often until something is typed, as Palm OS reads and throws
// away what comes in the first seconds after CONNECT before its script waits for the prompt.
constexpr uint64_t promptRepeat = 3 * second;
// With nothing typed at the prompt for this long the server hangs up, like getty.
constexpr uint64_t loginTimeout = 60 * second;

enum : uint8_t {
    ConfigureRequest = 1,
    ConfigureAck,
    ConfigureNak,
    ConfigureReject,
    TerminateRequest,
    TerminateAck,
    CodeReject,
    ProtocolReject,
    EchoRequest,
    EchoReply,
    DiscardRequest,
};

// FCS-16 from RFC 1662, low bit first.
uint16_t Fcs(uint16_t fcs, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        fcs ^= data[i];
        for (int bit = 0; bit < 8; ++bit) fcs = (fcs & 1) ? (fcs >> 1) ^ 0x8408 : fcs >> 1;
    }
    return fcs;
}
constexpr uint16_t fcsInit = 0xffff;
constexpr uint16_t fcsGood = 0xf0b8;

uint32_t Get32(const uint8_t* at) {
    return (uint32_t(at[0]) << 24) | (uint32_t(at[1]) << 16) | (uint32_t(at[2]) << 8) | at[3];
}
void Put32(std::vector<uint8_t>& to, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) to.push_back(uint8_t(value >> shift));
}
void Option(std::vector<uint8_t>& to, uint8_t type, uint32_t value) {
    to.push_back(type);
    to.push_back(6);
    Put32(to, value);
}
void Option(std::vector<uint8_t>& to, const uint8_t* option) {
    to.insert(to.end(), option, option + option[1]);
}

// Calls each(type, option) for the options in a Configure packet's data, or returns false
// if one runs past the end.
template <typename Each>
bool EachOption(const std::vector<uint8_t>& data, Each each) {
    for (size_t i = 0; i < data.size();) {
        if (data.size() - i < 2 || data[i + 1] < 2 || data[i + 1] > data.size() - i) return false;
        each(data[i], &data[i]);
        i += data[i + 1];
    }
    return true;
}

}  // namespace

PppServer::PppServer(frondNet::IpNetwork& network) : PppServer(network, Options{}) {}

PppServer::PppServer(frondNet::IpNetwork& network, Options options)
    : network(network), options(options) {}

void PppServer::Reset() {
    inFrame = escaped = false;
    frame.clear();
    loginStage = 0;
    typed.clear();
    lcp.Reset();
    ipcp.Reset();
    askAccm = askMagic = askAddress = true;
    askAuth = options.askPap;
    ourAccm = 0;
    guestAccm = sendAccm = 0xffffffff;
    guestPfc = guestAcfc = sendPfc = sendAcfc = false;
    authenticated = false;
    out.clear();
}

void PppServer::Answer(uint64_t now) {
    Reset();
    login.clear();
    papPeer.clear();
    if (options.loginPrompt) {
        phase = Phase::Login;
        loginStart = now;
        promptAt = now + promptRepeat;
        Text("\r\n\r\nFrond dial-up\r\n\r\nlogin: ");
    } else {
        StartPpp(now);
    }
}

void PppServer::HangUp() {
    Reset();
    network.Reset();
    phase = Phase::Idle;
}

bool PppServer::Carrier(uint64_t now) const {
    if (phase == Phase::Down) return false;
    return !(phase == Phase::Terminating && out.empty() && now >= dropAt);
}

size_t PppServer::ToModem(uint8_t* data, size_t max, uint64_t now) {
    const size_t n = std::min(max, out.size());
    std::copy(out.begin(), out.begin() + long(n), data);
    out.erase(out.begin(), out.begin() + long(n));
    return n;
}

void PppServer::FromModem(const uint8_t* data, size_t size, uint64_t now) {
    for (size_t i = 0; i < size; ++i) Byte(data[i], now);
}

void PppServer::Service(uint64_t now) {
    if (phase == Phase::Login && loginStage == 0 && typed.empty()) {
        if (now >= loginStart + loginTimeout) {
            phase = Phase::Down;
            out.clear();
        } else if (now >= promptAt) {
            promptAt = now + promptRepeat;
            Text("\r\nlogin: ");
        }
    }
    if (phase == Phase::Establish && !lcp.ackedOurs && now >= lcp.retryAt) {
        if (lcp.tries >= maxConfigure) phase = Phase::Down;
        else LcpRequest(now);
    }
    if (phase == Phase::Network) {
        if (!ipcp.ackedOurs && now >= ipcp.retryAt) {
            if (ipcp.tries >= maxConfigure) phase = Phase::Down;
            else IpcpRequest(now);
        }
        Network(now);
    }
}

void PppServer::Network(uint64_t now) {
    network.Poll(now);
    if (!IpcpOpen()) return;
    std::vector<uint8_t> packet;
    while (out.size() < outLimit && network.Receive(packet)) Send(protocolIp, packet);
}

void PppServer::Text(const std::string& text) { out.insert(out.end(), text.begin(), text.end()); }

// Takes any login and password, one line each, then starts PPP.
void PppServer::LoginByte(uint8_t byte, uint64_t now) {
    if (byte == '\n') return;
    if (byte != '\r') {
        if (byte == 8 || byte == 127) {
            if (!typed.empty()) typed.pop_back();
        } else if (byte >= 32 && typed.size() < 64) {
            typed.push_back(char(byte));
        }
        return;
    }
    if (loginStage == 0) {
        if (typed.empty()) {
            promptAt = now + promptRepeat;
            Text("\r\nlogin: ");
            return;
        }
        login = typed;
        typed.clear();
        loginStage = 1;
        Text("\r\nPassword: ");
        return;
    }
    typed.clear();
    Text("\r\nStarting PPP\r\n");
    StartPpp(now);
}

void PppServer::StartPpp(uint64_t now) {
    phase = Phase::Establish;
    LcpRequest(now);
}

void PppServer::Byte(uint8_t byte, uint64_t now) {
    if (phase == Phase::Login) {
        if (byte != 0x7e) {
            LoginByte(byte, now);
            return;
        }
        StartPpp(now);
    }
    if (phase == Phase::Idle || phase == Phase::Terminating || phase == Phase::Down) return;
    if (byte == 0x7e) {
        // 0x7d then 0x7e aborts the frame.
        if (inFrame && !frame.empty() && !escaped) Frame(now);
        frame.clear();
        inFrame = true;
        escaped = false;
        return;
    }
    if (!inFrame) return;
    if (byte == 0x7d) {
        escaped = true;
        return;
    }
    if (escaped) {
        byte ^= 0x20;
        escaped = false;
    }
    if (frame.size() > frameLimit) {
        ++badFrames;
        inFrame = false;
        frame.clear();
        return;
    }
    frame.push_back(byte);
}

void PppServer::Frame(uint64_t now) {
    if (frame.size() < 4 || Fcs(fcsInit, frame.data(), frame.size()) != fcsGood) {
        ++badFrames;
        return;
    }
    size_t at = 0;
    const size_t end = frame.size() - 2;
    if (end - at >= 2 && frame[0] == 0xff && frame[1] == 0x03) at = 2;
    if (at >= end) return;
    uint16_t protocol = frame[at++];
    if (!(protocol & 1)) {
        if (at >= end) return;
        protocol = uint16_t(protocol << 8) | frame[at++];
    }
    Packet(protocol, std::vector<uint8_t>(frame.begin() + long(at), frame.begin() + long(end)), now);
}

void PppServer::Packet(uint16_t protocol, const std::vector<uint8_t>& packet, uint64_t now) {
    if (protocol == protocolLcp) {
        Lcp(packet, now);
        return;
    }
    // Everything else waits for LCP (RFC 1661 section 3.4).
    if (!LcpOpen()) return;
    if (protocol == protocolPap) {
        Pap(packet, now);
    } else if (protocol == protocolIpcp) {
        if (phase == Phase::Network) Ipcp(packet, now);
    } else if (protocol == protocolIp) {
        if (phase == Phase::Network && IpcpOpen()) network.Input(packet.data(), packet.size(), now);
    } else {
        std::vector<uint8_t> data{uint8_t(protocol >> 8), uint8_t(protocol)};
        data.insert(data.end(), packet.begin(), packet.begin() + long(std::min<size_t>(packet.size(), 1400)));
        Control(protocolLcp, ProtocolReject, ++nextId, data);
    }
}

void PppServer::LcpRequest(uint64_t now) {
    std::vector<uint8_t> data;
    if (askAccm) Option(data, 2, ourAccm);
    if (askAuth) data.insert(data.end(), {3, 4, uint8_t(protocolPap >> 8), uint8_t(protocolPap)});
    if (askMagic) Option(data, 5, magic);
    lcp.id = ++nextId;
    ++lcp.tries;
    lcp.retryAt = now + restartTime;
    Control(protocolLcp, ConfigureRequest, lcp.id, data);
}

void PppServer::IpcpRequest(uint64_t now) {
    std::vector<uint8_t> data;
    if (askAddress) Option(data, 3, frondNet::hostAddress);
    ipcp.id = ++nextId;
    ++ipcp.tries;
    ipcp.retryAt = now + restartTime;
    Control(protocolIpcp, ConfigureRequest, ipcp.id, data);
}

void PppServer::LcpOpened(uint64_t now) {
    sendAccm = guestAccm;
    sendPfc = guestPfc;
    sendAcfc = guestAcfc;
    if (askAuth && !authenticated) {
        phase = Phase::Authenticate;
        return;
    }
    phase = Phase::Network;
    IpcpRequest(now);
}

void PppServer::Lcp(const std::vector<uint8_t>& packet, uint64_t now) {
    if (packet.size() < 4) return;
    const size_t length = (size_t(packet[2]) << 8) | packet[3];
    if (length < 4 || length > packet.size()) return;
    const uint8_t code = packet[0], id = packet[1];
    const std::vector<uint8_t> data(packet.begin() + 4, packet.begin() + long(length));
    const bool wasOpen = LcpOpen();
    switch (code) {
        case ConfigureRequest: {
            if (wasOpen) {
                // The guest starts over: LCP and everything above it go down.
                lcp.Reset();
                ipcp.Reset();
                sendAccm = 0xffffffff;
                sendPfc = sendAcfc = false;
                phase = Phase::Establish;
                LcpRequest(now);
            }
            std::vector<uint8_t> nak, reject;
            uint32_t accm = 0xffffffff;
            bool pfc = false, acfc = false;
            const bool valid = EachOption(data, [&](uint8_t type, const uint8_t* option) {
                const uint8_t size = option[1];
                if (type == 1 && size == 4) {
                    // MRU: anything from 128 up is fine; the guest's TCP MSS keeps segments small.
                    if (((option[2] << 8) | option[3]) < 128) nak.insert(nak.end(), {1, 4, 0x05, 0xdc});
                } else if (type == 2 && size == 6) {
                    accm = Get32(option + 2);
                } else if (type == 5 && size == 6) {
                    if (Get32(option + 2) == 0) Option(nak, 5, magic ^ 0x5a5a5a5a);
                } else if (type == 7 && size == 2) {
                    pfc = true;
                } else if (type == 8 && size == 2) {
                    acfc = true;
                } else {
                    // Asking the server to authenticate, quality reports, callback, multilink.
                    Option(reject, option);
                }
            });
            if (!valid) return;
            if (!reject.empty()) {
                Control(protocolLcp, ConfigureReject, id, reject);
            } else if (!nak.empty()) {
                Control(protocolLcp, ConfigureNak, id, nak);
            } else {
                guestAccm = accm;
                guestPfc = pfc;
                guestAcfc = acfc;
                lcp.ackedTheirs = true;
                Control(protocolLcp, ConfigureAck, id, data);
            }
            break;
        }
        case ConfigureAck:
            if (id == lcp.id && phase == Phase::Establish) lcp.ackedOurs = true;
            break;
        case ConfigureNak:
        case ConfigureReject:
            if (id != lcp.id || phase != Phase::Establish) break;
            EachOption(data, [&](uint8_t type, const uint8_t* option) {
                if (code == ConfigureReject) {
                    if (type == 2) askAccm = false;
                    if (type == 3) askAuth = false;
                    if (type == 5) askMagic = false;
                } else if (type == 2 && option[1] == 6) {
                    ourAccm = Get32(option + 2);
                } else if (type == 3) {
                    // The guest won't do PAP; go on without a login.
                    askAuth = false;
                } else if (type == 5) {
                    magic = magic * 1103515245u + 12345u;
                }
            });
            lcp.tries = 0;
            LcpRequest(now);
            break;
        case TerminateRequest:
            Control(protocolLcp, TerminateAck, id, {});
            phase = Phase::Terminating;
            dropAt = now + dropDelay;
            return;
        case TerminateAck:
        case CodeReject:
        case ProtocolReject:
        case EchoReply:
        case DiscardRequest:
            break;
        case EchoRequest:
            if (wasOpen && data.size() >= 4) {
                std::vector<uint8_t> reply;
                Put32(reply, magic);
                reply.insert(reply.end(), data.begin() + 4, data.end());
                Control(protocolLcp, EchoReply, id, reply);
            }
            break;
        default:
            Control(protocolLcp, CodeReject, ++nextId,
                    std::vector<uint8_t>(packet.begin(), packet.begin() + long(std::min<size_t>(length, 1400))));
            break;
    }
    if (!wasOpen && LcpOpen()) LcpOpened(now);
}

void PppServer::Pap(const std::vector<uint8_t>& packet, uint64_t now) {
    if (packet.size() < 5 || packet[0] != 1) return;
    const size_t length = (size_t(packet[2]) << 8) | packet[3];
    if (length < 5 || length > packet.size() || 5u + packet[4] > length) return;
    papPeer.assign(packet.begin() + 5, packet.begin() + 5 + packet[4]);
    // Authenticate-Ack with an empty message: any peer ID and password will do.
    Control(protocolPap, 2, packet[1], {0});
    authenticated = true;
    if (phase == Phase::Authenticate) {
        phase = Phase::Network;
        IpcpRequest(now);
    }
}

void PppServer::Ipcp(const std::vector<uint8_t>& packet, uint64_t now) {
    if (packet.size() < 4) return;
    const size_t length = (size_t(packet[2]) << 8) | packet[3];
    if (length < 4 || length > packet.size()) return;
    const uint8_t code = packet[0], id = packet[1];
    const std::vector<uint8_t> data(packet.begin() + 4, packet.begin() + long(length));
    switch (code) {
        case ConfigureRequest: {
            if (IpcpOpen()) {
                ipcp.Reset();
                IpcpRequest(now);
            }
            std::vector<uint8_t> nak, reject;
            const bool valid = EachOption(data, [&](uint8_t type, const uint8_t* option) {
                const uint8_t size = option[1];
                // The guest's address, then primary and secondary DNS (RFC 1877).
                const uint32_t want = type == 3 ? frondNet::guestAddress : frondNet::dnsAddress;
                if ((type == 3 || type == 129 || type == 131) && size == 6) {
                    if (Get32(option + 2) != want) Option(nak, type, want);
                } else {
                    // Van Jacobson compression, the old IP-Addresses option, NetBIOS servers.
                    Option(reject, option);
                }
            });
            if (!valid) return;
            if (!reject.empty()) {
                Control(protocolIpcp, ConfigureReject, id, reject);
            } else if (!nak.empty()) {
                Control(protocolIpcp, ConfigureNak, id, nak);
            } else {
                ipcp.ackedTheirs = true;
                Control(protocolIpcp, ConfigureAck, id, data);
            }
            break;
        }
        case ConfigureAck:
            if (id == ipcp.id) ipcp.ackedOurs = true;
            break;
        case ConfigureNak:
        case ConfigureReject:
            if (id != ipcp.id) break;
            // The server keeps 10.0.2.2 unless the guest rejects the option outright.
            EachOption(data, [&](uint8_t type, const uint8_t*) {
                if (code == ConfigureReject && type == 3) askAddress = false;
            });
            ipcp.tries = 0;
            IpcpRequest(now);
            break;
        case TerminateRequest:
            Control(protocolIpcp, TerminateAck, id, {});
            ipcp.Reset();
            ipcp.retryAt = now + restartTime;
            break;
        case TerminateAck:
        case CodeReject:
            break;
        default:
            Control(protocolIpcp, CodeReject, ++nextId,
                    std::vector<uint8_t>(packet.begin(), packet.begin() + long(std::min<size_t>(length, 1400))));
            break;
    }
}

void PppServer::Control(uint16_t protocol, uint8_t code, uint8_t id, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> packet{code, id, uint8_t((data.size() + 4) >> 8), uint8_t(data.size() + 4)};
    packet.insert(packet.end(), data.begin(), data.end());
    Send(protocol, packet);
}

// Frames a packet. LCP always goes with the full header and every control character escaped
// (RFC 1662 section 7.2); the rest use what the guest agreed to.
void PppServer::Send(uint16_t protocol, const std::vector<uint8_t>& packet) {
    const bool lcpFrame = protocol == protocolLcp;
    std::vector<uint8_t> raw;
    raw.reserve(packet.size() + 6);
    if (lcpFrame || !sendAcfc) raw.insert(raw.end(), {0xff, 0x03});
    if (lcpFrame || !sendPfc || protocol > 0xff) raw.push_back(uint8_t(protocol >> 8));
    raw.push_back(uint8_t(protocol));
    raw.insert(raw.end(), packet.begin(), packet.end());
    const uint16_t fcs = Fcs(fcsInit, raw.data(), raw.size()) ^ 0xffff;
    raw.push_back(uint8_t(fcs));
    raw.push_back(uint8_t(fcs >> 8));
    const uint32_t accm = lcpFrame ? 0xffffffff : sendAccm;
    out.push_back(0x7e);
    for (const uint8_t byte : raw) {
        if (byte == 0x7e || byte == 0x7d || (byte < 32 && (accm >> byte) & 1)) {
            out.push_back(0x7d);
            out.push_back(byte ^ 0x20);
        } else {
            out.push_back(byte);
        }
    }
    out.push_back(0x7e);
}

}  // namespace frondModem
