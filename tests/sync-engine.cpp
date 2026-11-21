// The sync manager's pieces without a handheld: SLP's checksums, the serial (SLP/PADP/CMP) and
// NetSync lines against a scripted handheld, DLP arguments, PDB files and the profile store.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "ActiveSync.h"
#include "ActiveSyncFiles.h"
#include "Dlp.h"
#include "HotSync.h"
#include "InstallerFiles.h"
#include "../core/src/common/zip/miniz.h"
#include "Link.h"
#include "PalmDb.h"
#include "Sha256.h"
#include "SyncStore.h"
#include "SyncStream.h"
#include "../core/src/common/sync/vendor/sqlite/sqlite3.h"

using namespace frondSync;

namespace {

int failures = 0;
#define CHECK(x)                                                         \
    do {                                                                 \
        if (!(x)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x), ++failures; \
    } while (0)

// The handheld's end of a pipe: blocking reads of whole SLP packets.
struct Handheld {
    SyncPipe& pipe;
    Bytes buffer;

    void Put(const Bytes& b) {
        size_t at = 0;
        while (at < b.size()) {
            at += pipe.DeviceToHost(b.data() + at, b.size() - at);
            if (at < b.size()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    bool Fill(size_t n) {
        for (int i = 0; i < 5000 && buffer.size() < n; ++i) {
            uint8_t chunk[4096];
            const size_t got = pipe.HostToDevice(chunk, sizeof(chunk));
            buffer.insert(buffer.end(), chunk, chunk + got);
            if (!got) {
                if (pipe.Closed()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return buffer.size() >= n;
    }
    // Next SLP packet: xid and body.
    bool Slp(uint8_t& xid, Bytes& body) {
        if (!Fill(10)) return false;
        if (buffer[0] != 0xbe || SlpHeaderSum(buffer.data()) != buffer[9]) return false;
        const size_t size = size_t(buffer[6] << 8 | buffer[7]);
        if (!Fill(12 + size)) return false;
        if (SlpCrc(buffer.data(), 10 + size) != uint16_t(buffer[10 + size] << 8 | buffer[11 + size])) return false;
        xid = buffer[8];
        body.assign(buffer.begin() + 10, buffer.begin() + long(10 + size));
        buffer.erase(buffer.begin(), buffer.begin() + long(12 + size));
        return true;
    }
    Bytes Bytes6(size_t n) {
        Fill(n);
        Bytes b(buffer.begin(), buffer.begin() + long(std::min(n, buffer.size())));
        buffer.erase(buffer.begin(), buffer.begin() + long(b.size()));
        return b;
    }
};

Bytes Padp(uint8_t type, uint8_t flags, uint16_t size, const Bytes& data = {}) {
    Bytes b{type, flags, uint8_t(size >> 8), uint8_t(size)};
    b.insert(b.end(), data.begin(), data.end());
    return b;
}

void TestChecksums() {
    const uint8_t text[] = "123456789";
    CHECK(SlpCrc(text, 9) == 0x31c3);  // CRC-16/XMODEM check value
    const auto packet = SlpPacket(3, 3, 2, 7, Bytes{1, 2, 3});
    CHECK(packet.size() == 15 && packet[9] == SlpHeaderSum(packet.data()));
}

void TestSerialLine() {
    auto pipe = std::make_shared<SyncPipe>();
    SyncPipe::End end(pipe);
    std::atomic<bool> stop{false};
    std::unique_ptr<HandheldLink> link;
    std::string error;
    std::thread desk([&] { link = AcceptHandheld(end, Transport::serial, stop, error); });
    Handheld h{*pipe};
    // Line noise, a loopback ping, then the CMP wakeup at 57600.
    h.Put({0x00, 0x55});
    h.Put(SlpPacket(3, 3, 3, 1, {}));
    const Bytes wakeup{1, 0, 1, 1, 0, 0, 0, 0, 0xe1, 0};
    h.Put(SlpPacket(3, 3, 2, 0xff, Padp(1, 0xc0, uint16_t(wakeup.size()), wakeup)));
    uint8_t xid;
    Bytes body;
    CHECK(h.Slp(xid, body) && xid == 0xff && body.size() == 4 && body[0] == 2);  // ack
    CHECK(h.Slp(xid, body) && xid == 0xff && body.size() == 14 && body[0] == 1 && body[4] == 2 && body[5] == 0x80 &&
          body[12] == 0xe1);  // INIT, change to 57600
    h.Put(SlpPacket(3, 3, 2, xid, Padp(2, body[1], uint16_t(body[2] << 8 | body[3]))));
    desk.join();
    CHECK(link != nullptr);
    if (!link) return std::printf("FAIL accept: %s\n", error.c_str()), void(++failures);

    // A 2500-byte request goes in three fragments, each waiting for its ack.
    Bytes big(2500);
    for (size_t i = 0; i < big.size(); ++i) big[i] = uint8_t(i * 7);
    bool sent = false;
    std::thread send([&] { sent = link->Send(big); });
    Bytes got;
    uint8_t requestXid = 0;
    for (int fragment = 0; fragment < 3; ++fragment) {
        CHECK(h.Slp(xid, body) && body[0] == 1);
        const uint16_t size = uint16_t(body[2] << 8 | body[3]);
        if (fragment == 0) {
            CHECK(size == 2500 && (body[1] & 0x80));
            requestXid = xid;
        } else {
            CHECK(size == got.size() && xid == requestXid);
        }
        if (fragment == 2) CHECK(body[1] & 0x40);
        got.insert(got.end(), body.begin() + 4, body.end());
        if (fragment == 0) {
            h.Put(SlpPacket(3, 3, 2, xid, Padp(1, 0x10, 0)));  // truncated long header
            h.Put(SlpPacket(3, 3, 2, xid, Padp(2, body[1], size + 1)));
            h.Put(SlpPacket(3, 3, 2, 0x40, Padp(1, 0xc0, 0)));
            uint8_t duplicate;
            Bytes ack;
            CHECK(h.Slp(duplicate, ack) && duplicate == 0x40 && ack == Padp(2, 0xc0, 0));
        }
        h.Put(SlpPacket(3, 3, 2, xid, Padp(2, body[1], size)));
    }
    send.join();
    CHECK(sent && got == big && requestXid != 0 && requestXid != 0xff);

    // The answer in two fragments, a tickle between them; each fragment is acked.
    Bytes answer(1500, 0x5a);
    Bytes received;
    bool ok = false;
    std::thread receive([&] { ok = link->Receive(received, 5000); });
    h.Put(SlpPacket(3, 3, 2, requestXid, Padp(1, 0x80, 1500, Bytes(answer.begin(), answer.begin() + 1024))));
    CHECK(h.Slp(xid, body) && body[0] == 2 && xid == requestXid);
    h.Put(SlpPacket(3, 3, 2, requestXid, Padp(4, 0, 0)));
    h.Put(SlpPacket(3, 3, 2, requestXid, Padp(1, 0x40, 1024, Bytes(answer.begin() + 1024, answer.end()))));
    CHECK(h.Slp(xid, body) && body[0] == 2);
    receive.join();
    CHECK(ok && received == answer);

    // The handheld gives up.
    std::thread abort([&] { ok = link->Receive(received, 5000); });
    h.Put(SlpPacket(3, 3, 2, 9, Padp(8, 0, 0)));
    abort.join();
    CHECK(!ok && !link->error.empty());
}

void TestNetSyncLine() {
    auto pipe = std::make_shared<SyncPipe>();
    SyncPipe::End end(pipe);
    std::atomic<bool> stop{false};
    std::unique_ptr<HandheldLink> link;
    std::string error;
    std::thread desk([&] { link = AcceptHandheld(end, Transport::usb, stop, error); });
    Handheld h{*pipe};
    const auto frame = [](uint8_t xid, const Bytes& m) {
        Bytes f{1, xid, 0, 0, uint8_t(m.size() >> 8), uint8_t(m.size())};
        f.insert(f.end(), m.begin(), m.end());
        return f;
    };
    Bytes hello(22, 0);
    hello[0] = 0x90, hello[1] = 1;
    h.Put(frame(0xff, hello));
    auto r = h.Bytes6(6 + 50);
    CHECK(r.size() == 56 && r[0] == 1 && r[1] == 0xff && r[5] == 50 && r[6] == 0x12);
    Bytes m(50, 0);
    m[0] = 0x92;
    h.Put(frame(0xff, m));
    r = h.Bytes6(6 + 46);
    CHECK(r.size() == 52 && r[5] == 46 && r[6] == 0x13);
    h.Put(frame(0xff, Bytes{0x93, 0, 0, 0, 0, 0, 0, 0}));
    desk.join();
    CHECK(link != nullptr);
    if (!link) return std::printf("FAIL netsync accept: %s\n", error.c_str()), void(++failures);
    // Requests count up from 0; answers carry the request's ID.
    CHECK(link->Send(Bytes{0x12, 1, 0x20, 4, 0, 1, 0, 4}));
    r = h.Bytes6(6 + 8);
    CHECK(r.size() == 14 && r[1] == 0 && r[6] == 0x12);
    h.Put(frame(0, Bytes{0x92, 0, 0, 0}));
    Bytes answer;
    CHECK(link->Receive(answer, 2000) && answer == (Bytes{0x92, 0, 0, 0}));
    CHECK(link->Send(Bytes{0x2f, 1}));
    r = h.Bytes6(6 + 2);
    CHECK(r.size() == 8 && r[1] == 1);
}

// A resource close to the legacy packet limit must be assembled from two DLP reads.
void TestLegacyResourceRead() {
    class Device : public PacketLink {
       public:
        bool Send(const Bytes& message) override {
            uint8_t function{};
            std::vector<DlpArg> args;
            if (!DlpParseRequest(message, function, args) || function != dlpReadResource ||
                args.size() != 1 || args.front().data.size() != 8)
                return false;
            const auto& request = args.front().data;
            const size_t offset = (size_t(request[4]) << 8) | request[5];
            const size_t limit = (size_t(request[6]) << 8) | request[7];
            if (offset != read || limit > 0xf000 || read >= size) return false;
            Bytes data{'t', 'e', 's', 't', 0, 9, 0, 0, uint8_t(size >> 8), uint8_t(size)};
            const auto count = std::min(size - offset, limit);
            data.insert(data.end(), count, offset ? 0xbe : 0xef);
            reply = DlpResponse(function, 0, {{0x20, std::move(data)}});
            read += count;
            return true;
        }

        bool Receive(Bytes& message, int) override {
            message = std::move(reply);
            return true;
        }
        std::string Name() const override { return "test"; }

        const size_t size = 0xfff0;
        size_t read = 0;
        Bytes reply;
    } device;
    DlpClient dlp(device);
    PalmRecord resource;
    CHECK(dlp.ReadResourceByIndex(1, 0, resource) == 0);
    CHECK(resource.type == 0x74657374 && resource.id == 9 && resource.data.size() == device.size);
    CHECK(!resource.data.empty() && resource.data.front() == 0xef && resource.data.back() == 0xbe);
}

void TestDlp() {
    // Tiny, small and long arguments, no padding between them.
    const Bytes small(300, 1), large(70000, 2);
    const auto request = DlpRequest(0x16, {{0x20, Bytes{0x80, 0, 0, 0}}, {0x21, small}, {0x22, large}});
    uint8_t function = 0;
    std::vector<DlpArg> args;
    CHECK(DlpParseRequest(request, function, args) && function == 0x16 && args.size() == 3);
    CHECK(args.size() == 3 && args[0].data.size() == 4 && args[1].data == small && args[2].data == large);
    CHECK(request.size() == 2 + 2 + 4 + 4 + 300 + 6 + 70000);
    const auto response = DlpResponse(0x16, 0, {{0x20, Bytes{1, 2}}});
    uint16_t error = 1;
    CHECK(DlpParseResponse(response, 0x16, error, args) && error == 0 && args.size() == 1 && args[0].data == (Bytes{1, 2}));
    CHECK(!DlpParseResponse(response, 0x17, error, args));
}

void TestInstallerFiles() {
    PalmDb db;
    db.name = "Installer test";
    db.type = FourCc("DATA");
    db.creator = FourCc("Frnd");
    Bytes palm;
    CHECK(db.Write(palm));

    Bytes cabinet(48);
    std::copy_n("MSCF", 4, cabinet.data());
    cabinet[8] = 48;
    cabinet[16] = 36;
    cabinet[24] = 3;
    cabinet[25] = 1;

    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) { CHECK(false); return; }
    CHECK(mz_zip_writer_add_mem(&zip, "folder/App.PDB", palm.data(), palm.size(), 0));
    CHECK(mz_zip_writer_add_mem(&zip, "folder/App.cab", cabinet.data(), cabinet.size(), 0));
    CHECK(mz_zip_writer_add_mem(&zip, "folder/bad.prc", cabinet.data(), cabinet.size(), 0));
    CHECK(mz_zip_writer_add_mem(&zip, "folder/README.txt", palm.data(), palm.size(), 0));
    void* buffer = nullptr;
    size_t size = 0;
    CHECK(mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size));
    CHECK(mz_zip_writer_end(&zip));
    if (!buffer) return;
    const auto* data = static_cast<const uint8_t*>(buffer);
    const Bytes archive(data, data + size);
    mz_free(buffer);

    std::vector<InstallerFile> files;
    std::string error;
    CHECK(UnpackInstaller("suite.ZIP", archive, files, error));
    CHECK(files.size() == 2);
    if (files.size() == 2) {
        CHECK(files[0].name == "App.PDB" && files[0].bytes == palm);
        CHECK(files[1].name == "App.cab" && files[1].bytes == cabinet);
    }

    Bytes setup{'M', 'Z', 0, 0};
    setup.insert(setup.end(), cabinet.begin(), cabinet.end());
    CHECK(UnpackInstaller("Setup.exe", setup, files, error));
    CHECK(files.size() == 1);
    if (files.size() == 1) CHECK(files[0].name == "Setup-1.cab" && files[0].bytes == cabinet);
    setup.pop_back();
    CHECK(!UnpackInstaller("Setup.exe", setup, files, error) && files.empty());
}

void TestPalmDb() {
    PalmDb db;
    db.name = "Test DB";
    db.type = FourCc("DATA"), db.creator = FourCc("test");
    db.appInfo = Bytes{1, 2, 3};
    db.records.push_back({0x123456, 0x42, 0, 0, Bytes{9, 8, 7}});
    db.records.push_back({0x123457, 0x01, 0, 0, Bytes{}});
    Bytes file;
    CHECK(db.Write(file));
    PalmDb back;
    std::string error;
    CHECK(PalmDb::Read(file, back, error) && back.name == db.name && back.type == db.type && back.appInfo == db.appInfo);
    CHECK(back.records.size() == 2 && back.records[0].uid == 0x123456 && back.records[0].attributes == 0x42 &&
          back.records[0].data == db.records[0].data && back.records[1].data.empty());
    PalmDb prc;
    prc.name = "Res";
    prc.attributes = dbResource;
    prc.records.push_back({0, 0, FourCc("code"), 1, Bytes(100, 0x4e)});
    CHECK(prc.Write(file) && PalmDb::Read(file, back, error) && back.Resource() && back.records.size() == 1 &&
          back.records[0].type == FourCc("code") && back.records[0].id == 1);
    CHECK(!PalmDb::Read(Bytes(40, 0), back, error));
}

void PutWords(Bytes& bytes, std::initializer_list<uint32_t> values) {
    for (const auto n : values)
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(uint8_t(n >> shift));
}

Bytes Words(std::initializer_list<uint32_t> values) {
    Bytes bytes;
    PutWords(bytes, values);
    return bytes;
}

bool RapiReply(Handheld& guest, uint8_t op, const Bytes& reply) {
    const auto length = guest.Bytes6(4);
    if (length.size() != 4 || length[2] || length[3]) return false;
    const auto command = guest.Bytes6(length[0] | (length[1] << 8));
    if (command.size() < 4 || command[0] != op || command[1] || command[2] || command[3]) return false;
    guest.Put(Words({uint32_t(reply.size())}));
    guest.Put(reply);
    return true;
}

Bytes RapiConfigReply(const std::string& body) {
    const auto xml = "<wap-provisioningdoc>" + body + "</wap-provisioningdoc>";
    auto reply = Words({0, 0, uint32_t((xml.size() + 1) * 2)});
    for (const auto character : xml) {
        reply.push_back(uint8_t(character));
        reply.push_back(0);
    }
    reply.insert(reply.end(), {0, 0});
    return reply;
}

void TestActiveSyncProfileFiles(const std::string& root) {
    namespace fs = std::filesystem;
    std::string error;
    SyncLibrary library;
    ProfileInfo profile;
    CHECK(library.Open(root, error));
    CHECK(library.CreateProfile("File exchange", profile, error));
    const auto profilePath = library.ProfilePath(profile);
    CHECK(ActiveSyncFilesFolder(profilePath).empty());
    CHECK(ListActiveSyncFiles(profilePath).empty());
    CHECK(!ActiveSyncFilesFolder(profilePath, true).empty());

    const auto source = (fs::u8path(root) / "file-source").u8string();
    std::ofstream(source, std::ios::binary) << "Original";
    AddActiveSyncFile(profilePath, "Example.txt", source);
    CHECK(ListActiveSyncFiles(profilePath) == std::vector<std::string>{"Example.txt"});
    for (const auto name : {"example.TXT", "../other.txt", "LPT1.txt"}) {
        bool rejected = false;
        try {
            AddActiveSyncFile(profilePath, name, source);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        CHECK(rejected);
    }
    const auto exported = (fs::u8path(root) / "file-export").u8string();
    ExportActiveSyncFile(profilePath, "Example.txt", exported);
    std::ifstream file(exported, std::ios::binary);
    std::string contents;
    std::getline(file, contents);
    CHECK(contents == "Original");
}

void TestActiveSync(const std::string& root) {
    std::string error;
    SyncLibrary library;
    CHECK(library.Open(root, error));
    ProfileInfo info;
    CHECK(library.CreateProfile("Windows Mobile", info, error));
    const auto key = MachineDeviceKey("wm-test");
    CHECK(library.Assign(key, info.id, error, "winmo"));
    SyncProfile profile;
    CHECK(profile.Open(library.ProfilePath(info), error));
    PalmDb palm;
    palm.name = "Test";
    Bytes pdb;
    CHECK(palm.Write(pdb));
    InstallItem cabItem, palmItem;
    CHECK(profile.QueueInstall("", "Test.pdb", pdb, palmItem, error));
    Bytes cab(36);
    std::copy_n("MSCF", 4, cab.begin());
    CHECK(!profile.QueueInstall("", "Test.CAB", pdb, cabItem, error, "cab"));
    CHECK(profile.QueueInstall("", "Test.CAB", cab, cabItem, error, "cab"));
    profile.Close();

    SyncOptions options;
    options.root = root;
    options.machineKey = "wm-test";
    auto& manager = SyncManager::Get();
    manager.Configure(options);
    ActiveSyncSession session;
    session.Start();
    Handheld control{session.Pipe(0)}, rapi{session.Pipe(1)};
    const auto connectRapi = [&](Handheld& control, Handheld& rapi) {
        control.Put(Words({0}));
        CHECK(control.Bytes6(4) == Words({3}));
        Bytes deviceInfo(16);
        deviceInfo.front() = 1;
        PutWords(deviceInfo, {5, 1, 1});
        deviceInfo.insert(deviceInfo.end(), {'P', 0, 0, 0});
        PutWords(deviceInfo, {0, 0, 0, 42, 0, 2});
        deviceInfo.insert(deviceInfo.end(), {'P', 0});
        PutWords(deviceInfo, {2});
        deviceInfo.insert(deviceInfo.end(), {'C', 0, 0});
        PutWords(deviceInfo, {0, 0});
        control.Put(Words({4, uint32_t(deviceInfo.size())}));
        control.Put(deviceInfo);
        CHECK(control.Bytes6(12) == Words({5, 4, 1}));
        rapi.Put(Words({1}));
        CHECK(RapiReply(rapi, 0x43, Words({0, 1, 16, 16, 5, 1, 0})));
    };
    const auto acceptCab = [&](Handheld& rapi) {
        CHECK(RapiReply(rapi, 0x16, Words({0, 1})));
        CHECK(RapiReply(rapi, 0x18, Words({0, 1, uint32_t(cab.size())})));
        CHECK(RapiReply(rapi, 0x19, Words({0, 1})));
        CHECK(RapiReply(rapi, 0x2a, Words({0, 1, 2, 3, 4, 5})));
        CHECK(RapiReply(rapi, 0x19, Words({0, 1})));
        CHECK(RapiReply(rapi, 0x19, Words({0, 1})));
    };
    const auto finish = [](ActiveSyncSession& session) {
        for (unsigned i = 0; i < 1000 && session.Running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(!session.Running());
    };
    connectRapi(control, rapi);
    acceptCab(rapi);
    finish(session);
    CHECK(manager.Status().state == SyncStatus::State::done);
    const auto connected = library.FindAssignment(key);
    CHECK(connected && connected->kind == "winmo" && connected->userId == 42);
    CHECK(profile.Open(library.ProfilePath(info), error));
    const auto left = profile.PendingInstalls(key);
    CHECK(left.size() == 1 && left.front().id == palmItem.id);

    CHECK(profile.RequestActiveSyncPairing(key, true, error));
    ActiveSyncSession occupiedSession;
    occupiedSession.Start();
    Handheld occupiedControl{occupiedSession.Pipe(0)}, occupiedRapi{occupiedSession.Pipe(1)};
    connectRapi(occupiedControl, occupiedRapi);
    for (const char* slot : {"P1", "P2"}) {
        const std::string path = R"(HKLM\Software\Microsoft\Windows CE Services\Partners\)" + std::string(slot);
        CHECK(RapiReply(occupiedRapi, 0x0e, RapiConfigReply(
            "<characteristic type=\"Registry\"><characteristic type=\"" + path +
            "\"><parm name=\"PId\" value=\"42\"/></characteristic></characteristic>")));
    }
    for (unsigned i = 0; i < 1000 && occupiedSession.Running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(manager.Status().state == SyncStatus::State::failed);
    CHECK(manager.Status().message == "Windows Mobile has no free desktop partnership slot.");
    CHECK(profile.ActiveSyncPairing(key) == "requested");
    CHECK(profile.RequestActiveSyncPairing(key, false, error));
    CHECK(profile.ActiveSyncPairing(key).empty());
    CHECK(profile.ConfirmActiveSyncPairing(key, error));
    CHECK(profile.RequestActiveSyncPairing(key, false, error));
    CHECK(profile.ActiveSyncPairing(key) == "confirmed");

    {
        // The script connects with a GUID whose first byte is 1 and the rest are zero.
        const auto name = "Frond ActiveSync:" + std::to_string(profile.PcId()) + ":01" + std::string(30, '0');
        auto hash = Sha256Hex(reinterpret_cast<const uint8_t*>(name.data()), name.size());
        for (char& digit : hash)
            if (digit >= 'a' && digit <= 'f') digit -= 'a' - 'A';
        const auto id = std::to_string((std::stoul(hash.substr(32, 8), nullptr, 16) & 0x3fffffff) + 1);
        const auto host = "FROND-" + hash.substr(40, 8);
        const auto source = "{" + hash.substr(0, 8) + "-" + hash.substr(8, 4) + "-8" + hash.substr(13, 3) +
                            "-8" + hash.substr(17, 3) + "-" + hash.substr(20, 12) + "}";
        ActiveSyncSession paired;
        paired.Start();
        Handheld control{paired.Pipe(0)}, rapi{paired.Pipe(1)};
        connectRapi(control, rapi);
        CHECK(RapiReply(rapi, 0x0e, RapiConfigReply(
            R"(<characteristic type="Registry"><characteristic type="HKLM\Software\Microsoft\Windows CE Services\Partners\P1">)" +
            std::string("<parm name=\"DataSourceID\" value=\"") + source + "\"/><parm name=\"PName\" value=\"" + host +
            "\"/><parm name=\"PId\" value=\"" + id + "\"/></characteristic></characteristic>")));
        CHECK(RapiReply(rapi, 0x0e, RapiConfigReply(
            R"(<characteristic type="Registry"><nocharacteristic type="HKLM\Software\Microsoft\Windows CE Services\Partners\P2"/></characteristic>)")));
        CHECK(RapiReply(rapi, 0x0e, RapiConfigReply(
            "<characteristic recursive=\"false\" type=\"Sources\"><characteristic type=\"" + source +
            "\"/></characteristic>")));
        CHECK(RapiReply(rapi, 0x0e, RapiConfigReply(
            "<characteristic type=\"" + source + "\"><parm name=\"Server\" value=\"" + host +
            "\"/><parm name=\"StoreType\" value=\"2\"/></characteristic>")));
        finish(paired);
        CHECK(manager.Status().state == SyncStatus::State::done);
        CHECK(manager.Status().message.find("Desktop partnership verified.") != std::string::npos);
        CHECK(profile.ActiveSyncPairing(key) == "confirmed");
    }

    {
        ActiveSyncSession failedCheck;
        failedCheck.Start();
        Handheld control{failedCheck.Pipe(0)}, rapi{failedCheck.Pipe(1)};
        connectRapi(control, rapi);
        CHECK(RapiReply(rapi, 0x0e, Words({5, 0x80004005})));
        finish(failedCheck);
        CHECK(manager.Status().state == SyncStatus::State::failed);
        CHECK(profile.ActiveSyncPairing(key) == "confirmed");
    }
    {
        ActiveSyncSession afterReset;
        afterReset.Start();
        Handheld control{afterReset.Pipe(0)}, rapi{afterReset.Pipe(1)};
        connectRapi(control, rapi);
        for (const char* slot : {"P1", "P2"}) {
            const auto path = R"(HKLM\Software\Microsoft\Windows CE Services\Partners\)" + std::string(slot);
            CHECK(RapiReply(rapi, 0x0e, RapiConfigReply(
                "<characteristic type=\"Registry\"><nocharacteristic type=\"" + path +
                "\"/></characteristic>")));
        }
        finish(afterReset);
        CHECK(manager.Status().state == SyncStatus::State::done);
        CHECK(manager.Status().message.find("Desktop partnership missing.") != std::string::npos);
        CHECK(profile.ActiveSyncPairing(key).empty());
        CHECK(profile.RequestActiveSyncPairing(key, true, error));
        CHECK(profile.ActiveSyncPairing(key) == "requested");
        CHECK(profile.RequestActiveSyncPairing(key, false, error));
    }

    // Both directories contain 80 MiB. Comparing the pairs reads only the guest's 80 MiB.
    const auto folder = std::filesystem::u8path(ActiveSyncFilesFolder(library.ProfilePath(info), true));
    constexpr uint32_t fileSize = 16 * 1024 * 1024, chunkSize = 8192;
    auto listing = Words({0, 1, 5});
    for (unsigned i = 0; i < 5; ++i) {
        const auto name = "file" + std::to_string(i);
        std::ofstream sparse(folder / name, std::ios::binary);
        sparse.seekp(fileSize - 1);
        CHECK(bool(sparse.put(0)));
        PutWords(listing, {uint32_t(2 * (name.size() + 1)), 0, 0, fileSize});
        for (char c : name) listing.insert(listing.end(), {uint8_t(c), 0});
        listing.insert(listing.end(), {0, 0});
    }
    CHECK(profile.QueueInstall("", "AfterFiles.cab", cab, cabItem, error, "cab"));
    {
        ActiveSyncSession withFiles;
        withFiles.Start();
        Handheld control{withFiles.Pipe(0)}, rapi{withFiles.Pipe(1)};
        connectRapi(control, rapi);
        CHECK(RapiReply(rapi, 0x28, Words({0, 1})));
        CHECK(RapiReply(rapi, 0x1a, listing));
        auto data = Words({0, 1, chunkSize});
        data.resize(data.size() + chunkSize);
        for (unsigned i = 0; i < 5; ++i) {
            const bool opened = RapiReply(rapi, 0x16, Words({0, 1}));
            CHECK(opened);
            if (!opened) break;
            for (unsigned offset = 0; offset < fileSize; offset += chunkSize) {
                const bool read = RapiReply(rapi, 0x17, data);
                CHECK(read);
                if (!read) break;
            }
            CHECK(RapiReply(rapi, 0x17, Words({0, 1, 0})));
            CHECK(RapiReply(rapi, 0x19, Words({0, 1})));
        }
        acceptCab(rapi);
        finish(withFiles);
        CHECK(manager.Status().state == SyncStatus::State::done);
        CHECK(manager.Status().message.find("Files: 0 sent, 0 received, 0 differed") != std::string::npos);
        CHECK(profile.PendingInstalls(key).size() == 1);
    }

    CHECK(profile.QueueInstall("", "AfterFileError.cab", cab, cabItem, error, "cab"));
    {
        ActiveSyncSession withFileError;
        withFileError.Start();
        Handheld control{withFileError.Pipe(0)}, rapi{withFileError.Pipe(1)};
        connectRapi(control, rapi);
        CHECK(RapiReply(rapi, 0x28, Words({5, 0})));
        acceptCab(rapi);
        finish(withFileError);
        CHECK(manager.Status().state == SyncStatus::State::done);
        CHECK(manager.Status().message.find("File exchange failed:") != std::string::npos);
        CHECK(profile.PendingInstalls(key).size() == 1);
    }

    ActiveSyncSession malformedSession;
    malformedSession.Start();
    Handheld malformed{malformedSession.Pipe(0)};
    Bytes shortInfo(70);
    shortInfo[24] = 31;
    malformed.Put(Words({4, uint32_t(shortInfo.size())}));
    malformed.Put(shortInfo);
    for (unsigned i = 0; i < 1000 && malformedSession.Running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(manager.Status().state == SyncStatus::State::failed);
    CHECK(manager.Status().message == "Invalid ActiveSync device information.");
}

class InstallHandheld : public PacketLink {
   public:
    bool Send(const Bytes& message) override {
        uint8_t command = 0;
        std::vector<DlpArg> args;
        if (!DlpParseRequest(message, command, args)) return false;
        uint16_t status = 0;
        Bytes data;
        switch (command) {
            case dlpReadSysInfo: data = Bytes(10); break;
            case dlpReadUserInfo:
                data = Bytes(30);
                data[3] = 1;
                data[28] = 2;
                data.insert(data.end(), {'A', 0});
                break;
            case dlpReadDBList:
                if (args.front().data[3]) {
                    status = dlpErrNotFound;
                } else {
                    data = Bytes(48);
                    data[3] = 1;
                    data[4] = 49;
                    data[7] = dbCopyPrevention;
                    data.insert(data.end(), {'T', 'e', 's', 't', 0});
                }
                break;
            case dlpOpenDB:
                if (refuseRead) status = dlpErrOpen;
                else data = {1};
                break;
            case dlpReadOpenDBInfo: data = {0, uint8_t(record != 0)}; break;
            case dlpReadAppBlock:
            case dlpReadSortBlock: status = dlpErrNotFound; break;
            case dlpReadRecord:
                data = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, record};
                break;
            case dlpDeleteDB:
                ++deletes;
                record = 0;
                break;
            case dlpCreateDB: data = {1}; break;
            case dlpWriteRecord:
                if (args.front().data.back() == 2) status = dlpErrSpace;
                else record = args.front().data.back();
                break;
            default: break;
        }
        response = DlpResponse(command, status, data.empty() ? std::vector<DlpArg>{} : std::vector<DlpArg>{{0x20, data}});
        return true;
    }

    bool Receive(Bytes& message, int) override {
        message = std::move(response);
        return true;
    }
    std::string Name() const override { return "install test"; }

    bool refuseRead{true};
    uint8_t record{1};
    unsigned deletes{0};

   private:
    Bytes response;
};

void TestInstallRecovery(const std::string& root) {
    std::string error;
    SyncLibrary library;
    CHECK(library.Open(root, error));
    ProfileInfo info;
    CHECK(library.CreateProfile("Install recovery", info, error));
    SyncOptions options;
    options.machineKey = "recovery-test";
    options.profile = info.name;
    InstallHandheld handheld;
    SyncProfile profile;
    CHECK(profile.Open(library.ProfilePath(info), error));
    PalmDb replacement;
    replacement.name = "Test";
    replacement.records.push_back({1, 0, 0, 0, Bytes{2}});
    Bytes file;
    CHECK(replacement.Write(file));
    InstallItem item;
    for (bool refuse : {true, false}) {
        CHECK(profile.QueueInstall("", "test.pdb", file, item, error));
        handheld.refuseRead = refuse;
        const auto result = RunHotSync(handheld, options, root, Transport::serial, SyncManager::Get());
        CHECK(result.installFailed == 1 && result.installed == 0);
        CHECK(handheld.deletes == (refuse ? 0 : 2));
        CHECK(handheld.record == 1);
    }
    const auto sources = profile.BackupDevices();
    const auto recovery = std::find_if(sources.begin(), sources.end(), [](const auto& source) {
        return source.find(" / recovery ") != std::string::npos;
    });
    CHECK(recovery != sources.end());
    if (recovery != sources.end()) {
        Bytes backup;
        PalmDb original;
        CHECK(profile.LoadBackup(*recovery, "Test", backup) && PalmDb::Read(backup, original, error));
        CHECK(original.records.size() == 1 && original.records.front().data == Bytes{1});
        CHECK(profile.QueueRestore(*recovery, "machine:other", error) == 0);
        CHECK(profile.QueueRestore(*recovery, MachineDeviceKey(options.machineKey), error) == 1);
    }
}

void TestStore(const std::string& root) {
    std::string error;
    SyncLibrary library;
    CHECK(library.Open(root, error));
    ProfileInfo home, work;
    CHECK(library.CreateProfile("Home", home, error) && library.CreateProfile("Work", work, error));
    CHECK(!library.CreateProfile("home", home, error));  // names are unique, ignoring case
    CHECK(library.Assign("machine:a", home.id, error));
    auto a = library.FindAssignment("machine:a");
    CHECK(a && !a->slowNext);
    const auto originalAssignment = *a;
    CHECK(library.Assign("machine:a", work.id, error));
    a = library.FindAssignment("machine:a");
    CHECK(a && a->profileId == work.id && a->slowNext);  // a move makes the next sync slow
    CHECK(library.Assign("machine:a", home.id, error));
    const auto returned = library.FindAssignment("machine:a");
    CHECK(returned && returned->generation != originalAssignment.generation && returned->slowNext);
    CHECK(library.FinishAssignment(originalAssignment, error));
    CHECK(library.FindAssignment("machine:a")->slowNext);

    SyncProfile profile;
    CHECK(profile.Open(library.ProfilePath(home), error) && profile.PcId());
    CHECK(!profile.ActiveSyncUsesSerial("machine:a"));
    CHECK(profile.SetActiveSyncSerial("machine:a", true, error));
    CHECK(profile.ActiveSyncUsesSerial("machine:a"));
    CHECK(!profile.ActiveSyncUsesSerial("machine:b"));
    SyncOptions options;
    options.root = root;
    options.machineKey = "a";
    SyncManager::Get().Configure(options);
    CHECK(ConfiguredActiveSyncUsesSerial());
    options.profile = work.name;
    SyncManager::Get().Configure(options);
    CHECK(!ConfiguredActiveSyncUsesSerial());

    PalmDb db;
    db.name = "MemoDB";
    db.records.push_back({1, 0, 0, 0, Bytes{1}});
    Bytes file;
    db.Write(file);
    CHECK(profile.StoreBackup("machine:a", db, file, error));
    Bytes back;
    CHECK(profile.LoadBackup("machine:a", "MemoDB", back) && back == file);
    CHECK(profile.Backups("machine:a").size() == 1);

    InstallItem item;
    CHECK(profile.QueueInstall("", "memo.pdb", file, item, error) && item.dbName == "MemoDB");
    CHECK(!profile.QueueInstall("", "bad.prc", Bytes(10, 0), item, error));
    CHECK(!profile.QueueInstall("", "memo.pdb", file, item, error, "card:/PALM/Launcher"));
    const auto oldInstall = item.id;
    CHECK(profile.QueueInstall("", "memo2.pdb", file, item, error));  // replaces the queued copy
    CHECK(item.id != oldInstall && !profile.FinishInstall(oldInstall, true, "Stale"));
    CHECK(profile.PendingInstalls("machine:b").size() == 1);
    CHECK(profile.LoadInstall(item.id, back) && back == file);
    CHECK(profile.FinishInstall(item.id, true, "Installed") && profile.PendingInstalls("machine:b").empty());
    CHECK(profile.QueueRestore("machine:a", "machine:b", error) == 1);
    CHECK(profile.PendingInstalls("machine:b").size() == 1);
    CHECK(profile.QueueInstall("machine:b", "Kept.pdb", file, item, error));
    auto corrupt = db;
    corrupt.name = "Z-Corrupt";
    CHECK(profile.StoreBackup("machine:a", corrupt, Bytes(10, 0), error));
    CHECK(profile.QueueRestore("machine:a", "machine:b", error) == -1);
    const auto afterRestore = profile.PendingInstalls("machine:b");
    CHECK(afterRestore.size() == 1 && afterRestore[0].fileName == "Kept.pdb");
    profile.Log("machine:a", "hello");
    CHECK(!profile.LogEntries("machine:a").empty());
    profile.Close();
    {
        ProfileSyncLock lock;
        CHECK(lock.Acquire(root, home.id, error));
        CHECK(!library.DeleteProfile(home.id, error) && library.FindProfile("Home"));
    }
    sqlite3* corrupted = nullptr;
    CHECK(sqlite3_open((library.ProfilePath(home) + "/profile.db").c_str(), &corrupted) == SQLITE_OK);
    CHECK(sqlite3_exec(corrupted, "UPDATE meta SET value='4294967296' WHERE key='pc_id'", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(corrupted);
    CHECK(!profile.Open(library.ProfilePath(home), error) && error == "The profile has an invalid HotSync PC ID.");
    CHECK(library.DeleteProfile(home.id, error) && !library.FindProfile("Home"));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return std::fprintf(stderr, "usage: sync-engine SCRATCH_FOLDER\n"), 2;
    std::filesystem::remove_all(argv[1]);
    TestChecksums();
    TestSerialLine();
    TestNetSyncLine();
    TestDlp();
    TestLegacyResourceRead();
    TestPalmDb();
    TestInstallerFiles();
    TestStore(argv[1]);
    TestInstallRecovery(argv[1]);
    TestActiveSync(argv[1]);
    TestActiveSyncProfileFiles(argv[1]);
    std::filesystem::remove_all(argv[1]);
    if (failures) return std::printf("sync-engine: %d failed\n", failures), 1;
    std::printf("sync-engine: ok\n");
    return 0;
}
