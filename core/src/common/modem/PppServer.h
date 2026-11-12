#ifndef FROND_PPP_SERVER_H
#define FROND_PPP_SERVER_H

// The dial-up server at the far end of a modem call: an optional login: and Password: prompt,
// then PPP in async HDLC framing (RFC 1661, 1662) with LCP, PAP and IPCP, carrying the guest's
// IP packets to an IpNetwork. The guest gets 10.0.2.15 with DNS at 10.0.2.3; the server is
// 10.0.2.2. Any login and password are taken, and the first PPP flag (0x7e) skips the prompt,
// so both a "Unix" script and a scriptless "Windows RAS" connection work. Single-threaded;
// time is guest nanoseconds.

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "HayesModem.h"
#include "net/IpNetwork.h"

namespace frondModem {

class PppServer : public ModemLine {
   public:
    struct Options {
        bool loginPrompt = true;
        // Ask the guest to log in with PAP during LCP.
        bool askPap = false;
    };
    enum class Phase { Idle, Login, Establish, Authenticate, Network, Terminating, Down };

    explicit PppServer(frondNet::IpNetwork& network);
    PppServer(frondNet::IpNetwork& network, Options options);

    void Answer(uint64_t now) override;
    void HangUp() override;
    void FromModem(const uint8_t* data, size_t size, uint64_t now) override;
    size_t ToModem(uint8_t* data, size_t max, uint64_t now) override;
    bool Carrier(uint64_t now) const override;
    void Service(uint64_t now) override;

    Phase GetPhase() const { return phase; }
    bool LcpOpen() const { return lcp.ackedOurs && lcp.ackedTheirs; }
    bool IpcpOpen() const { return ipcp.ackedOurs && ipcp.ackedTheirs; }
    // Frames dropped for a bad FCS or length.
    unsigned BadFrames() const { return badFrames; }
    // Login and password as typed at the prompt, and the PAP peer ID.
    const std::string& Login() const { return login; }
    const std::string& PapPeer() const { return papPeer; }

   private:
    // One Configure-Request exchange each way for LCP or IPCP.
    struct Negotiation {
        bool ackedOurs{false}, ackedTheirs{false};
        uint8_t id{0};
        unsigned tries{0};
        uint64_t retryAt{0};
        void Reset() { *this = Negotiation{}; }
    };

    void Reset();
    void Byte(uint8_t byte, uint64_t now);
    void LoginByte(uint8_t byte, uint64_t now);
    void StartPpp(uint64_t now);
    void Frame(uint64_t now);
    void Packet(uint16_t protocol, const std::vector<uint8_t>& packet, uint64_t now);
    void Lcp(const std::vector<uint8_t>& packet, uint64_t now);
    void Ipcp(const std::vector<uint8_t>& packet, uint64_t now);
    void Pap(const std::vector<uint8_t>& packet, uint64_t now);
    void LcpRequest(uint64_t now);
    void IpcpRequest(uint64_t now);
    void LcpOpened(uint64_t now);
    void Network(uint64_t now);
    void Send(uint16_t protocol, const std::vector<uint8_t>& packet);
    void Control(uint16_t protocol, uint8_t code, uint8_t id, const std::vector<uint8_t>& data);
    void Text(const std::string& text);

    frondNet::IpNetwork& network;
    Options options;
    Phase phase{Phase::Idle};

    // Receive framing.
    bool inFrame{false}, escaped{false};
    std::vector<uint8_t> frame;
    unsigned badFrames{0};

    // Login prompt: when it started and when the login: prompt goes again.
    unsigned loginStage{0};
    uint64_t loginStart{0}, promptAt{0};
    std::string typed, login, papPeer;

    // LCP: our requested options, what the guest's Configure-Request asked of us.
    Negotiation lcp, ipcp;
    bool askAccm{true}, askMagic{true}, askAuth{false}, askAddress{true};
    uint32_t ourAccm{0}, magic{0x46726f6e};
    // What the guest can receive: agreed in its Configure-Request, used once LCP is open.
    uint32_t guestAccm{0xffffffff}, sendAccm{0xffffffff};
    bool guestPfc{false}, guestAcfc{false}, sendPfc{false}, sendAcfc{false};
    uint8_t nextId{0};
    bool authenticated{false};

    uint64_t dropAt{0};
    std::deque<uint8_t> out;
};

}  // namespace frondModem

#endif
