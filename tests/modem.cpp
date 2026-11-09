// The emulated modem: Hayes command scripts from the Palm OS 3.1, 3.5 and 4.1 modem tables,
// the PPP server against a scripted client, and one DNS query plus one TCP connection through
// libslirp to loopback listeners. Nothing leaves the machine.

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "modem/HayesModem.h"
#include "modem/PppServer.h"
#include "net/SlirpIp.h"

using namespace frondModem;
using Bytes = std::vector<uint8_t>;

static unsigned checks, failures;
#define CHECK(condition)                                                        \
    do {                                                                        \
        ++checks;                                                               \
        if (!(condition)) {                                                     \
            ++failures;                                                         \
            fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                       \
    } while (0)

constexpr uint64_t ms = 1000000ull;

static std::string Printable(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n";
        else if (uint8_t(c) < 32 || uint8_t(c) > 126) {
            char hex[8];
            snprintf(hex, sizeof(hex), "\\x%02x", uint8_t(c));
            out += hex;
        } else out += c;
    }
    return out;
}

#define CHECK_TEXT(actual, expected)                                                       \
    do {                                                                                   \
        const std::string a_ = (actual), e_ = (expected);                                  \
        CHECK(a_ == e_);                                                                   \
        if (a_ != e_) fprintf(stderr, "  got \"%s\"\n  want \"%s\"\n", Printable(a_).c_str(), \
                              Printable(e_).c_str());                                      \
    } while (0)

// ---- Hayes command set against a recording line ----

struct FakeLine : ModemLine {
    unsigned answers{0}, hangups{0};
    bool carrier{true}, up{false};
    std::string received;
    std::deque<uint8_t> sending;
    void Answer(uint64_t) override {
        ++answers;
        up = true;
        carrier = true;
    }
    void HangUp() override {
        ++hangups;
        up = false;
        sending.clear();
    }
    void FromModem(const uint8_t* data, size_t size, uint64_t) override {
        received.append(reinterpret_cast<const char*>(data), size);
    }
    size_t ToModem(uint8_t* data, size_t max, uint64_t) override {
        size_t n = 0;
        while (n < max && !sending.empty()) {
            data[n++] = sending.front();
            sending.pop_front();
        }
        return n;
    }
    bool Carrier(uint64_t) const override { return carrier; }
};

// Drives a modem on a made-up guest clock and collects what it sends back.
struct Rig {
    HayesModem& modem;
    uint64_t now{1000 * ms};
    std::string got;
    std::function<void()> each;
    explicit Rig(HayesModem& modem) : modem(modem) {}
    void Drain() {
        uint8_t buffer[512];
        while (size_t n = modem.Read(buffer, sizeof(buffer))) got.append(reinterpret_cast<char*>(buffer), n);
    }
    void Run(uint64_t time, uint64_t step = ms) {
        for (uint64_t done = 0; done < time; done += step) {
            now += step;
            modem.Service(now);
            Drain();
            if (each) each();
        }
    }
    void Type(const std::string& text, uint64_t gap = 0) {
        for (const char c : text) {
            const uint8_t byte = uint8_t(c);
            modem.Write(&byte, 1, now);
            if (gap) Run(gap);
        }
        Drain();
    }
    std::string Take() {
        Drain();
        std::string text;
        text.swap(got);
        return text;
    }
    // Sends a command line and returns what came back.
    std::string Command(const std::string& line) {
        Take();
        Type(line + "\r");
        Run(ms);
        return Take();
    }
    // Dials and waits for CONNECT (or whatever comes back).
    std::string Dial(const std::string& line, uint64_t limit = 120000 * ms) {
        Take();
        Type(line + "\r");
        for (uint64_t waited = 0; waited < limit && modem.GetState() == HayesModem::State::Dialling; waited += 10 * ms)
            Run(10 * ms, 10 * ms);
        Run(ms);
        return Take();
    }
};

static const std::string ok = "\r\nOK\r\n";

static void TestCommands() {
    FakeLine line;
    HayesModem::Options options;
    options.dialPlan["5550001"] = HayesModem::Result::Busy;
    options.dialPlan["5550002"] = HayesModem::Result::NoAnswer;
    options.dialPlan["5550004"] = HayesModem::Result::NoDialtone;
    HayesModem modem(line, options);
    Rig rig(modem);

    // Unpowered, the modem says nothing.
    rig.Type("AT\r");
    CHECK_TEXT(rig.Take(), "");
    modem.SetPower(true, rig.now);
    CHECK(modem.GetState() == HayesModem::State::Command);

    CHECK_TEXT(rig.Command("AT"), "AT\r" + ok);
    CHECK_TEXT(rig.Command("at"), "at\r" + ok);
    // Text before AT is ignored.
    CHECK_TEXT(rig.Command("xyzAT"), "xyzAT\r" + ok);
    CHECK_TEXT(rig.Command("ATE0"), "ATE0\r" + ok);
    CHECK_TEXT(rig.Command("AT"), ok);

    // Init strings from the 3.1, 3.5 and 4.1 modem lists and the ModemMgr command table.
    for (const char* init : {"AT&FX4", "AT&FX4*NC22", "AT&FX4*NC16", "AT&F&C1&D2X4", "AT&F1&A3&B1&D2&H1&R2&N0X4",
                             "ATQ0E0V1S6=2S7=60X4M1&D0&C1", "ATZ", "AT&FE0", "AT+FCLASS=0;+MS=V34", "ATM0L0",
                             "AT &F X4 M1"}) {
        rig.Command("ATE0");
        CHECK_TEXT(rig.Command(init), ok);
    }
    rig.Command("ATE0");

    // S-registers, with ATSn? answers and range checks.
    CHECK_TEXT(rig.Command("ATS7?"), "\r\n050\r\n" + ok);
    CHECK_TEXT(rig.Command("ATS7=45S0=1"), ok);
    CHECK_TEXT(rig.Command("ATS7?S0?"), "\r\n045\r\n\r\n001\r\n" + ok);
    CHECK_TEXT(rig.Command("ATS300=1"), "\r\nERROR\r\n");
    CHECK_TEXT(rig.Command("ATS7=300"), "\r\nERROR\r\n");
    CHECK_TEXT(rig.Command("ATS12?"), "\r\n050\r\n" + ok);
    CHECK_TEXT(rig.Command("ATS2?"), "\r\n043\r\n" + ok);
    CHECK_TEXT(rig.Command("AT&F"), ok);
    CHECK_TEXT(rig.Command("ATS7?"), "ATS7?\r\r\n050\r\n" + ok);
    // A/ repeats the last line at once, with no return.
    rig.Take();
    rig.Type("A/");
    CHECK_TEXT(rig.Take(), "A/\r\n050\r\n" + ok);
    rig.Command("ATE0");
    // Backspace edits the line.
    CHECK_TEXT(rig.Command("ATS7=44\b5"), ok);
    CHECK_TEXT(rig.Command("ATS7?"), "\r\n045\r\n" + ok);
    CHECK_TEXT(rig.Command("ATI0"), "\r\n33600\r\n" + ok);
    CHECK_TEXT(rig.Command("AT&D4"), "\r\nERROR\r\n");
    CHECK_TEXT(rig.Command("ATX5"), "\r\nERROR\r\n");
    CHECK_TEXT(rig.Command("AT!"), "\r\nERROR\r\n");
    CHECK_TEXT(rig.Command("AT" + std::string(300, 'M')), "\r\nERROR\r\n");
    // ATO and ATA with no call.
    CHECK_TEXT(rig.Command("ATO"), "\r\nNO CARRIER\r\n");
    CHECK_TEXT(rig.Command("ATA"), "\r\nNO CARRIER\r\n");

    // Quiet and numeric results.
    CHECK_TEXT(rig.Command("ATQ1"), "");
    CHECK_TEXT(rig.Command("ATQ0"), ok);
    CHECK_TEXT(rig.Command("ATV0"), "0\r");
    CHECK_TEXT(rig.Command("AT!"), "4\r");
    CHECK_TEXT(rig.Command("ATS7?"), "045\r\n0\r");
    CHECK_TEXT(rig.Dial("ATX4DT555-0001"), "7\r");
    CHECK_TEXT(rig.Dial("ATX0DT5550001"), "3\r");
    CHECK_TEXT(rig.Dial("ATX4DT5550004"), "6\r");
    CHECK_TEXT(rig.Dial("ATX3DT5550004", 60000 * ms), "3\r");
    CHECK_TEXT(rig.Dial("ATX4DT5550002", 60000 * ms), "8\r");
    CHECK_TEXT(rig.Dial("ATX4DT5551234"), "1\r");
    CHECK(modem.GetState() == HayesModem::State::Online);
    modem.Drop(rig.now);
    CHECK_TEXT(rig.Take(), "3\r");
    CHECK_TEXT(rig.Command("ATV1"), ok);
    CHECK(line.answers == 1 && line.hangups == 1);

    // Plain dial: S6 for the dial tone, 7 tone digits, then ringing and training.
    rig.Command("AT&FE0");
    rig.Take();
    rig.Type("ATDT5551234\r");
    rig.Run(8000 * ms);
    CHECK_TEXT(rig.Take(), "");
    CHECK(modem.GetState() == HayesModem::State::Dialling && modem.OffHook());
    rig.Run(800 * ms);
    CHECK_TEXT(rig.Take(), "\r\nCONNECT 33600\r\n");
    CHECK(modem.GetState() == HayesModem::State::Online && line.up && line.answers == 2);

    // Pacing: 33600 bit/s is 3360 bytes a second each way.
    line.sending.assign(3360, 'x');
    rig.Run(500 * ms);
    const size_t half = rig.Take().size();
    CHECK(half > 1600 && half < 1850);
    line.sending.clear();
    rig.Run(100 * ms);
    rig.Take();
    line.received.clear();
    rig.Type(std::string(1000, 'y'));
    rig.Run(100 * ms);
    CHECK(line.received.size() > 300 && line.received.size() < 500);
    rig.Run(300 * ms);
    CHECK(line.received.size() == 1000);

    // +++ right after data isn't an escape, nor are pluses too far apart.
    rig.Type("z+++");
    rig.Run(2000 * ms);
    CHECK_TEXT(rig.Take(), "");
    rig.Type("+", 1500 * ms);
    rig.Type("+", 1500 * ms);
    rig.Type("+", 1500 * ms);
    CHECK_TEXT(rig.Take(), "");
    CHECK(modem.GetState() == HayesModem::State::Online);
    // With a second's guard either side it is; the pluses still reach the line.
    rig.Run(1100 * ms);
    line.received.clear();
    rig.Type("+++", 100 * ms);
    rig.Run(800 * ms);
    CHECK_TEXT(rig.Take(), "");
    rig.Run(200 * ms);
    CHECK_TEXT(rig.Take(), ok);
    CHECK(modem.GetState() == HayesModem::State::OnlineCommand && line.received == "+++");
    // A fourth plus inside the guard time cancels the escape.
    CHECK_TEXT(rig.Command("ATO"), "\r\nCONNECT 33600\r\n");
    rig.Run(1100 * ms);
    rig.Type("++++", 100 * ms);
    rig.Run(2000 * ms);
    CHECK_TEXT(rig.Take(), "");
    // S12 sets the guard time in fiftieths of a second.
    rig.Run(1100 * ms);
    rig.Type("+++", 50 * ms);
    rig.Run(1100 * ms);
    CHECK_TEXT(rig.Take(), ok);
    CHECK_TEXT(rig.Command("ATS12=10O"), "\r\nCONNECT 33600\r\n");
    rig.Run(250 * ms);
    rig.Type("+++", 20 * ms);
    rig.Run(250 * ms);
    CHECK_TEXT(rig.Take(), ok);
    CHECK_TEXT(rig.Command("ATDT1"), "\r\nERROR\r\n");
    CHECK_TEXT(rig.Command("ATH0"), ok);
    CHECK(modem.GetState() == HayesModem::State::Command && !line.up && line.hangups == 2);

    // ATZ in a call hangs up and restores echo.
    rig.Command("ATE0");
    CHECK_TEXT(rig.Dial("ATDT5551234"), "\r\nCONNECT 33600\r\n");
    rig.Run(1100 * ms);
    rig.Type("+++");
    rig.Run(1100 * ms);
    CHECK_TEXT(rig.Take(), ok);
    CHECK_TEXT(rig.Command("ATZ"), ok);
    CHECK(!line.up && line.hangups == 3);
    CHECK_TEXT(rig.Command("AT"), "AT\r" + ok);

    // DTR: &D0 ignores a drop, &D1 goes to command mode, &D2 hangs up.
    rig.Command("ATE0");
    rig.Command("AT&D0");
    rig.Dial("ATDT5551234");
    modem.SetDTR(false, rig.now);
    modem.SetDTR(true, rig.now);
    rig.Run(10 * ms);
    CHECK_TEXT(rig.Take(), "");
    CHECK(modem.GetState() == HayesModem::State::Online);
    modem.Drop(rig.now);
    rig.Take();
    rig.Command("AT&D1");
    rig.Dial("ATDT5551234");
    modem.SetDTR(false, rig.now);
    modem.SetDTR(true, rig.now);
    CHECK_TEXT(rig.Take(), ok);
    CHECK(modem.GetState() == HayesModem::State::OnlineCommand);
    rig.Command("ATH0");
    rig.Command("AT&D2");
    rig.Dial("ATDT5551234");
    const unsigned hangups = line.hangups;
    modem.SetDTR(false, rig.now);
    modem.SetDTR(true, rig.now);
    CHECK_TEXT(rig.Take(), "\r\nNO CARRIER\r\n");
    CHECK(modem.GetState() == HayesModem::State::Command && line.hangups == hangups + 1);

    // A key while dialling gives up, and S7 limits the wait for carrier.
    rig.Take();
    rig.Type("ATDT5551234\r");
    rig.Run(1000 * ms);
    rig.Type("x");
    CHECK_TEXT(rig.Take(), "\r\nNO CARRIER\r\n");
    CHECK(modem.GetState() == HayesModem::State::Command && !modem.OffHook());
    rig.Take();
    rig.Type("ATS7=5DT5551234\r");
    rig.Run(4900 * ms);
    CHECK_TEXT(rig.Take(), "");
    rig.Run(200 * ms);
    CHECK_TEXT(rig.Take(), "\r\nNO CARRIER\r\n");
    rig.Command("ATS7=50");

    // Dialling with ; holds the line in command mode; commas wait S8 seconds.
    rig.Take();
    rig.Type("ATDT9,;\r");
    rig.Run(4000 * ms);
    CHECK_TEXT(rig.Take(), "");
    rig.Run(200 * ms);
    CHECK_TEXT(rig.Take(), ok);
    CHECK(modem.OffHook() && modem.GetState() == HayesModem::State::Command);
    CHECK_TEXT(rig.Command("ATH0"), ok);
    CHECK(!modem.OffHook());
    // Pulse digits take longer: 700 ms plus 100 ms a pulse, and DL redials.
    CHECK_TEXT(rig.Dial("ATDP0"), "\r\nCONNECT 33600\r\n");
    modem.Drop(rig.now);
    rig.Take();
    CHECK_TEXT(rig.Dial("ATDL"), "\r\nCONNECT 33600\r\n");

    // The far end hanging up gives NO CARRIER.
    line.carrier = false;
    rig.Run(ms);
    CHECK_TEXT(rig.Take(), "\r\nNO CARRIER\r\n");
    CHECK(modem.GetState() == HayesModem::State::Command && !line.up);

    // Power off hangs up and forgets the settings.
    rig.Dial("ATDT5551234");
    const unsigned before = line.hangups;
    modem.SetPower(false, rig.now);
    CHECK(modem.GetState() == HayesModem::State::Off && line.hangups == before + 1);
    modem.SetPower(true, rig.now);
    CHECK_TEXT(rig.Command("AT"), "AT\r" + ok);
}

// ---- PPP against a scripted client ----

static uint16_t Fcs(const uint8_t* data, size_t size) {
    uint16_t fcs = 0xffff;
    for (size_t i = 0; i < size; ++i) {
        fcs ^= data[i];
        for (int bit = 0; bit < 8; ++bit) fcs = (fcs & 1) ? (fcs >> 1) ^ 0x8408 : fcs >> 1;
    }
    return fcs;
}

static uint16_t Get16(const Bytes& b, size_t at) { return uint16_t((b[at] << 8) | b[at + 1]); }
static uint32_t Get32(const Bytes& b, size_t at) { return (uint32_t(Get16(b, at)) << 16) | Get16(b, at + 2); }
static void Put16(Bytes& b, size_t at, uint16_t value) {
    b[at] = uint8_t(value >> 8);
    b[at + 1] = uint8_t(value);
}
static void Put32(Bytes& b, size_t at, uint32_t value) {
    Put16(b, at, uint16_t(value >> 16));
    Put16(b, at + 2, uint16_t(value));
}

struct Frame {
    uint16_t protocol;
    Bytes packet;
    bool header;
};

// The guest's side of PPP: frames packets and takes frames apart.
struct Client {
    uint32_t accm{0xffffffff};
    bool acfc{false}, pfc{false};
    std::deque<Frame> frames;
    Bytes current;
    bool escaped{false};
    unsigned bad{0};
    std::string raw;

    std::string Encode(uint16_t protocol, const Bytes& packet, bool corrupt = false) const {
        const bool lcp = protocol == 0xc021;
        Bytes b;
        if (lcp || !acfc) b.insert(b.end(), {0xff, 0x03});
        if (lcp || !pfc || protocol > 0xff) b.push_back(uint8_t(protocol >> 8));
        b.push_back(uint8_t(protocol));
        b.insert(b.end(), packet.begin(), packet.end());
        uint16_t fcs = Fcs(b.data(), b.size()) ^ 0xffff;
        if (corrupt) fcs ^= 0x0100;
        b.push_back(uint8_t(fcs));
        b.push_back(uint8_t(fcs >> 8));
        const uint32_t map = lcp ? 0xffffffff : accm;
        std::string out(1, '\x7e');
        for (const uint8_t c : b) {
            if (c == 0x7e || c == 0x7d || (c < 32 && (map >> c) & 1)) {
                out.push_back('\x7d');
                out.push_back(char(c ^ 0x20));
            } else {
                out.push_back(char(c));
            }
        }
        out.push_back('\x7e');
        return out;
    }
    static Bytes Control(uint8_t code, uint8_t id, const Bytes& data) {
        Bytes b{code, id, 0, 0};
        b.insert(b.end(), data.begin(), data.end());
        Put16(b, 2, uint16_t(b.size()));
        return b;
    }
    void Feed(const std::string& bytes) {
        raw += bytes;
        for (const char ch : bytes) {
            uint8_t c = uint8_t(ch);
            if (c == 0x7e) {
                if (current.size() >= 4) {
                    if (Fcs(current.data(), current.size()) != 0xf0b8) {
                        ++bad;
                    } else {
                        current.resize(current.size() - 2);
                        size_t at = 0;
                        const bool header = current[0] == 0xff && current[1] == 0x03;
                        if (header) at = 2;
                        uint16_t protocol = current[at++];
                        if (!(protocol & 1)) protocol = uint16_t(protocol << 8) | current[at++];
                        frames.push_back({protocol, Bytes(current.begin() + long(at), current.end()), header});
                    }
                }
                current.clear();
                escaped = false;
            } else if (c == 0x7d) {
                escaped = true;
            } else {
                if (escaped) c ^= 0x20;
                escaped = false;
                current.push_back(c);
            }
        }
    }
    // Takes the first frame of a protocol, dropping any frames of other protocols before it.
    bool Next(uint16_t protocol, Frame& frame) {
        while (!frames.empty()) {
            frame = frames.front();
            frames.pop_front();
            if (frame.protocol == protocol) return true;
        }
        return false;
    }
};

static Bytes Option32(uint8_t type, uint32_t value) {
    Bytes b{type, 6, 0, 0, 0, 0};
    Put32(b, 2, value);
    return b;
}
static Bytes Join(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}
static Bytes Options(const Bytes& packet) { return Bytes(packet.begin() + 4, packet.end()); }

struct FakeNet : frondNet::IpNetwork {
    std::vector<Bytes> input;
    std::deque<Bytes> output;
    unsigned polls{0};
    void Input(const uint8_t* packet, size_t size, uint64_t) override { input.emplace_back(packet, packet + size); }
    void Poll(uint64_t) override { ++polls; }
    bool Receive(Bytes& packet) override {
        if (output.empty()) return false;
        packet = output.front();
        output.pop_front();
        return true;
    }
};

// Moves bytes between a server and a client directly, without a modem.
struct Wire {
    PppServer& server;
    Client& client;
    uint64_t now{1000 * ms};
    void Send(const std::string& bytes) {
        server.FromModem(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), now);
        Pump();
    }
    void Send(uint16_t protocol, const Bytes& packet, bool corrupt = false) { Send(client.Encode(protocol, packet, corrupt)); }
    std::string Pump() {
        server.Service(now);
        std::string text;
        uint8_t buffer[512];
        while (size_t n = server.ToModem(buffer, sizeof(buffer), now)) text.append(reinterpret_cast<char*>(buffer), n);
        client.Feed(text);
        return text;
    }
    void Run(uint64_t time) {
        now += time;
        Pump();
    }
};

static void TestPpp() {
    FakeNet net;
    PppServer server(net);
    Client client;
    Wire wire{server, client};
    Frame f;

    // The Unix-style prompt takes any login and password, then PPP starts.
    server.Answer(wire.now);
    CHECK(server.GetPhase() == PppServer::Phase::Login);
    std::string text = wire.Pump();
    CHECK(text.find("login: ") != std::string::npos);
    wire.Send("\r");
    CHECK(client.raw.find("login: ", text.size()) != std::string::npos);
    wire.Send("dan\r");
    CHECK(client.raw.find("Password: ") != std::string::npos);
    CHECK(server.Login() == "dan");
    wire.Send("secret\r");
    CHECK(server.GetPhase() == PppServer::Phase::Establish);
    CHECK(client.raw.find("Starting PPP") != std::string::npos);
    CHECK(client.Next(0xc021, f) && f.packet[0] == 1 && f.header);
    // LCP goes out with every control character escaped.
    {
        const auto start = client.raw.find('\x7e');
        bool clean = true;
        for (size_t i = start; i < client.raw.size(); ++i) clean = clean && uint8_t(client.raw[i]) >= 32;
        CHECK(clean);
    }
    const Bytes serverLcp = f.packet;
    CHECK(Options(serverLcp) == Join({Option32(2, 0), Option32(5, Get32(serverLcp, 12))}));

    // The guest asks for CHAP; the server rejects asking it to authenticate.
    const Bytes guestOptions = Join({{1, 4, 0x05, 0xdc}, Option32(2, 0), Option32(5, 0x12345678), {7, 2}, {8, 2}});
    wire.Send(0xc021, Client::Control(1, 1, Join({guestOptions, {3, 5, 0xc2, 0x23, 0x05}})));
    CHECK(client.Next(0xc021, f) && f.packet[0] == 4 && f.packet[1] == 1);
    CHECK(Options(f.packet) == Bytes({3, 5, 0xc2, 0x23, 0x05}));
    // Without it, the server acks the same options back.
    wire.Send(0xc021, Client::Control(1, 2, guestOptions));
    CHECK(client.Next(0xc021, f) && f.packet[0] == 2 && f.packet[1] == 2 && Options(f.packet) == guestOptions);
    CHECK(!server.LcpOpen());
    // A frame with a bad FCS is dropped.
    wire.Send(0xc021, Client::Control(2, serverLcp[1], Options(serverLcp)), true);
    CHECK(server.BadFrames() == 1 && !server.LcpOpen() && client.frames.empty());
    wire.Send(0xc021, Client::Control(2, serverLcp[1], Options(serverLcp)));
    CHECK(server.LcpOpen() && server.GetPhase() == PppServer::Phase::Network);
    client.accm = 0;
    client.pfc = client.acfc = true;

    // IPCP goes out with the agreed address and control field compression.
    CHECK(client.Next(0x8021, f) && f.packet[0] == 1 && !f.header);
    CHECK(Options(f.packet) == Option32(3, 0x0a000202));
    const Bytes serverIpcp = f.packet;
    // Echo-Request gets the server's magic number back.
    wire.Send(0xc021, Client::Control(9, 7, {0x12, 0x34, 0x56, 0x78, 'h', 'i'}));
    CHECK(client.Next(0xc021, f) && f.packet[0] == 10 && f.packet[1] == 7);
    CHECK(Get32(f.packet, 4) == Get32(serverLcp, 12) && f.packet.size() == 10);
    // An unknown protocol (IPv6CP) gets a Protocol-Reject.
    wire.Send(0x8057, Client::Control(1, 1, {1, 10, 0, 0, 0, 0, 0, 0, 0, 1}));
    CHECK(client.Next(0xc021, f) && f.packet[0] == 8 && Get16(f.packet, 4) == 0x8057);
    // A PAP login nobody asked for is still welcomed.
    wire.Send(0xc023, {1, 3, 0, 11, 3, 'd', 'a', 'n', 2, 'p', 'w'});
    CHECK(client.Next(0xc023, f) && f.packet[0] == 2 && f.packet[1] == 3);
    CHECK(server.PapPeer() == "dan");

    // IPCP: NetBIOS and Van Jacobson are rejected, then the address and DNS are given.
    const Bytes ask = Join({Option32(3, 0), Option32(129, 0), Option32(131, 0)});
    wire.Send(0x8021, Client::Control(1, 1, Join({ask, {2, 6, 0, 0x2d, 0x0f, 0x01}, Option32(130, 0)})));
    CHECK(client.Next(0x8021, f) && f.packet[0] == 4);
    CHECK(Options(f.packet) == Join({{2, 6, 0, 0x2d, 0x0f, 0x01}, Option32(130, 0)}));
    wire.Send(0x8021, Client::Control(1, 2, ask));
    CHECK(client.Next(0x8021, f) && f.packet[0] == 3);
    const Bytes given = Join({Option32(3, 0x0a00020f), Option32(129, 0x0a000203), Option32(131, 0x0a000203)});
    CHECK(Options(f.packet) == given);
    wire.Send(0x8021, Client::Control(1, 3, given));
    CHECK(client.Next(0x8021, f) && f.packet[0] == 2);
    CHECK(!server.IpcpOpen());
    // IP before IPCP is open is dropped.
    wire.Send(0x21, Bytes(20, 0x45));
    CHECK(net.input.empty());
    wire.Send(0x8021, Client::Control(2, serverIpcp[1], Options(serverIpcp)));
    CHECK(server.IpcpOpen());

    // IP both ways, with a PFC protocol and ACCM 0: control characters go raw, flags escaped.
    Bytes packet(64);
    for (size_t i = 0; i < packet.size(); ++i) packet[i] = uint8_t(i);
    packet[0] = 0x45;
    packet[40] = 0x7e;
    packet[41] = 0x7d;
    wire.Send(0x21, packet);
    CHECK(net.input.size() == 1 && net.input[0] == packet);
    // The guest may escape more than it needs to.
    {
        Client loud = client;
        loud.accm = 0xffffffff;
        wire.Send(loud.Encode(0x21, packet));
    }
    CHECK(net.input.size() == 2 && net.input[1] == packet);
    client.raw.clear();
    net.output.push_back(packet);
    wire.Run(ms);
    CHECK(client.Next(0x21, f) && f.packet == packet && !f.header);
    CHECK(client.raw.find(std::string("\x01\x02\x03", 3)) != std::string::npos);
    CHECK(client.raw.find(std::string("\x7d\x5e\x7d\x5d", 4)) != std::string::npos);
    CHECK(client.raw.size() == 1 + 1 + 64 + 2 + 2 + 1);
    CHECK(net.polls > 0);

    // A new Configure-Request takes LCP and IPCP down until both open again.
    wire.Send(0xc021, Client::Control(1, 3, guestOptions));
    CHECK(!server.LcpOpen() && !server.IpcpOpen() && server.GetPhase() == PppServer::Phase::Establish);
    client.accm = 0xffffffff;
    client.pfc = client.acfc = false;
    CHECK(client.Next(0xc021, f) && f.packet[0] == 1);
    const Bytes again = f.packet;
    CHECK(client.Next(0xc021, f) && f.packet[0] == 2);
    wire.Send(0xc021, Client::Control(2, again[1], Options(again)));
    CHECK(server.LcpOpen() && server.GetPhase() == PppServer::Phase::Network);

    // Terminate: an ack, then the line drops once it has gone out.
    wire.Send(0xc021, Client::Control(5, 9, {}));
    CHECK(server.GetPhase() == PppServer::Phase::Terminating && server.Carrier(wire.now));
    CHECK(client.Next(0xc021, f) && f.packet[0] == 6 && f.packet[1] == 9);
    CHECK(server.Carrier(wire.now + 400 * ms) && !server.Carrier(wire.now + 600 * ms));

    // A scriptless (Windows RAS) caller: the first flag skips the prompt.
    {
        PppServer ras(net);
        Client guest;
        Wire line{ras, guest};
        ras.Answer(line.now);
        line.Pump();
        line.Send(0xc021, Client::Control(1, 1, guestOptions));
        CHECK(ras.GetPhase() == PppServer::Phase::Establish && ras.Login().empty());
        CHECK(guest.Next(0xc021, f) && f.packet[0] == 1);
        CHECK(guest.Next(0xc021, f) && f.packet[0] == 2);
        // Unanswered, the Configure-Request goes again after three seconds, ten times in all.
        line.Run(2900 * ms);
        CHECK(guest.frames.empty());
        line.Run(200 * ms);
        CHECK(guest.Next(0xc021, f) && f.packet[0] == 1);
        for (int i = 0; i < 8; ++i) line.Run(3100 * ms);
        CHECK(guest.frames.size() == 8 && ras.Carrier(line.now));
        line.Run(3100 * ms);
        CHECK(!ras.Carrier(line.now));
    }

    // A server that asks for PAP waits for it before IPCP; no prompt this time.
    {
        PppServer::Options options;
        options.loginPrompt = false;
        options.askPap = true;
        PppServer pap(net, options);
        Client guest;
        Wire line{pap, guest};
        pap.Answer(line.now);
        line.Pump();
        CHECK(guest.raw.find("login") == std::string::npos);
        CHECK(guest.Next(0xc021, f) && f.packet[0] == 1);
        CHECK(Options(f.packet) == Join({Option32(2, 0), {3, 4, 0xc0, 0x23}, Option32(5, Get32(f.packet, 16))}));
        const Bytes request = f.packet;
        line.Send(0xc021, Client::Control(1, 1, guestOptions));
        line.Send(0xc021, Client::Control(2, request[1], Options(request)));
        CHECK(pap.LcpOpen() && pap.GetPhase() == PppServer::Phase::Authenticate);
        guest.frames.clear();
        line.Run(ms);
        CHECK(guest.frames.empty());
        guest.accm = 0;
        guest.pfc = guest.acfc = true;
        line.Send(0xc023, {1, 1, 0, 12, 4, 'u', 's', 'e', 'r', 2, 'p', 'w'});
        CHECK(guest.Next(0xc023, f) && f.packet[0] == 2 && pap.PapPeer() == "user");
        CHECK(pap.GetPhase() == PppServer::Phase::Network);
        CHECK(guest.Next(0x8021, f) && f.packet[0] == 1);
    }
}

// ---- Through the modem and libslirp to loopback listeners ----

static uint16_t Sum(const Bytes& b, size_t start, size_t length, uint32_t sum = 0) {
    for (size_t i = 0; i < length; i += 2) sum += (uint16_t(b[start + i]) << 8) | (i + 1 < length ? b[start + i + 1] : 0);
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    return uint16_t(~sum);
}

static Bytes IpPacket(uint8_t protocol, const Bytes& body) {
    Bytes b(20 + body.size(), 0);
    b[0] = 0x45;
    Put16(b, 2, uint16_t(b.size()));
    b[8] = 64;
    b[9] = protocol;
    Put32(b, 12, frondNet::guestAddress);
    Put32(b, 16, frondNet::hostAddress);
    Put16(b, 10, Sum(b, 0, 20));
    std::copy(body.begin(), body.end(), b.begin() + 20);
    const uint32_t pseudo = Get16(b, 12) + Get16(b, 14) + Get16(b, 16) + Get16(b, 18) + protocol + body.size();
    const size_t at = protocol == 17 ? 26 : 36;
    Put16(b, at, Sum(b, 20, body.size(), pseudo));
    return b;
}

static Bytes Udp(uint16_t sport, uint16_t dport, const Bytes& payload) {
    Bytes b(8);
    Put16(b, 0, sport);
    Put16(b, 2, dport);
    Put16(b, 4, uint16_t(8 + payload.size()));
    b.insert(b.end(), payload.begin(), payload.end());
    return IpPacket(17, b);
}

static Bytes Tcp(uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags, const Bytes& data = {}) {
    Bytes b(20);
    Put16(b, 0, sport);
    Put16(b, 2, dport);
    Put32(b, 4, seq);
    Put32(b, 8, ack);
    b[12] = 0x50;
    b[13] = flags;
    Put16(b, 14, 8192);
    b.insert(b.end(), data.begin(), data.end());
    return IpPacket(6, b);
}

static int Listener(int type, uint16_t& port) {
    const int fd = socket(AF_INET, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return -1;
    socklen_t length = sizeof(a);
    getsockname(fd, reinterpret_cast<sockaddr*>(&a), &length);
    port = ntohs(a.sin_port);
    if (type == SOCK_STREAM) listen(fd, 1);
    return fd;
}

static void TestSlirp() {
    frondNet::SlirpIp net;
    CHECK(net.Valid());
    PppServer server(net);
    HayesModem modem(server);
    Rig rig(modem);
    Client client;
    auto post = [&](uint16_t protocol, const Bytes& packet) { rig.Type(client.Encode(protocol, packet)); };
    // Runs the guest clock 1 ms a step, with real time passing for the host sockets.
    auto until = [&](const std::function<bool()>& done) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < end) {
            if (done()) return true;
            rig.Run(ms);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        return false;
    };
    Frame f;

    modem.SetPower(true, rig.now);
    rig.Command("ATE0");
    CHECK(rig.Dial("ATDT5551234").rfind("\r\nCONNECT 33600\r\n", 0) == 0);
    rig.each = [&] {
        client.Feed(rig.got);
        rig.got.clear();
    };
    rig.Run(200 * ms);
    CHECK(client.raw.find("login: ") != std::string::npos);
    rig.Type("guest\r");
    rig.Run(100 * ms);
    CHECK(client.raw.find("Password: ") != std::string::npos);
    rig.Type("guest\r");
    CHECK(until([&] { return !client.frames.empty(); }));
    CHECK(client.Next(0xc021, f) && f.packet[0] == 1);
    const Bytes lcp = f.packet;
    post(0xc021, Client::Control(1, 1, Join({Option32(2, 0), Option32(5, 0x0badcafe)})));
    post(0xc021, Client::Control(2, lcp[1], Options(lcp)));
    CHECK(until([&] { return server.LcpOpen() && client.frames.size() >= 2; }));
    client.accm = 0;
    CHECK(client.Next(0xc021, f) && f.packet[0] == 2);
    CHECK(client.Next(0x8021, f) && f.packet[0] == 1);
    const Bytes ipcp = f.packet;
    const Bytes given = Join({Option32(3, 0x0a00020f), Option32(129, 0x0a000203)});
    post(0x8021, Client::Control(1, 1, given));
    post(0x8021, Client::Control(2, ipcp[1], Options(ipcp)));
    CHECK(until([&] { return server.IpcpOpen() && !client.frames.empty(); }));
    CHECK(client.Next(0x8021, f) && f.packet[0] == 2);

    // One DNS query for frond.test to a loopback DNS listener reached as 10.0.2.2.
    uint16_t dnsPort = 0;
    const int dns = Listener(SOCK_DGRAM, dnsPort);
    CHECK(dns >= 0);
    const Bytes query = {0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 5, 'f', 'r', 'o', 'n', 'd', 4, 't', 'e', 's', 't', 0, 0, 1, 0, 1};
    post(0x21, Udp(40000, dnsPort, query));
    Bytes asked(512);
    sockaddr_in from{};
    socklen_t fromLength = sizeof(from);
    ssize_t got = -1;
    CHECK(until([&] {
        got = recvfrom(dns, asked.data(), asked.size(), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
        return got >= 0;
    }));
    CHECK(got == ssize_t(query.size()) && std::equal(query.begin(), query.end(), asked.begin()));
    Bytes answer = query;
    answer[2] = 0x81;
    answer[3] = 0x80;
    answer[7] = 1;
    answer.insert(answer.end(), {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 10, 1, 2, 3});
    sendto(dns, answer.data(), answer.size(), 0, reinterpret_cast<sockaddr*>(&from), fromLength);
    CHECK(until([&] { return !client.frames.empty(); }));
    CHECK(client.Next(0x21, f) && f.packet.size() == 28 + answer.size() && f.packet[9] == 17);
    CHECK(Get32(f.packet, 12) == frondNet::hostAddress && Get32(f.packet, 16) == frondNet::guestAddress);
    CHECK(Get16(f.packet, 20) == dnsPort && Get16(f.packet, 22) == 40000);
    CHECK(Bytes(f.packet.begin() + 28, f.packet.end()) == answer);
    close(dns);

    // One TCP connection to a loopback listener, with a line each way.
    uint16_t tcpPort = 0;
    const int listener = Listener(SOCK_STREAM, tcpPort);
    CHECK(listener >= 0);
    uint32_t seq = 0x01000000;
    post(0x21, Tcp(40001, tcpPort, seq, 0, 0x02));
    CHECK(until([&] { return !client.frames.empty(); }));
    CHECK(client.Next(0x21, f) && f.packet[9] == 6 && (f.packet[33] & 0x12) == 0x12);
    CHECK(Get32(f.packet, 28) == seq + 1 && Sum(f.packet, 0, 20) == 0);
    const uint32_t ack = Get32(f.packet, 24) + 1;
    ++seq;
    post(0x21, Tcp(40001, tcpPort, seq, ack, 0x10));
    int peer = -1;
    CHECK(until([&] { return (peer = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0; }));
    const std::string hello = "GET / HTTP/1.0\r\n\r\n";
    post(0x21, Tcp(40001, tcpPort, seq, ack, 0x18, Bytes(hello.begin(), hello.end())));
    char buffer[256];
    ssize_t n = -1;
    CHECK(until([&] { return (n = recv(peer, buffer, sizeof(buffer), 0)) > 0; }));
    CHECK(n == ssize_t(hello.size()) && std::string(buffer, size_t(n)) == hello);
    const std::string reply = "HTTP/1.0 200 OK\r\n\r\n";
    CHECK(send(peer, reply.data(), reply.size(), 0) == ssize_t(reply.size()));
    std::string data;
    CHECK(until([&] {
        while (client.Next(0x21, f))
            if (f.packet[9] == 6) {
                const size_t start = 20 + size_t(f.packet[32] >> 4) * 4;
                if (f.packet.size() > start) data.append(f.packet.begin() + long(start), f.packet.end());
            }
        return data.size() >= reply.size();
    }));
    CHECK(data == reply);
    close(peer);
    close(listener);

    // The guest hangs up with LCP Terminate; the modem reports the line drop.
    rig.got.clear();
    rig.each = nullptr;
    post(0xc021, Client::Control(5, 2, {}));
    std::string tail;
    for (int i = 0; i < 1000 && modem.GetState() == HayesModem::State::Online; ++i) {
        rig.Run(ms);
        tail += rig.Take();
    }
    CHECK(tail.find("\r\nNO CARRIER\r\n") != std::string::npos);
    CHECK(modem.GetState() == HayesModem::State::Command && server.GetPhase() == PppServer::Phase::Idle);
}

int main() {
    TestCommands();
    const unsigned commands = checks;
    TestPpp();
    const unsigned ppp = checks - commands;
    TestSlirp();
    const unsigned slirp = checks - commands - ppp;
    if (failures) {
        fprintf(stderr, "FAIL: %u of %u modem checks\n", failures, checks);
        return 1;
    }
    printf("PASS: %u modem checks (%u AT command, %u PPP, %u slirp DNS and TCP over loopback)\n", checks, commands,
           ppp, slirp);
    return 0;
}
