// Writes the synthetic seed corpora for the fuzz targets: seeds DIR makes DIR/<target>/<n>.
// Every seed is made here from Frond's own encoders or by hand; none come from firmware,
// sessions or user data.
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
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
#include "wire.h"

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
}  // namespace

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
        while (gzip.GetState() == 0) gzip.Continue();
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
    return 0;
}
