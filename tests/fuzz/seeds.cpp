// Writes the synthetic seed corpora for the fuzz targets: seeds DIR makes DIR/<target>/<n>.
// Every seed is made here from Frond's own encoders or by hand; none come from firmware,
// sessions or user data.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <tuple>
#include <vector>

#include "CreateZipContext.h"
#include "GzipContext.h"
#include "ir/Sir.h"
#include "session/session_file5.h"
#include "sync/Dlp.h"
#include "sync/Link.h"
#include "sync/PalmDb.h"
#include "sync/PimRecord.h"
#include "sync/PwiNote.h"
#include "sync/SyncStore.h"
#include "sync/Wbxml.h"
#include "messages.h"
#include "savestate/Chunk.h"
#include "wire.h"
#include "SeedFdp.h"

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {
fs::path out;

void Seed(const std::string& target, const Bytes& bytes) {
    const auto folder = out / target;
    fs::create_directories(folder);
    size_t n = 0;
    while (fs::exists(folder / std::to_string(n))) ++n;
    std::ofstream(folder / std::to_string(n), std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}
void Seed(const std::string& target, const std::string& text) { Seed(target, Bytes(text.begin(), text.end())); }
Bytes Cat(Bytes a, const Bytes& b) { return a.insert(a.end(), b.begin(), b.end()), a; }
void Le32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
void Be16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v >> 8)), b.push_back(uint8_t(v)); }

// HDLC framing with the PPP FCS, every control byte escaped.
Bytes Hdlc(uint16_t protocol, const Bytes& packet) {
    Bytes body{0xff, 0x03};
    Be16(body, protocol);
    body.insert(body.end(), packet.begin(), packet.end());
    uint16_t fcs = 0xffff;
    for (uint8_t c : body) {
        fcs ^= c;
        for (int i = 0; i < 8; ++i) fcs = (fcs & 1) ? (fcs >> 1) ^ 0x8408 : fcs >> 1;
    }
    fcs ^= 0xffff;
    body.push_back(uint8_t(fcs)), body.push_back(uint8_t(fcs >> 8));
    Bytes out{0x7e};
    for (uint8_t c : body) {
        if (c < 0x20 || c == 0x7d || c == 0x7e) out.push_back(0x7d), out.push_back(c ^ 0x20);
        else out.push_back(c);
    }
    out.push_back(0x7e);
    return out;
}

Bytes PalmDbFile(bool resource) {
    frondSync::PalmDb db;
    db.name = resource ? "Fuzz App" : "FuzzDB";
    db.attributes = resource ? frondSync::dbResource : 0;
    db.type = resource ? 0x6170706c : 0x44415441;
    db.creator = 0x46757a7a;
    db.appInfo = {1, 2, 3, 4};
    if (resource) db.records.push_back({0, 0, 0x636f6465, 1, Bytes(32, 0x4e)});
    else for (uint32_t i = 1; i <= 3; ++i) db.records.push_back({i, uint8_t(i), 0, 0, Bytes(i * 4, uint8_t('a' + i))});
    Bytes file;
    db.Write(file);
    return file;
}

void Be32(Bytes& b, uint32_t v) { Be16(b, uint16_t(v >> 16)), Be16(b, uint16_t(v)); }

// An Ethernet frame from the guest (MAC 02:46:72:6f:6e:64) to the router, with an IPv4 packet.
Bytes Ipv4(uint8_t protocol, uint32_t from, uint32_t to, const Bytes& body) {
    Bytes ip{0x45, 0};
    Be16(ip, uint16_t(20 + body.size())), Be16(ip, 1), Be16(ip, 0x4000);
    ip.push_back(64), ip.push_back(protocol), Be16(ip, 0);
    Be32(ip, from), Be32(ip, to);
    return Cat(ip, body);
}
Bytes Ether(const Bytes& payload, uint16_t type = 0x0800, bool broadcast = false) {
    Bytes e = broadcast ? Bytes(6, 0xff) : Bytes{0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
    e.insert(e.end(), {0x02, 0x46, 0x72, 0x6f, 0x6e, 0x64});
    Be16(e, type);
    return Cat(e, payload);
}
Bytes Udp(uint16_t from, uint16_t to, const Bytes& payload) {
    Bytes u;
    Be16(u, from), Be16(u, to), Be16(u, uint16_t(8 + payload.size())), Be16(u, 0);
    return Cat(u, payload);
}
Bytes TcpSyn(uint16_t from, uint16_t to) {
    Bytes t;
    Be16(t, from), Be16(t, to), Be32(t, 1000), Be32(t, 0);
    t.push_back(5 << 4), t.push_back(0x02), Be16(t, 8192), Be16(t, 0), Be16(t, 0);
    return t;
}
Bytes Dhcp(uint8_t type) {
    Bytes b(240, 0);
    b[0] = 1, b[1] = 1, b[2] = 6, b[4] = 0x12, b[10] = 0x80;
    const uint8_t mac[] = {0x02, 0x46, 0x72, 0x6f, 0x6e, 0x64};
    std::copy(mac, mac + 6, b.begin() + 28);
    b[236] = 0x63, b[237] = 0x82, b[238] = 0x53, b[239] = 0x63;
    b.insert(b.end(), {53, 1, type, 255});
    return b;
}
Bytes DnsQuery() {
    Bytes q{0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 5, 'f', 'r', 'o', 'n', 'd', 4, 't', 'e', 's', 't', 0, 0, 1, 0, 1};
    return q;
}

// GDB remote packets with their checksums.
std::string Gdb(const std::string& packet) {
    uint8_t sum = 0;
    for (char c : packet) sum = uint8_t(sum + uint8_t(c));
    char tail[4];
    std::snprintf(tail, sizeof tail, "#%02x", sum);
    return "$" + packet + tail;
}
std::string Hex(const std::string& text) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : text) out += digits[c >> 4], out += digits[c & 15];
    return out;
}

}  // namespace

void StorageSeeds(const std::function<void(const std::string&, const std::vector<uint8_t>&)>& seed);

int main(int argc, char** argv) {
    using namespace frondSync;
    if (argc != 2) return 2;
    out = argv[1];
    fs::remove_all(out);

    for (const char* s : {"ATZ\r", "AT&F E0 V1 Q0 X4 S0=1\r", "ATDT5551234\r", "ATDT1234\r\x01\x02+++", "ATI3\rATS7=30\rATH0\r",
                          "AT&C1&D2\rATDP*99#\rATA\r", "ATX3DT9,5551234;\rATO\r"})
        Seed("hayes-modem", s);

    const Bytes lcp{1, 1, 0, 14, 2, 6, 0, 0, 0, 0, 5, 6, 0x12, 0x34, 0x56, 0x78};
    const Bytes ipcp{1, 1, 0, 10, 3, 6, 0, 0, 0, 0};
    const Bytes pap{1, 1, 0, 12, 4, 'u', 's', 'e', 'r', 2, 'p', 'w'};
    Seed("ppp-server", Cat(Cat(Hdlc(0xc021, lcp), Hdlc(0xc023, pap)), Hdlc(0x8021, ipcp)));
    {
        const std::string login = "guest\rpassword\rppp\r";
        Seed("ppp-server", Cat(Bytes(login.begin(), login.end()), Hdlc(0xc021, lcp)));
    }
    Seed("ppp-server", Hdlc(0x0021, Bytes(28, 0x45)));

    {
        Bytes ip{0x45, 0, 0, 40, 0, 1, 0, 0, 64, 17, 0, 0, 10, 0, 2, 15, 10, 0, 2, 2};
        Bytes udp;
        Be16(udp, 14237), Be16(udp, 14237), Be16(udp, 20), Be16(udp, 0);
        Bytes payload{0xfa, 0xde, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        Seed("netsync-wakeup", Cat(Bytes{0}, Cat(Cat(ip, udp), payload)));
        payload[0] = 0xde, payload[1] = 0xfa;
        Seed("netsync-wakeup", Cat(Bytes{1}, Cat(Cat(ip, udp), payload)));
    }

    {
        // SLP: a CMP wakeup in PADP, then a DLP answer. NetSync: the 0x90, 0x92 and 0x93 messages.
        const Bytes cmp{1, 0x80, 0, 0, 1, 1, 0, 0, 0x96, 0, 0, 0, 0, 0};
        const Bytes padp = Cat(Bytes{1, 0xc0, 0, uint8_t(cmp.size())}, cmp);
        Seed("hotsync-link", Cat(SlpPacket(3, 3, 2, 0xff, padp), SlpPacket(3, 3, 2, 1, Bytes{2, 0xc0, 0, 0})));
        auto net = [](uint8_t id, const Bytes& body) {
            Bytes b{1, id};
            Be16(b, 0), Be16(b, uint16_t(body.size()));
            return Cat(b, body);
        };
        const Bytes m90 = Cat(Bytes{0x90}, Bytes(21, 1)), m92 = Cat(Bytes{0x92}, Bytes(49, 2)), m93 = Cat(Bytes{0x93}, Bytes(7, 3));
        Seed("hotsync-link", Cat(Cat(Cat(net(0xff, m90), net(0xff, m92)), net(0xff, m93)), net(1, Bytes{0x90, 1, 0, 0})));
        Seed("hotsync-link", Bytes{0x55, 0xbe, 0xef, 0xed, 3, 3, 2, 0, 0x10, 1, 0});
    }

    Seed("dlp", DlpRequest(dlpReadUserInfo, {}));
    Seed("dlp", DlpRequest(dlpOpenDB, {{0x20, {0, 0x80, 'M', 'e', 'm', 'o', 'D', 'B', 0}}}));
    Seed("dlp", DlpResponse(dlpReadUserInfo, 0, {{0x20, Bytes(30, 1)}}));
    Seed("dlp", DlpResponse(dlpReadDBList, 0, {{0x20, Bytes(300, 2)}, {0x21, {1, 2}}}));

    {
        // Answers in the order a HotSync asks: system info, user info, storage, DB list.
        Bytes s;
        for (const auto& m : {DlpResponse(dlpReadSysInfo, 0, {{0x20, Bytes(14, 0)}, {0x21, {1, 2, 1, 4}}}),
                              DlpResponse(dlpReadUserInfo, 0, {{0x20, Cat(Bytes(29, 0), Bytes{5, 0, 'F', 'u', 'z', 'z', 0})}}),
                              DlpResponse(dlpReadStorageInfo, 0, {{0x20, Bytes(40, 0)}}),
                              DlpResponse(dlpReadDBList, 0, {{0x20, Bytes(60, 0)}}), DlpResponse(dlpEndOfSync, 0, {})})
            s = Cat(s, m);
        Seed("hotsync-session", s);
    }

    {
        Bytes info(16, 0x11);
        Le32(info, 0), Le32(info, 0), Le32(info, 4);
        for (char c : std::string("Fuzz")) info.push_back(uint8_t(c)), info.push_back(0);
        info.push_back(0), info.push_back(0);
        Bytes rest(20, 0);
        info = Cat(info, rest);
        for (int i = 0; i < 2; ++i) Le32(info, 2), info.push_back('x'), info.push_back('y');
        info.push_back(0);
        Le32(info, 1), Le32(info, 0), Le32(info, 0), Le32(info, 0);
        while (info.size() < 70) info.push_back(0);
        Bytes control;
        Le32(control, 0), Le32(control, 6), Le32(control, 4), Le32(control, uint32_t(info.size()));
        control = Cat(control, info);
        Bytes rapi;
        Le32(rapi, 1);
        Seed("activesync-connect", Cat(Cat(control, rapi), Bytes{4}));
    }

    {
        Bytes reply;
        Le32(reply, 8), Le32(reply, 0), Le32(reply, 0);
        Seed("rapi", Cat(Cat(reply, reply), Bytes{0, 2, 3}));
        Bytes list;
        Le32(list, 24), Le32(list, 0), Le32(list, 1), Le32(list, 4), Le32(list, 0x10), Le32(list, 'a'), Le32(list, 0);
        Seed("rapi", Cat(list, Bytes{5}));
    }

    WbxmlNode folderSync(7, 0x16);
    folderSync.Add(7, 0x12, "0");
    const auto folderBody = EncodeWbxml(folderSync);
    WbxmlNode sync(0, 0x05);
    auto& collection = sync.Add(0, 0x1c).Add(0, 0x0f);
    collection.Add(0, 0x0b, "1");
    collection.Add(0, 0x12, "contacts");
    auto& add = collection.Add(0, 0x16).Add(0, 0x07);
    add.Add(0, 0x0c, "client1");
    add.Add(0, 0x1d).Add(1, 0x1f, "Doe");
    const auto syncBody = EncodeWbxml(sync);
    Seed("wbxml", folderBody);
    Seed("wbxml", syncBody);
    Seed("wbxml", Bytes{3, 1, 106, 0, 0, 7, 0x56, 0x52, 3, '0', 0, 1, 1});

    auto post = [](const std::string& cmd, const Bytes& body) {
        std::string head = "POST /Microsoft-Server-ActiveSync?User=fuzz&DeviceId=1&DeviceType=PocketPC&Cmd=" + cmd +
                           " HTTP/1.1\r\nMS-ASProtocolVersion: 2.5\r\nContent-Type: application/vnd.ms-sync.wbxml\r\n"
                           "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        return Cat(Bytes(head.begin(), head.end()), body);
    };
    Seed("airsync-http", std::string("OPTIONS /Microsoft-Server-ActiveSync HTTP/1.1\r\nHost: frond\r\n\r\n"));
    Seed("airsync-http", post("FolderSync", folderBody));
    Seed("airsync-http", Cat(post("FolderSync", folderBody), post("Sync", syncBody)));

    {
        using namespace frond::link;
        Bytes hello;
        putHeader(hello, FrameMode::Noise);
        Message m{MsgType::Hello, 0, encodeHello({1, 1, "fuzz"})};
        hello = Cat(hello, encodeMessage(m));
        Bytes frames;
        writeFrame(frames, hello);
        writeFrame(frames, encodeMessage({MsgType::Open, 2, encodeOpen({ChannelKind::Serial, "m515", "dev"})}));
        writeFrame(frames, encodeMessage({MsgType::Data, 2, Bytes(40, 7)}));
        writeFrame(frames, refuseFrame(RefuseReason::WrongCode));
        Seed("link-wire", frames);
        Bytes pair;
        putHeader(pair, FrameMode::Pair);
        pair = Cat(pair, Bytes(80, 0x33));
        Bytes framed;
        writeFrame(framed, pair);
        Seed("link-pairing", framed);
        Bytes noise;
        putHeader(noise, FrameMode::Noise);
        noise = Cat(noise, Bytes(16 + 32 + 48, 0x44));
        framed.clear();
        writeFrame(framed, noise);
        Seed("link-handshake", framed);
        Seed("link-mux", Bytes{6, 2, 0, 0, 8, 1, 2, 3, 4, 5, 6, 7, 8, 7, 2, 0, 0, 0, 1, 0});
    }

    Seed("palm-db", PalmDbFile(false));
    Seed("palm-db", PalmDbFile(true));

    {
        for (int kind = 0; kind < 4; ++kind)
            for (int format = 0; format < 2; ++format) {
                PimRecord r;
                r.kind = PimKind(kind);
                r.fields = {{"text", "Fuzz note"}, {"lastName", "Doe"}, {"firstName", "Jo"}, {"description", "Event"},
                            {"date", "2026-11-25"}, {"startTime", "09:00"}, {"endTime", "10:00"}, {"priority", "1"}};
                Bytes data;
                std::string error;
                if (EncodePalmPim(r, PalmPimFormat(format), data, error))
                    Seed("pim-record", Cat(data, Bytes{uint8_t(format), uint8_t(kind)}));
            }
        if (auto note = WritePwiNote("Fuzz note\nsecond line")) Seed("pim-record", Cat(*note, Bytes{0, 3}));
    }

    {
        CreateZipContext zip;
        zip.Initialize(6);
        auto pdb = PalmDbFile(false);
        zip.AddEntry("app.pdb");
        zip.WriteData(pdb.data(), pdb.size());
        auto prc = PalmDbFile(true);
        zip.AddEntry("dir/app.prc");
        zip.WriteData(prc.data(), prc.size());
        const auto* z = static_cast<const uint8_t*>(zip.GetZipData());
        const Bytes zipped(z, z + zip.GetZipDataSize());
        Seed("zip", zipped);
        Seed("installer", Cat(zipped, Bytes{1, 1}));
        Bytes cab{'M', 'S', 'C', 'F', 0, 0, 0, 0};
        Le32(cab, 60), Le32(cab, 0), Le32(cab, 44), Le32(cab, 0);
        cab.push_back(3), cab.push_back(1), Le32(cab, 0x00010001), Le32(cab, 0);
        while (cab.size() < 60) cab.push_back(0);
        Seed("installer", Cat(cab, Bytes{0, 2}));
        Seed("installer", Cat(Cat(Bytes{'M', 'Z', 0x90, 0}, Bytes(200, 0)), Cat(cab, Bytes{0, 0})));
        Seed("installer", Cat(pdb, Bytes{0, 4}));
    }

    {
        const std::string text(3000, 'q');
        GzipContext gzip(text.data(), text.size());
        while (gzip.GetState() == 0 || gzip.GetState() == 1) gzip.Continue();
        Seed("gunzip", Bytes(gzip.GetGzipData(), gzip.GetGzipData() + gzip.GetGzipSize()));
    }

    {
        std::vector<uint8_t> memory(4096, 0), nor(2048, 0x5a);
        for (size_t i = 0; i < memory.size(); i += 64) memory[i] = uint8_t(i);
        SessionFile5 session;
        session.SetDeviceName("m515").SetRamSize(4096).SetMemory(memory.size(), memory.data()).SetNor(nor.size(), nor.data())
            .SetSavestate(16, nor.data());
        if (session.Serialize()) {
            const auto* p = static_cast<const uint8_t*>(session.GetSerializedSession());
            Seed("session-file", Bytes(p, p + session.GetSerializedSessionSize()));
        }
    }

    {
        // IrLAP discovery (XID command, 8 slots), then SNRM and an I frame, SIR wrapped.
        Bytes sir;
        const Bytes xid{0xff, 0x3f, 0x01, 0x11, 0x22, 0x33, 0x44, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00};
        frondIr::SirWrap(xid, 0, sir);
        Bytes snrm{0xff, 0x93, 0x11, 0x22, 0x33, 0x44, 0x01, 0x02, 0x03, 0x04, 0x02, 0x01, 0x01, 0x01, 0x01, 0x02, 0x82,
                   0x01, 0x3f, 0x83, 0x01, 0x3f, 0x84, 0x01, 0x01, 0x85, 0x01, 0x80, 0x86, 0x01, 0x03};
        frondIr::SirWrap(snrm, 0, sir);
        frondIr::SirWrap(Bytes{0x02, 0x10, 0x00, 0x00, 0x01, 0x00}, 0, sir);
        Seed("irda-phone", sir);
    }

    Seed("rom-info", Bytes(512, 0));
    Seed("rx1950-nand", Bytes(0xa5a + 64, 0xff));

    {
        // Profile and library databases made by the sync store itself.
        const auto root = out / ".profile-root";
        SyncLibrary library;
        std::string error;
        ProfileInfo info;
        if (library.Open(root.string(), error) && library.CreateProfile("Fuzz", info, error)) {
            SyncProfile profile;
            if (profile.Open(library.ProfilePath(info), error)) {
                InstallItem item;
                profile.QueueInstall("", "app.pdb", PalmDbFile(false), item, error);
                PimRecord memo;
                memo.fields["text"] = "hello";
                profile.SavePimRecord(memo, error);
                profile.Close();
            }
            library.Close();
            auto file = [](const fs::path& p, uint8_t flag) {
                std::ifstream in(p, std::ios::binary);
                Bytes b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                b.push_back(flag);
                return b;
            };
            Seed("sync-profile", file(root / "profiles.db", 1));
            Seed("sync-profile", file(library.ProfilePath(info) + "/profile.db", 0));
        }
        fs::remove_all(root);
    }

    {
        // The router: ARP, DHCP, DNS, ICMP, the Network HotSync wakeup and TCP to HotSync and ActiveSync.
        const uint32_t guest = 0x0a00020f, host = 0x0a000202, dns = 0x0a000203;
        Bytes arp{0, 1, 8, 0, 6, 4, 0, 1, 0x02, 0x46, 0x72, 0x6f, 0x6e, 0x64, 10, 0, 2, 15, 0, 0, 0, 0, 0, 0, 10, 0, 2, 2};
        const Bytes dhcp = Ether(Ipv4(17, 0, 0xffffffff, Udp(68, 67, Dhcp(1))), 0x0800, true);
        const Bytes dnsFrame = Ether(Ipv4(17, guest, dns, Udp(1234, 53, DnsQuery())));
        const Bytes ping = Ether(Ipv4(1, guest, host, Bytes{8, 0, 0, 0, 0, 1, 0, 1, 'p', 'i', 'n', 'g'}));
        const Bytes wakeup = Ether(Ipv4(17, guest, host, Udp(14237, 14237, Bytes{0xfa, 0xde, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0})));
        const Bytes hotsync = Ether(Ipv4(6, guest, host, TcpSyn(1025, 14238)));
        const Bytes activeSync = Ether(Ipv4(6, guest, host, TcpSyn(1026, 990)));
        const Bytes airSync = Ether(Ipv4(6, guest, host, TcpSyn(1027, 26675)));
        for (unsigned mode = 0; mode < 3; ++mode) {
            Fdp f;
            f.Int(mode, 0, 2).Bool(true);
            if (mode == 2) f.Bool(true);
            const std::vector<Bytes> frames = mode == 0 ? std::vector<Bytes>{Ether(arp, 0x0806, true), dhcp, dnsFrame, ping}
                                              : mode == 1 ? std::vector<Bytes>{dhcp, wakeup, hotsync}
                                                          : std::vector<Bytes>{dhcp, activeSync, airSync};
            for (const auto& frame : frames) f.Int(frame.size(), 14, 1518).Data(frame).Bool(true).Int(50000000, 0, 2000000000ull);
            Seed("wifi-router", f.Build());
        }
        Fdp f;
        f.Bool(true).Bool(false);
        for (const auto& packet : {Ipv4(17, guest, dns, Udp(1234, 53, DnsQuery())), Ipv4(1, guest, host, Bytes{8, 0, 0, 0, 0, 1, 0, 1}),
                                   Ipv4(17, guest, host, Udp(14237, 14237, Bytes{0xfa, 0xde, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0})),
                                   Ipv4(6, guest, host, TcpSyn(1025, 14238)), Ipv4(6, guest, 0x08080808, TcpSyn(1026, 80))})
            f.Int(packet.size(), 20, 1500).Data(packet).Bool(true).Int(50000000, 0, 2000000000ull).Int(1, 0, 63);
        Seed("slirp-ip", f.Build());
    }

    {
        // RNDIS bring-up as a device answers it: INIT, the MAC and frame size queries, the filter, then a frame.
        auto completion = [](uint32_t type, uint32_t id, const Bytes& rest) {
            Bytes m;
            Le32(m, type | 0x80000000u), Le32(m, uint32_t(16 + rest.size())), Le32(m, id), Le32(m, 0);
            return Cat(m, rest);
        };
        Bytes init;
        for (uint32_t v : {1u, 0u, 1u, 0u, 1u, 0x4000u, 3u, 0u, 0u}) Le32(init, v);
        Bytes mac;
        Le32(mac, 6), Le32(mac, 16), mac.insert(mac.end(), {0x02, 0x11, 0x22, 0x33, 0x44, 0x55});
        Bytes frameSize;
        Le32(frameSize, 4), Le32(frameSize, 16), Le32(frameSize, 1500);
        Bytes packet;
        const Bytes frame = Ether(Ipv4(17, 0x0a00020f, 0x0a000202, Udp(1, 2, Bytes(8, 1))));
        Le32(packet, 1), Le32(packet, uint32_t(44 + frame.size())), Le32(packet, 36), Le32(packet, uint32_t(frame.size()));
        packet.resize(44, 0);
        packet = Cat(packet, frame);
        Fdp f;
        f.Int(64, 8, 512);
        const std::vector<Bytes> replies{completion(2, 1, init), completion(4, 2, mac), completion(4, 3, frameSize), completion(5, 4, {})};
        for (const auto& reply : replies) {
            f.Int(1000, 0, 6000000000ull).Int(0, 0, 8).Bool(true);  // the command goes out
            f.Int(1000, 0, 6000000000ull).Int(2, 0, 8).Int(8, 0, 16).Data(Bytes{1, 0, 0, 0, 0, 0, 0, 0});
            f.Int(1000, 0, 6000000000ull).Int(1, 0, 8).Int(reply.size(), 0, 1100).Data(reply).Bool(true);
        }
        f.Int(1000, 0, 6000000000ull).Int(4, 0, 8).Int(packet.size(), 0, 0x4000).Data(packet);
        f.Int(1000, 0, 6000000000ull).Int(6, 0, 8).Int(frame.size(), 0, 1600).Data(frame);
        f.Int(1000, 0, 6000000000ull).Int(5, 0, 8).Int(64, 1, 1024);
        Seed("rndis-host", f.Build());
    }

    {
        // USB: a device descriptor and a configuration, Palm's vendor-class HotSync port or RNDIS with CDC data.
        Bytes device{18, 1, 0x10, 0x01, 0, 0, 0, 16, 0x30, 0x08, 0x02, 0x00, 0, 1, 1, 2, 3, 1};
        auto config = [](const Bytes& interfaces, uint8_t count) {
            Bytes c{9, 2, 0, 0, count, 1, 0, 0x80, 50};
            c = Cat(c, interfaces);
            c[2] = uint8_t(c.size()), c[3] = uint8_t(c.size() >> 8);
            return c;
        };
        const Bytes palm{9, 4, 0, 0, 2, 0xff, 0, 0, 0, 7, 5, 0x82, 2, 64, 0, 0, 7, 5, 0x02, 2, 64, 0, 0};
        const Bytes rndis{9, 4, 0, 0, 1, 0xef, 1, 1, 0, 7, 5, 0x81, 3, 8, 0, 1,
                          9, 4, 1, 0, 2, 0x0a, 0, 0, 0, 7, 5, 0x82, 2, 64, 0, 0, 7, 5, 0x03, 2, 64, 0, 0};
        for (const auto& [interfaces, count, rndisOn] : {std::tuple{palm, uint8_t(1), false}, std::tuple{rndis, uint8_t(2), true}}) {
            const auto c = config(interfaces, count);
            Bytes in = device;
            in.push_back(uint8_t(c.size())), in.push_back(uint8_t(c.size() >> 8));
            in = Cat(in, c);
            Fdp f;
            f.Bool(rndisOn).Data(Bytes(300, 0x03));
            Seed("usb-bus-host", Cat(in, f.Build()));
        }
    }

    {
        const std::string packets = Gdb("qSupported:multiprocess+") + "+" + Gdb("?") + Gdb("g") + Gdb("p f") + Gdb("pf") +
                                    Gdb("P1=78563412") + Gdb("m100,10") + Gdb("M100,4:01020304") + Gdb("Z0,200,4") +
                                    Gdb("z0,200,4") + Gdb("qXfer:features:read:target.xml:0,100") + Gdb("vCont?") +
                                    Gdb("qRcmd," + Hex("pace-break 1000")) + Gdb("qRcmd," + Hex("pace")) + Gdb("s") + Gdb("c") +
                                    "\x03" + Gdb("D");
        Seed("gdbstub-arm", Cat(Bytes(8, 0), Bytes(packets.begin(), packets.end())));
        Seed("gdbstub-arm", Cat(Bytes{0, 0, 0, 0, 0, 0, 0, 3}, Bytes(packets.begin(), packets.end())));
        const std::string m68k = Gdb("qSupported") + Gdb("?") + Gdb("qOffsets") + Gdb("g") + Gdb("p11") + Gdb("P1=5") +
                                 Gdb("m0,20") + Gdb("m0,4") + Gdb("M10,2:abcd") + Gdb("Z0,1000,2") + Gdb("Z2,2000,4") +
                                 Gdb("z2,2000,4") + Gdb("Hg0") + Gdb("s") + "\x03" + Gdb("c") + "\x03" + Gdb("D");
        Seed("gdbstub-68k", Cat(Bytes(8, 0), Bytes(m68k.begin(), m68k.end())));
    }

    {
        const std::string lines = "1\tsave\t" + Hex("/tmp/a.frond") + "\n0\tpanel-visible\t" + Hex("on") + "\n0\tcancel-files\t\n" +
                                  "2\tsd-create\t" + Hex("64") + "\n3\tquit\t\n";
        for (uint8_t choices : {0, 7}) Seed("desktop-channel", Cat(Bytes{choices}, Bytes(lines.begin(), lines.end())));
    }

    {
        // Two chunks: a version word and values, then a buffer and a string.
        Bytes state;
        Le32(state, 2), Le32(state, 0), Le32(state, 4), Le32(state, 1), Le32(state, 6);
        for (uint32_t v : {1u, 0x12u, 0x3456u, 0x789abcdeu}) Le32(state, v);
        Le32(state, 2);
        for (char c : std::string("fuzz string\0\0\0\0\0\0\0\0\0\0\0", 20)) state.push_back(uint8_t(c));
        Fdp f;
        f.Int(3, 0, 64);
        for (const auto& [chunk, kind, size] : {std::tuple{8u, 2u, 1u}, std::tuple{0u, 10u, 0u}, std::tuple{1u, 5u, 20u}})
            f.Int(chunk, 0, 255).Int(kind, 0, 255).Int(size, 0, 65535);
        f.Bool(false).Data(state);
        Seed("savestate", f.Build());
    }

    StorageSeeds([](const std::string& target, const Bytes& bytes) { Seed(target, bytes); });

    {
        // A Tapwave-style update made here: a PRC whose ROMZ id 2 is gzip of 64 zero bytes, alone and in a zip.
        Bytes rom(64, 0), gz;
        {
            GzipContext gzip(rom.data(), rom.size());
            while (gzip.GetState() == 0 || gzip.GetState() == 1) gzip.Continue();
            gz.assign(gzip.GetGzipData(), gzip.GetGzipData() + gzip.GetGzipSize());
        }
        Bytes prc(0x20, 0);
        std::memcpy(prc.data(), "Zodiac Update", 13);
        Be16(prc, 1), Be16(prc, 1);
        prc.resize(0x3c, 0);
        prc.insert(prc.end(), {'r', 'o', 'm', 'z', 'T', 'p', 'w', 'v'});
        Be32(prc, 0), Be32(prc, 0), Be16(prc, 2);
        prc.insert(prc.end(), {'c', 'o', 'd', 'e'}), Be16(prc, 1), Be32(prc, 0x4e + 20 + 2);
        prc.insert(prc.end(), {'R', 'O', 'M', 'Z'}), Be16(prc, 2), Be32(prc, 0x4e + 20 + 2 + 4);
        prc.insert(prc.end(), {0, 0, 1, 2, 3, 4});
        prc = Cat(prc, gz);
        Seed("zodiac-rom", Cat(Bytes{1}, prc));
        CreateZipContext zip;
        zip.Initialize(6);
        zip.AddEntry("Zodiac_Update.prc");
        zip.WriteData(prc.data(), prc.size());
        const auto* z = static_cast<const uint8_t*>(zip.GetZipData());
        Seed("zodiac-rom", Cat(Bytes{1}, Bytes(z, z + zip.GetZipDataSize())));
        Seed("zodiac-rom", Cat(Bytes{0}, Bytes(z, z + zip.GetZipDataSize())));
    }

    {
        // Standard MIDI files: format 0 with tempo, notes, sysex and end of track; format 1 with two tracks.
        auto track = [](const Bytes& events) {
            Bytes t{'M', 'T', 'r', 'k'};
            Be32(t, uint32_t(events.size()));
            return Cat(t, events);
        };
        auto header = [](uint16_t format, uint16_t tracks, uint16_t division) {
            Bytes h{'M', 'T', 'h', 'd', 0, 0, 0, 6};
            Be16(h, format), Be16(h, tracks), Be16(h, division);
            return h;
        };
        const Bytes events{0, 0xff, 0x51, 3, 0x07, 0xa1, 0x20, 0, 0x90, 60, 100, 0x60, 0x80, 60, 0, 0, 0xf0, 3, 0x43, 0x10, 0xf7,
                           0x81, 0x00, 0x90, 64, 90, 0x40, 64, 0, 0, 0xff, 0x2f, 0};
        Seed("zodiac-smf", Cat(header(0, 1, 96), track(events)));
        Seed("zodiac-smf", Cat(Cat(header(1, 2, 480), track(Bytes{0, 0xff, 0x58, 4, 4, 2, 24, 8, 0, 0xff, 0x2f, 0})), track(events)));
        Seed("zodiac-smf", Cat(header(0, 1, 0xe728), track(events)));
    }

    {
        // Firmware headers: a Palm ROM card header for the 68k check and an rx1950-style NBF for the OS scan.
        Bytes rom(0x20000, 0xff);
        const uint8_t card[] = {0, 0, 0, 0x01, 0x00, 0x00, 0x00, 0x00};
        std::memcpy(rom.data(), card, sizeof card);
        Seed("qt-firmware-input", Cat(Bytes{128}, rom));
        Bytes nbf(0xa5a + 520 * 4, 0xff);
        for (size_t s = 0; s < 4; ++s) nbf[0xa5a + 520 * s + 512] = uint8_t(s), nbf[0xa5a + 520 * s + 513] = 0, nbf[0xa5a + 520 * s + 514] = 0, nbf[0xa5a + 520 * s + 515] = 0;
        Seed("qt-firmware-input", Cat(Bytes{0}, nbf));
        const std::string lines = "0\tevent\tready\t\n0\tevent\tpointer-press\t" + Hex("123") + "\n0\tevent\tsd-progress\t" +
                                  Hex("1 2") + "\n0\tevent\tusb\t" + Hex("on") + "\n1\tok\tdone\t" + Hex("Complete") + "\n" +
                                  "2\terror\tbusy\t\n";
        Seed("qt-desktop-client", Cat(Bytes{0x0b}, Bytes(lines.begin(), lines.end())));
        Seed("qt-desktop-client", Cat(Bytes{0x20}, Bytes(lines.begin(), lines.end())));
    }
    return 0;
}
