// The IR phone peer: a scripted IrLAP primary, written from the IrDA IrLAP 1.1, IrLMP 1.1,
// Tiny TP 1.1 and IrCOMM 1.0 specs, asks what Palm OS 3.5 and 4.1 ask when they dial through
// "IR to Phone": discovery in 6 slots, SNRM up to 115200 with a window of 1, IAS for
// IrDA:IrCOMM's Tiny TP LSAP, a 9-wire cooked IrCOMM connect with credit 1, the Hayes init and
// dial, then PPP's LCP. Also lost frames each way, the poll timer, the watchdog and DISC.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ir/IrPhone.h"
#include "ir/Sir.h"
#include "modem/PppServer.h"
#include "net/IpNetwork.h"

using namespace frondIr;
using frondModem::HayesModem;
using frondModem::PppServer;

static unsigned checks, failures;
#define CHECK(condition)                                                            \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(condition)) {                                                         \
            ++failures;                                                             \
            fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                           \
    } while (0)

constexpr uint64_t ms = 1000000ull;

static std::string Hex(const Bytes& bytes) {
    std::string out;
    char text[4];
    for (const uint8_t b : bytes) {
        snprintf(text, sizeof(text), "%02x ", b);
        out += text;
    }
    return out;
}

static Bytes Cat(Bytes a, const Bytes& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

static Bytes Text(const std::string& text) { return Bytes(text.begin(), text.end()); }

// The far side of PPP: takes the guest's packets and sends none.
class QuietNetwork : public frondNet::IpNetwork {
   public:
    void Input(const uint8_t*, size_t, uint64_t) override { ++packets; }
    void Poll(uint64_t) override {}
    bool Receive(std::vector<uint8_t>&) override { return false; }
    unsigned packets{0};
};

// ---- SIR framing ----

static void TestSir() {
    // CRC-16/X-25 of "123456789" is 0x906e: IrLAP's FCS is its complement on the wire.
    const Bytes nine = Text("123456789");
    CHECK(uint16_t(~Fcs16(nine.data(), nine.size())) == 0x906e);

    // Each special byte is escaped, the FCS goes low byte first, and the frame comes back.
    const Bytes frame{0xff, 0x93, 0xc0, 0xc1, 0x7d, 0x00};
    std::vector<uint8_t> wire;
    SirWrap(frame, 3, wire);
    const uint16_t fcs = ~Fcs16(frame.data(), frame.size());
    const Bytes head{0xff, 0xff, 0xff, 0xc0, 0xff, 0x93, 0x7d, 0xe0, 0x7d, 0xe1, 0x7d, 0x5d, 0x00};
    CHECK(Bytes(wire.begin(), wire.begin() + head.size()) == head);
    Bytes tail;
    for (const uint8_t b : {uint8_t(fcs & 0xff), uint8_t(fcs >> 8)}) {
        if (b == 0xc0 || b == 0xc1 || b == 0x7d) tail = Cat(tail, {0x7d, uint8_t(b ^ 0x20)});
        else tail.push_back(b);
    }
    tail.push_back(0xc1);
    CHECK(Bytes(wire.begin() + head.size(), wire.end()) == tail);
    SirUnwrapper unwrapper;
    bool got = false;
    for (const uint8_t b : wire) got = unwrapper.Feed(b) || got;
    CHECK(got && unwrapper.Frame() == frame);
    CHECK(unwrapper.BadFrames() == 0);

    // A flipped bit is dropped and counted; a frame cut short by a new BOF is just dropped.
    wire[5] ^= 0x01;
    got = false;
    for (const uint8_t b : wire) got = unwrapper.Feed(b) || got;
    CHECK(!got && unwrapper.BadFrames() == 1);
    const Bytes cut{0xc0, 0x01, 0x02, 0xc0};
    for (const uint8_t b : cut) unwrapper.Feed(b);
    wire[5] ^= 0x01;
    got = false;
    for (size_t i = 1; i < wire.size(); ++i) got = unwrapper.Feed(wire[i]) || got;
    CHECK(got && unwrapper.Frame() == frame && unwrapper.BadFrames() == 1);
}

// ---- A scripted primary ----

struct Primary {
    IrPhone& phone;
    PppServer& ppp;
    uint64_t now{1000 * ms};
    SirUnwrapper unwrapper;
    // The guest's device address, our connection address (shifted, C/R clear) and sequence.
    Bytes address{0x31, 0x41, 0x59, 0x26};
    uint8_t caddr{0x5a};
    uint8_t vs{0}, vr{0};
    // Peer LSAPs the guest uses, and the Tiny TP credit the guest has given the phone.
    uint8_t iasLsap{0x01}, commLsap{0x05};
    unsigned phoneCredit{0};
    std::string fromPhone;
    std::vector<Bytes> lastFrames;

    Primary(IrPhone& phone, PppServer& ppp) : phone(phone), ppp(ppp) {}

    void Send(const Bytes& frame, unsigned extraBofs = 0) {
        std::vector<uint8_t> wire;
        SirWrap(frame, extraBofs, wire);
        phone.Write(wire.data(), wire.size(), now);
    }

    // Runs the clock in 1 ms steps for up to limit, gathering frames; stops after one with F.
    std::vector<Bytes> Collect(uint64_t limit = 600 * ms, bool untilFinal = true) {
        std::vector<Bytes> frames;
        for (uint64_t waited = 0; waited < limit; waited += ms) {
            now += ms;
            phone.Service(now);
            uint8_t data[512];
            size_t n;
            bool final = false;
            while ((n = phone.Read(data, sizeof(data), now)) > 0)
                for (size_t i = 0; i < n; ++i)
                    if (unwrapper.Feed(data[i])) {
                        frames.push_back(unwrapper.Frame());
                        final = final || (unwrapper.Frame().size() >= 2 && (unwrapper.Frame()[1] & 0x10));
                    }
            if (final && untilFinal) break;
        }
        lastFrames = frames;
        return frames;
    }

    // Lets time pass with no frames.
    void Idle(uint64_t span) {
        for (uint64_t t = 0; t < span; t += ms) {
            now += ms;
            phone.Service(now);
        }
    }

    Bytes Command(uint8_t control, const Bytes& info = {}) { return Cat({uint8_t(caddr | 1), control}, info); }

    // One I frame with P set, then the phone's answer; the phone's I frames are taken in and
    // their IrCOMM data gathered.
    std::vector<Bytes> Exchange(const Bytes& info) {
        Send(Command(uint8_t(vr << 5 | 0x10 | vs << 1), info));
        vs = (vs + 1) & 7;
        return Take(Collect());
    }

    std::vector<Bytes> Poll() {
        Send(Command(uint8_t(vr << 5 | 0x11)));
        return Take(Collect());
    }

    std::vector<Bytes> Take(const std::vector<Bytes>& frames) {
        for (const Bytes& f : frames) {
            if (f.size() < 2 || (f[1] & 1)) continue;
            if (((f[1] >> 1) & 7) != vr) continue;
            vr = (vr + 1) & 7;
            Lmp(Bytes(f.begin() + 2, f.end()));
        }
        return frames;
    }

    // IrCOMM data in an LM-PDU from the phone's Tiny TP LSAP: credit, clen, parameters, data.
    void Lmp(const Bytes& pdu) {
        if (pdu.size() < 3 || (pdu[0] & 0x80) || pdu[0] != commLsap || pdu[1] != IrComm::cookedLsap) return;
        // A PDU with an SDU uses one of the phone's credits; a bare credit byte doesn't.
        if (pdu.size() < 4) return;
        if (phoneCredit) --phoneCredit;
        const size_t start = 4 + pdu[3];
        if (start < pdu.size()) fromPhone.append(pdu.begin() + start, pdu.end());
    }

    // An IrCOMM SDU with Tiny TP credit for the phone.
    Bytes Comm(const Bytes& data, uint8_t give = 1, const Bytes& params = {}) {
        phoneCredit += give;
        Bytes pdu{IrComm::cookedLsap, commLsap, give, uint8_t(params.size())};
        return Cat(Cat(pdu, params), data);
    }

    // Waits for text from the phone's modem, polling as a primary would.
    bool WaitFor(const std::string& text, unsigned polls = 400) {
        for (unsigned i = 0; i < polls && fromPhone.find(text) == std::string::npos; ++i) {
            if (phoneCredit == 0) {
                phoneCredit = 1;
                Exchange({IrComm::cookedLsap, commLsap, 0x01});
            } else {
                Poll();
            }
            Idle(20 * ms);
        }
        return fromPhone.find(text) != std::string::npos;
    }
};

static Bytes Discovery(const Bytes& from, uint8_t flags, uint8_t slot) {
    Bytes info{0x01};
    info = Cat(Cat(info, from), {0xff, 0xff, 0xff, 0xff, flags, slot, 0x00});
    // The last slot carries the guest's hints (PDA, IrCOMM), ASCII and its nickname.
    if (slot == 0xff) info = Cat(Cat(info, {0x82, 0x04, 0x00}), Text("Palm"));
    return Cat({0xff, 0x3f}, info);
}

// The guest's SNRM: what the Palm asks for (up to 115200, 500 ms, 512 bytes, window 1, 5 extra
// BOFs, 1 ms minimum turnaround), with the disconnect time given.
static Bytes Snrm(Primary& p, const Bytes& to, uint8_t disconnect = 0xff) {
    Bytes info = Cat(p.address, to);
    info.push_back(p.caddr);
    info = Cat(info, {0x01, 0x01, 0x3f, 0x82, 0x01, 0x01, 0x83, 0x01, 0x0f, 0x84, 0x01, 0x01, 0x85, 0x01, 0x08, 0x86,
                      0x01, 0x07, 0x08, 0x01, disconnect});
    return Cat({0xff, 0x93}, info);
}

static const Bytes phoneAddress{0x46, 0x72, 0x6f, 0x6e};

// Discovery, SNRM and UA, and the first poll.
static void Connect(Primary& p, uint8_t disconnect = 0xff) {
    for (uint8_t slot = 0; slot < 6; ++slot) {
        p.Send(Discovery(p.address, 0x01, slot), 10);
        p.Collect(60 * ms, false);
    }
    p.Send(Discovery(p.address, 0x01, 0xff), 10);
    p.Idle(5 * ms);
    p.Send(Snrm(p, phoneAddress, disconnect), 10);
    p.Collect();
    p.vs = p.vr = 0;
    p.Poll();
}

// LM connect from the guest's LSAP to one of ours, returning the confirm's user data.
static Bytes LmConnect(Primary& p, uint8_t from, uint8_t to, const Bytes& user) {
    const auto frames = p.Exchange(Cat({uint8_t(0x80 | to), from, 0x01, 0x00}, user));
    for (const Bytes& f : frames)
        if (f.size() >= 6 && !(f[1] & 1) && f[2] == (0x80 | from) && f[3] == to && f[4] == 0x81)
            return Bytes(f.begin() + 6, f.end());
    return {0xee};
}

// IAS GetValueByClass on the open IAS connection, returning the answer after the IAP byte.
static Bytes Ias(Primary& p, const std::string& className, const std::string& attribute) {
    Bytes request{0x00, p.iasLsap, 0x84, uint8_t(className.size())};
    request = Cat(Cat(request, Text(className)), {uint8_t(attribute.size())});
    request = Cat(request, Text(attribute));
    for (const Bytes& f : p.Exchange(request))
        if (f.size() >= 5 && !(f[1] & 1) && f[2] == p.iasLsap && f[3] == 0x00 && f[4] == 0x84)
            return Bytes(f.begin() + 5, f.end());
    return {0xee};
}

// ---- PPP over the IrCOMM data ----

static Bytes PppFrame(uint16_t protocol, const Bytes& packet) {
    Bytes body{0xff, 0x03, uint8_t(protocol >> 8), uint8_t(protocol)};
    body = Cat(body, packet);
    const uint16_t fcs = ~Fcs16(body.data(), body.size());
    body.push_back(fcs & 0xff);
    body.push_back(fcs >> 8);
    Bytes out{0x7e};
    for (const uint8_t b : body) {
        if (b < 0x20 || b == 0x7e || b == 0x7d) {
            out.push_back(0x7d);
            out.push_back(b ^ 0x20);
        } else {
            out.push_back(b);
        }
    }
    out.push_back(0x7e);
    return out;
}

// Whole PPP frames in text, unescaped and with a good FCS, without address, control and FCS.
static std::vector<Bytes> PppFrames(const std::string& text) {
    std::vector<Bytes> frames;
    Bytes current;
    bool escaped = false;
    for (const char c : text) {
        uint8_t b = uint8_t(c);
        if (b == 0x7e) {
            if (current.size() >= 6 && Fcs16(current.data(), current.size()) == fcsGood)
                frames.push_back(Bytes(current.begin() + 2, current.end() - 2));
            current.clear();
            continue;
        }
        if (b == 0x7d) {
            escaped = true;
            continue;
        }
        if (escaped) b ^= 0x20;
        escaped = false;
        current.push_back(b);
    }
    return frames;
}

// ---- The call ----

static void TestCall() {
    QuietNetwork network;
    PppServer ppp{network, PppServer::Options{false, false}};
    IrPhone::Options options;
    options.modem.handshakeNs = 2000 * ms;
    IrPhone phone{ppp, options};
    Primary p{phone, ppp};

    // Discovery: one answer in six slots, held a moment for the guest's turnaround, with the
    // phone's hints and nickname.
    std::vector<Bytes> answers;
    for (uint8_t slot = 0; slot < 6; ++slot) {
        p.Send(Discovery(p.address, 0x01, slot), 10);
        CHECK(phone.Available(p.now) == 0);
        for (const Bytes& f : p.Collect(60 * ms, false)) answers.push_back(f);
    }
    p.Send(Discovery(p.address, 0x01, 0xff), 10);
    p.Collect(20 * ms, false);
    CHECK(answers.size() == 1);
    if (answers.size() == 1) {
        const Bytes& x = answers[0];
        const Bytes head = Cat(Cat({0xfe, 0xbf, 0x01}, phoneAddress), p.address);
        CHECK(Bytes(x.begin(), x.begin() + head.size()) == head);
        CHECK(x[11] == 0x01 && x[12] < 6 && x[13] == 0x00);
        CHECK(Bytes(x.begin() + 14, x.end()) == Cat({0x90, 0x05, 0x00}, Text("Frond Phone")));
    }
    CHECK(phone.Lap().GetStats().xidReplies == 1);
    CHECK(phone.Lap().GuestNickname() == "Palm");

    // A second discovery is answered again; one with a single slot is answered in it.
    answers.clear();
    p.Send(Discovery(p.address, 0x00, 0x00), 10);
    for (const Bytes& f : p.Collect(60 * ms, false)) answers.push_back(f);
    CHECK(answers.size() == 1 && answers[0].size() > 12 && answers[0][12] == 0x00);

    // An SNRM for someone else is ignored; ours gets the UA with the agreed speed and our limits,
    // after exactly the 1 ms turnaround the guest asks for.
    p.Send(Snrm(p, {0x01, 0x02, 0x03, 0x04}), 10);
    CHECK(p.Collect(30 * ms).empty());
    p.Send(Snrm(p, phoneAddress), 10);
    const uint64_t sent = p.now;
    CHECK(phone.Lap().ReadyAt() == sent + 1 * ms);
    CHECK(phone.Available(sent + 1 * ms - 1) == 0 && phone.Available(sent + 1 * ms) > 0);
    auto frames = p.Collect();
    CHECK(frames.size() == 1);
    CHECK(p.now - sent >= 1 * ms);
    CHECK(phone.ReadyAt() == UINT64_MAX);
    if (frames.size() == 1) {
        const Bytes ua = Cat(Cat(Cat({p.caddr, 0x73}, phoneAddress), p.address),
                             {0x01, 0x01, 0x3f, 0x82, 0x01, 0x01, 0x83, 0x01, 0x3f, 0x84, 0x01, 0x7f, 0x85, 0x01,
                              0x80, 0x86, 0x01, 0x80, 0x08, 0x01, 0xff});
        CHECK(frames[0] == ua);
        if (frames[0] != ua) fprintf(stderr, "  UA %s\n", Hex(frames[0]).c_str());
    }
    const IrLap::Link& link = phone.Lap().GetLink();
    CHECK(phone.Lap().Connected());
    CHECK(link.baud == 115200 && link.dataSize == 512 && link.window == 1);
    CHECK(link.minTurnNs == 1 * ms && link.maxTurnNs == 500 * ms && link.extraBofs == 5);
    CHECK(link.disconnectNs == 40000 * ms);

    // The first poll: RR with F and nr 0, after five extra BOFs.
    p.vs = p.vr = 0;
    p.Send(p.Command(0x11));
    std::vector<uint8_t> raw;
    for (int i = 0; i < 20 && raw.empty(); ++i) {
        p.now += ms;
        uint8_t data[64];
        const size_t n = phone.Read(data, sizeof(data), p.now);
        raw.assign(data, data + n);
    }
    CHECK(raw.size() > 6 && Bytes(raw.begin(), raw.begin() + 6) == Bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xc0}));
    CHECK(raw.size() > 7 && raw[6] == p.caddr && raw[7] == 0x11);

    // IAS: connect to LSAP 0, then the guest's query for IrCOMM's Tiny TP LSAP and a few more.
    CHECK(LmConnect(p, p.iasLsap, 0x00, {}).empty());
    CHECK(Ias(p, "IrDA:IrCOMM", "IrDA:TinyTP:LsapSel") ==
          Bytes({0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, IrComm::cookedLsap}));
    CHECK(Ias(p, "IrDA:IrCOMM", "Parameters") ==
          Bytes({0x00, 0x00, 0x01, 0x00, 0x01, 0x02, 0x00, 0x06, 0x00, 0x01, 0x07, 0x01, 0x01, 0x01}));
    CHECK(Ias(p, "IrDA:IrCOMM", "IrDA:IrLMP:LsapSel") ==
          Bytes({0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, IrComm::rawLsap}));
    CHECK(Ias(p, "Device", "DeviceName") == Cat({0x00, 0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 11}, Text("Frond Phone")));
    CHECK(Ias(p, "IrLPT", "IrDA:IrLMP:LsapSel") == Bytes({0x01}));
    CHECK(Ias(p, "IrDA:IrCOMM", "IrDA:IrLMP:Nothing") == Bytes({0x02}));
    CHECK(phone.Ias().Queries().size() == 6);
    // The guest closes IAS.
    p.Exchange({0x80, p.iasLsap, 0x02, 0x01});

    // Data for an LSAP with no connection gets an LM disconnect back.
    frames = p.Exchange({0x40, 0x07, 0x41});
    bool refused = false;
    for (const Bytes& f : frames) refused = refused || (f.size() >= 6 && f[2] == 0x87 && f[3] == 0x40 && f[4] == 0x02);
    CHECK(refused);

    // Tiny TP connect to IrCOMM: credit 1, no parameters, and IrCOMM asking for 9-wire. The
    // confirm gives credit 7.
    CHECK(LmConnect(p, p.commLsap, IrComm::cookedLsap, {0x01, 0x03, 0x00, 0x01, IrComm::nineWire}) == Bytes({0x07}));
    p.phoneCredit = 1;
    CHECK(phone.Comm().GetMode() == IrComm::Mode::Cooked);
    CHECK(phone.Comm().ServiceType() == IrComm::nineWire);
    CHECK(phone.Modem().GetState() == HayesModem::State::Command);

    // The phone's first SDU uses the one credit: DCE lines with CTS and DSR up.
    frames = p.Poll();
    bool lines = false;
    for (const Bytes& f : frames)
        lines = lines || (f.size() == 9 && f[2] == p.commLsap && f[3] == IrComm::cookedLsap && f[5] == 3 && f[6] == 0x21 &&
                          f[7] == 1 && f[8] == 0x30);
    CHECK(lines && p.phoneCredit == 0);

    // The guest's line settings as the Palm sends them: 57600 8N1, no flow control, DTR and RTS.
    p.Exchange(p.Comm({}, 2,
                      {0x10, 0x04, 0x00, 0x00, 0xe1, 0x00, 0x11, 0x01, 0x03, 0x12, 0x01, 0x00, 0x13, 0x02, 0x11, 0x13,
                       0x14, 0x02, 0x13, 0x11, 0x20, 0x01, 0x0c}));
    CHECK(phone.Comm().GuestDtr());

    // Hayes init with echo on, then the Palm's init and dial.
    p.Exchange(p.Comm(Text("ATQ0V1Z\r")));
    CHECK(p.WaitFor("ATQ0V1Z\r\r\nOK\r\n"));
    p.fromPhone.clear();
    p.Exchange(p.Comm(Text("ATQ0E0\r")));
    CHECK(p.WaitFor("\r\nOK\r\n"));
    // A command split over two Tiny TP PDUs (more bit set on the first) arrives whole.
    p.fromPhone.clear();
    p.Exchange({IrComm::cookedLsap, p.commLsap, 0x80, 0x00, 'A'});
    p.Exchange({IrComm::cookedLsap, p.commLsap, 0x00, 'T', '\r'});
    CHECK(p.WaitFor("\r\nOK\r\n"));
    p.fromPhone.clear();
    p.Exchange(p.Comm(Text("ATQ0E0&D0&C1V1S7=90L1DT5551234\r")));
    CHECK(phone.Modem().GetState() == HayesModem::State::Dialling);
    CHECK(p.WaitFor("\r\nCONNECT 33600\r\n"));
    CHECK(phone.Modem().GetState() == HayesModem::State::Online);
    CHECK(phone.Comm().BytesFromGuest() > 40);

    // LCP: the server's Configure-Request comes; the guest asks for nothing and acks it.
    CHECK(p.WaitFor(std::string(1, char(0x7e))));
    for (unsigned i = 0; i < 100 && PppFrames(p.fromPhone).empty(); ++i) {
        p.Poll();
        p.Idle(20 * ms);
    }
    auto pppFrames = PppFrames(p.fromPhone);
    CHECK(!pppFrames.empty() && pppFrames[0].size() > 4 && pppFrames[0][0] == 0xc0 && pppFrames[0][1] == 0x21 &&
          pppFrames[0][2] == 0x01);
    if (!pppFrames.empty()) {
        Bytes ack = Bytes(pppFrames[0].begin() + 2, pppFrames[0].end());
        ack[0] = 0x02;
        p.Exchange(p.Comm(PppFrame(0xc021, ack)));
    }
    p.Exchange(p.Comm(PppFrame(0xc021, {0x01, 0x01, 0x00, 0x04})));
    for (unsigned i = 0; i < 100 && !ppp.LcpOpen(); ++i) {
        p.Poll();
        p.Idle(20 * ms);
    }
    CHECK(ppp.LcpOpen());

    // A lost I frame: the guest's next one is out of sequence, so the phone sends REJ with the
    // missing number and passes nothing on; both again in order go through once.
    const unsigned before = unsigned(phone.Comm().BytesFromGuest());
    const uint8_t missing = p.vs;
    p.vs = (p.vs + 1) & 7;
    frames = p.Exchange(p.Comm(Text("B"), 0));
    CHECK(frames.size() == 1 && frames[0].size() == 2 && frames[0][1] == uint8_t(0x19 | missing << 5));
    CHECK(phone.Lap().GetStats().rejects == 1);
    CHECK(phone.Comm().BytesFromGuest() == before);
    p.vs = missing;
    p.Send(p.Command(uint8_t(p.vr << 5 | p.vs << 1), p.Comm(Text("A"), 0)));
    p.vs = (p.vs + 1) & 7;
    p.Exchange(p.Comm(Text("B"), 0));
    CHECK(phone.Comm().BytesFromGuest() == before + 2);

    // The phone's answer is lost and the guest's poll timer runs out: the guest polls again with
    // the same nr and the phone sends the same I frame again.
    // An Echo-Request; the I frame carrying the reply is not taken.
    p.Send(p.Command(uint8_t(p.vr << 5 | 0x10 | p.vs << 1),
                     p.Comm(PppFrame(0xc021, {0x09, 0x07, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00}), 2)));
    p.vs = (p.vs + 1) & 7;
    Bytes lost;
    for (unsigned i = 0; i < 50 && lost.empty(); ++i) {
        for (const Bytes& f : p.Collect())
            if (!(f[1] & 1)) lost = f;
        if (lost.empty()) {
            p.Idle(20 * ms);
            p.Send(p.Command(uint8_t(p.vr << 5 | 0x11)));
        }
    }
    CHECK(!lost.empty());
    const unsigned resent = phone.Lap().GetStats().resent;
    p.Idle(500 * ms);
    frames = p.Poll();
    CHECK(frames.size() == 1 && frames[0] == lost);
    CHECK(phone.Lap().GetStats().resent == resent + 1);
    bool settled = false;
    for (unsigned i = 0; i < 20 && !settled; ++i) {
        frames = p.Poll();
        settled = frames.size() == 1 && frames[0].size() == 2 && (frames[0][1] & 0x0f) == 0x01;
    }
    CHECK(settled);

    // RNR from the guest holds the phone's I frames back; RR lets them go.
    p.Send(p.Command(uint8_t(p.vr << 5 | p.vs << 1),
                     p.Comm(PppFrame(0xc021, {0x09, 0x08, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00}), 2)));
    p.vs = (p.vs + 1) & 7;
    p.Idle(100 * ms);
    p.Send(p.Command(uint8_t(p.vr << 5 | 0x15)));
    frames = p.Collect();
    CHECK(frames.size() == 1 && frames[0].size() == 2 && (frames[0][1] & 0x0f) == 0x01);
    bool data = false;
    for (unsigned i = 0; i < 20 && !data; ++i) {
        for (const Bytes& f : p.Poll()) data = data || !(f[1] & 1);
        p.Idle(20 * ms);
    }
    CHECK(data);

    // The guest closes the call: the escape, ATH0, then DISC. The phone answers UA, drops the
    // link and powers its modem off.
    p.fromPhone.clear();
    p.Idle(1100 * ms);
    p.Exchange(p.Comm(Text("+++"), 2));
    p.Idle(1100 * ms);
    CHECK(p.WaitFor("\r\nOK\r\n"));
    CHECK(phone.Modem().GetState() == HayesModem::State::OnlineCommand);
    p.fromPhone.clear();
    p.Exchange(p.Comm(Text("ATQ0H0\r")));
    CHECK(p.WaitFor("\r\nOK\r\n"));
    p.Send(p.Command(0x53));
    frames = p.Collect();
    CHECK(frames.size() == 1 && frames[0] == Bytes({p.caddr, 0x73}));
    CHECK(!phone.Lap().Connected());
    CHECK(phone.Comm().GetMode() == IrComm::Mode::Closed);
    CHECK(phone.Modem().GetState() == HayesModem::State::Off);
    CHECK(phone.Lap().GetStats().disconnects == 1);

    // Once the link is down, a poll to the old address gets DM.
    p.Send(p.Command(0x11));
    frames = p.Collect();
    CHECK(frames.size() == 1 && frames[0] == Bytes({p.caddr, 0x1f}));
    CHECK(phone.Lap().BadFrames() == 0);
}

// ---- The watchdog, a load, 3-wire raw ----

static void TestLinkLoss() {
    QuietNetwork network;
    PppServer ppp{network};
    IrPhone phone{ppp};

    Primary p{phone, ppp};
    // A guest that stops polling: after the 3 s disconnect time the link and the call are gone.
    {
        Connect(p, 0x01);
        CHECK(phone.Lap().Connected() && phone.Lap().GetLink().disconnectNs == 3000 * ms);
        CHECK(LmConnect(p, p.commLsap, IrComm::cookedLsap, {0x01, 0x00}) == Bytes({0x07}));
        CHECK(phone.Modem().GetState() == HayesModem::State::Command);
        p.Idle(2900 * ms);
        CHECK(phone.Lap().Connected());
        p.Idle(200 * ms);
        CHECK(!phone.Lap().Connected());
        CHECK(phone.Lap().GetStats().timeouts == 1);
        CHECK(phone.Comm().GetMode() == IrComm::Mode::Closed);
        CHECK(phone.Modem().GetState() == HayesModem::State::Off);
    }

    // After a session load: no link, and a fresh discovery and connect work.
    {
        Connect(p);
        CHECK(phone.Lap().Connected());
        phone.Drop(p.now);
        CHECK(!phone.Lap().Connected());
        Connect(p);
        CHECK(phone.Lap().Connected());
        CHECK(phone.Lap().GetStats().connects == 3);

        // 3-wire raw on IrLMP's LSAP: AT and OK as they are, no Tiny TP or control channel.
        CHECK(LmConnect(p, 0x09, IrComm::rawLsap, {}).empty());
        CHECK(phone.Comm().GetMode() == IrComm::Mode::Raw);
        std::string raw;
        auto frames = p.Exchange({IrComm::rawLsap, 0x09, 'A', 'T', '\r'});
        for (unsigned i = 0; i < 50 && raw.find("OK") == std::string::npos; ++i) {
            for (const Bytes& f : frames)
                if (f.size() > 4 && !(f[1] & 1) && f[2] == 0x09 && f[3] == IrComm::rawLsap) raw.append(f.begin() + 4, f.end());
            p.Idle(20 * ms);
            frames = p.Poll();
        }
        CHECK(raw == "AT\r\r\nOK\r\n");

        // A TEST command is echoed with the addresses swapped.
        p.Send(Cat(Cat(Cat({uint8_t(p.caddr | 1), 0xf3}, p.address), phoneAddress), Text("ping")));
        frames = p.Collect();
        CHECK(frames.size() == 1 && frames[0] == Cat(Cat(Cat({p.caddr, 0xf3}, phoneAddress), p.address), Text("ping")));
    }
}

// ---- Leftovers across links, and guest-driven buffers ----

// Whether any of the frames is an I frame on the IAS connection (an IAS answer).
static bool IasAnswered(Primary& p, const std::vector<Bytes>& frames) {
    for (const Bytes& f : frames)
        if (f.size() >= 5 && !(f[1] & 1) && f[2] == p.iasLsap && f[3] == 0x00) return true;
    return false;
}

static void TestLeftovers() {
    QuietNetwork network;
    PppServer ppp{network};
    IrPhone phone{ppp};
    Primary p{phone, ppp};

    // An answer the guest never reads (it turned IR off) goes with Drop, so the core stops
    // waking for it.
    Connect(p);
    p.Send(p.Command(uint8_t(p.vr << 5 | 0x11)));
    p.Idle(5 * ms);
    CHECK(phone.ReadyAt() != UINT64_MAX && phone.Available(p.now) > 0);
    phone.Drop(p.now);
    CHECK(phone.ReadyAt() == UINT64_MAX && phone.Available(p.now) == 0);

    // The same when the watchdog ends the link; the next link gets no stale frames, and its
    // UA and first answer wait the guest's turnaround.
    Connect(p, 0x01);
    p.Send(p.Command(uint8_t(p.vr << 5 | 0x11)));
    p.Idle(3100 * ms);
    CHECK(!phone.Lap().Connected() && phone.Lap().GetStats().timeouts == 1);
    CHECK(phone.ReadyAt() == UINT64_MAX);
    bool onlyXid = true;
    for (uint8_t slot = 0; slot < 6; ++slot) {
        p.Send(Discovery(p.address, 0x01, slot), 10);
        for (const Bytes& f : p.Collect(60 * ms, false)) onlyXid = onlyXid && f.size() > 2 && f[1] == 0xbf;
    }
    CHECK(onlyXid);
    p.Send(Discovery(p.address, 0x01, 0xff), 10);
    p.Idle(5 * ms);
    p.Send(Snrm(p, phoneAddress), 10);
    CHECK(phone.ReadyAt() == p.now + 1 * ms);
    auto frames = p.Collect();
    CHECK(frames.size() == 1 && frames[0].size() > 2 && frames[0][1] == 0x73);
    p.vs = p.vr = 0;
    p.Send(p.Command(0x11));
    CHECK(phone.ReadyAt() == p.now + 1 * ms);
    p.Collect();

    // DISC while an answer is unread: the guest gets only the UA.
    p.Send(p.Command(uint8_t(p.vr << 5 | 0x11)));
    p.Idle(5 * ms);
    p.Send(p.Command(0x53));
    frames = p.Collect();
    CHECK(frames.size() == 1 && frames[0] == Bytes({p.caddr, 0x73}));

    // A guest reset mid-link sends no DISC: its discovery is ignored while the link lasts, and
    // answered once the transport drops the link as the transceiver goes off.
    Connect(p);
    CHECK(phone.Lap().Connected());
    const unsigned replies = phone.Lap().GetStats().xidReplies;
    for (uint8_t slot = 0; slot < 6; ++slot) {
        p.Send(Discovery(p.address, 0x01, slot), 10);
        p.Collect(60 * ms, false);
    }
    CHECK(phone.Lap().GetStats().xidReplies == replies && phone.Lap().Connected());
    phone.Drop(p.now);
    unsigned answers = 0;
    for (uint8_t slot = 0; slot < 6; ++slot) {
        p.Send(Discovery(p.address, 0x01, slot), 10);
        for (const Bytes& f : p.Collect(60 * ms, false)) answers += f.size() > 2 && f[1] == 0xbf;
    }
    CHECK(answers == 1 && phone.Lap().GetStats().xidReplies == replies + 1);

    // IAS: a request that never ends is dropped whole rather than kept growing, and the next
    // one is answered; only the last 16 queries are kept.
    Connect(p);
    CHECK(LmConnect(p, p.iasLsap, 0x00, {}).empty());
    bool answered = false;
    for (int i = 0; i < 8; ++i)
        answered = IasAnswered(p, p.Exchange(Cat({0x00, p.iasLsap, 0x04}, Bytes(100, 'x')))) || answered;
    answered = IasAnswered(p, p.Exchange({0x00, p.iasLsap, 0x84, 0x00, 0x00})) || answered;
    CHECK(!answered);
    for (int i = 0; i < 20; ++i)
        CHECK(Ias(p, "IrDA:IrCOMM", "IrDA:TinyTP:LsapSel") ==
              Bytes({0x00, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, IrComm::cookedLsap}));
    CHECK(phone.Ias().Queries().size() == IasServer::keptQueries);
    CHECK(Ias(p, "Device", "DeviceName").size() > 1 && phone.Ias().Queries().back().className == "Device");
    p.Exchange({0x80, p.iasLsap, 0x02, 0x01});

    // IrCOMM with an 8-byte max SDU and a flood of line polls: one DCE line setting waits, and
    // every SDU fits the guest's limit.
    CHECK(LmConnect(p, p.commLsap, IrComm::cookedLsap, {0x81, 0x03, 0x01, 0x01, 0x08, 0x03, 0x00, 0x01, IrComm::nineWire}) ==
          Bytes({0x07}));
    p.phoneCredit = 1;
    Bytes polls;
    for (int i = 0; i < 120; ++i) polls = Cat(polls, {0x22, 0x00});
    p.Exchange(p.Comm({}, 0, polls));
    p.Exchange(p.Comm(Text("AT\r"), 0, polls));
    bool fits = true, sawLines = false;
    p.fromPhone.clear();
    for (unsigned i = 0; i < 200 && p.fromPhone.find("OK") == std::string::npos; ++i) {
        if (p.phoneCredit == 0) {
            p.phoneCredit = 1;
            frames = p.Exchange({IrComm::cookedLsap, p.commLsap, 0x01});
        } else {
            frames = p.Poll();
        }
        for (const Bytes& f : frames)
            if (f.size() >= 6 && !(f[1] & 1) && f[2] == p.commLsap && f[3] == IrComm::cookedLsap) {
                fits = fits && f.size() - 5 <= 8 && f[5] <= 3;
                sawLines = sawLines || f[5] == 3;
            }
        p.Idle(20 * ms);
    }
    CHECK(fits && sawLines);
    CHECK(p.fromPhone.find("OK") != std::string::npos);
}

int main() {
    TestSir();
    const unsigned sir = checks;
    TestCall();
    const unsigned call = checks - sir;
    TestLinkLoss();
    const unsigned loss = checks - sir - call;
    TestLeftovers();
    const unsigned leftovers = checks - sir - call - loss;
    if (failures) {
        fprintf(stderr, "FAIL: %u of %u IR peer checks\n", failures, checks);
        return 1;
    }
    printf("PASS: %u IR peer checks (%u SIR, %u call, %u link loss and raw, %u leftovers and limits)\n", checks, sir,
           call, loss, leftovers);
    return 0;
}
