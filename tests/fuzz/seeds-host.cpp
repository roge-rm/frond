// Seeds for the desktop store, link hub, Bluetooth, conduit, NetLib and JNI targets, all by hand.
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "SeedFdp.h"

namespace {
using Bytes = std::vector<uint8_t>;
using SeedFn = std::function<void(const std::string&, const Bytes&)>;

Bytes Text(const std::string& s) { return Bytes(s.begin(), s.end()); }
Bytes Cat(Bytes a, const Bytes& b) { return a.insert(a.end(), b.begin(), b.end()), a; }
void Le32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
void Be(Bytes& b, uint32_t v, unsigned size) { for (unsigned i = size; i--;) b.push_back(uint8_t(v >> (8 * i))); }

// A conduit request: operation byte then 32-bit little-endian words and blobs, framed.
struct Request {
    Bytes body;
    explicit Request(uint8_t op) : body{op} {}
    Request& Put(uint32_t v) { return Le32(body, v), *this; }
    Request& Text(const std::string& s) { Le32(body, uint32_t(s.size())), body.insert(body.end(), s.begin(), s.end()); return *this; }
    Bytes Frame() const {
        Bytes f;
        Le32(f, uint32_t(body.size()));
        return Cat(f, body);
    }
};

// 32 stack bytes for a NetLib trap: refnum at 0, then words placed by offset.
Bytes Stack(std::initializer_list<std::tuple<unsigned, uint32_t, unsigned>> fields) {
    Bytes s(32, 0);
    s[0] = 0x7f, s[1] = 0xfe;
    for (const auto& [at, value, size] : fields)
        for (unsigned i = 0; i < size; ++i) s[at + i] = uint8_t(value >> (8 * (size - 1 - i)));
    return s;
}

// The jni-sync split: the first byte puts the request's end where it is, padding it with spaces.
// The file part gets at least one byte, as the split can't give the request everything.
Bytes SyncInput(std::string request, Bytes file) {
    if (file.empty()) file.push_back('\n');
    for (;;) {
        const size_t size = 1 + request.size() + file.size();
        for (unsigned b = 0; b < 256; ++b)
            if (b * (size - 1) / 256 == request.size()) return Cat(Cat(Bytes{uint8_t(b)}, Text(request)), file);
        if (request.back() == ' ') file.push_back('\n');
        else request += ' ';
    }
}
}  // namespace

void HostSeeds(const SeedFn& seed) {
    // Machine and card stores.
    seed("qt-session-store", Text(R"({"version":1,"scale":5,"smooth":true,"audio":true,"fastAudio":false,"engine":"fast",)"
                                  R"("speedMultiplier":"2","batteryMode":"level","batteryLevel":40,"contrast":60,"infrared":"link",)"
                                  R"("danaView":{"keyboard":"auto","zoom":"fit","pointer":"touch","panX":0.5,"panY":0.5}})"));
    seed("qt-session-store", Text(R"({"version":1,"id":"0b6f2c1e-4a5d-4f3e-9c7b-1a2b3c4d5e6f","kind":"sd","type":"mmc","name":"Card",)"
                                  R"("bytes":67108864,"created":"2026-01-01T00:00:00Z","identity":"00112233445566778899aabbccddeeff",)"
                                  R"("writeProtected":false,"key":"0123456789abcdef0123456789abcdef"})"));
    seed("qt-session-store", Text(R"({"version":1,"card":"0b6f2c1e-4a5d-4f3e-9c7b-1a2b3c4d5e6f","link":null,"name":"Card","swap":true})"));
    {
        Bytes frond;
        Le32(frond, 0x19800819), Le32(frond, 9);
        Bytes name(32, 0);
        std::memcpy(name.data(), "palm-tx", 7);
        frond = Cat(frond, name);
        frond.resize(88, 0);
        Le32(frond, 2);
        for (uint32_t kind : {1u, 2u}) {
            Bytes d(100, 0);
            d[0] = uint8_t(kind), d[36] = 1;
            d[76] = 0, d[80] = 0x10, d[84] = 0, d[88] = 0x10, d[92] = 7;
            frond = Cat(frond, d);
        }
        seed("qt-session-store", frond);
        Bytes sd(64, 0);
        std::memcpy(sd.data(), "FRONDSD\0", 8);
        sd[8] = 2, sd[20] = 1, sd[28] = 7;
        seed("qt-session-store", sd);
        Bytes ms(64, 0);
        std::memcpy(ms.data(), "FRONDMS\0", 8);
        ms[12] = 0xff, ms[13] = 0xff, ms[14] = 0xff, ms[15] = 0xff, ms[16] = 0xff, ms[17] = 0xff, ms[18] = 0xff, ms[19] = 0xff;
        seed("qt-session-store", ms);
        const std::string manifest = "frond-linked-sd 3\n0 sd \"/cards/sd.frondsd\" 0123456789abcdef0123456789abcdef 01234567 "
                                     "67108864 0 1 1 2 3 sd 0\n";
        Bytes linked = Text("FRONDLINKSD1\n");
        Le32(linked, uint32_t(manifest.size())), Le32(linked, 0), Le32(linked, 0);
        linked = Cat(linked, Text(manifest));
        linked.resize(linked.size() + 64, 0);
        seed("qt-session-store", linked);
    }

    // Link store: identity.json then connections.json.
    {
        const std::string identity = R"({"version":1,"id":"00112233445566778899aabbccddeeff","name":"Fuzz",)"
                                     R"("secret":"0101010101010101010101010101010101010101010101010101010101010101","port":5035,"network":true})";
        const std::string list = R"({"version":1,"entries":[{"id":"ffeeddccbbaa99887766554433221100","name":"Peer","peerName":"P",)"
                                 R"("key":"0202020202020202020202020202020202020202020202020202020202020202","address":"[::1]:5036",)"
                                 R"("paired":true,"lastSeen":1.7e12,"bluetoothRadio":"there","bluetoothRadioAt":5,"lastHost":"10.0.0.2","lastPort":5035}]})";
        const size_t total = identity.size() + list.size();
        seed("qt-link-store", Cat(Cat(Bytes{uint8_t((identity.size() * 256 + total - 1) / total)}, Text(identity)), Text(list)));
        seed("qt-link-store", Cat(Bytes{255}, Text(R"({"version":1,"id":"00112233445566778899AABBCCDDEEFF","secret":"01","port":-1})")));
    }

    // Link hub lines: the byte before each piece sets its size, so these are mostly one piece each.
    {
        const std::string lines = "\x3f{\"op\":\"watch\",\"on\":true}\n{\"op\":\"pair\",\"address\":\"[fe80::1]:5036\"}\n"
                                  "{\"op\":\"pair\",\"address\":\"host:70000\",\"port\":0}\n{\"op\":\"bt-attach\",\"link\":\"radio\"}\n"
                                  "{\"op\":\"x\",\"beacon\":\"FRLB\\u0001\\u0001AAAAAAAAAAAAAAAA\\u0013\\u00ab\\u0004Name\"}\nnot json\n";
        seed("qt-link-lines", Text(lines));
        Bytes beacon = Text("FRLB");
        beacon.push_back(1), beacon.push_back(2);
        beacon.resize(beacon.size() + 16, 0x11);
        Be(beacon, 5035, 2);
        beacon.push_back(4);
        beacon = Cat(beacon, Text("Peer"));
        seed("qt-link-lines", beacon);
    }

    // Bluetooth H4: the Write_BD_ADDR completion, an event, ACL and SCO data, then reads and a command.
    {
        const Bytes host{4, 0x0e, 4, 1, 1, 0xfc, 0, 4, 0x0e, 4, 1, 3, 0x0c, 0, 2, 1, 0x20, 4, 0, 0xde, 0xad, 0xbe, 0xef, 3, 1, 0, 2, 0xaa, 0xbb};
        Fdp f;
        f.Bool(true);
        f.Int(0, 0, 4).Int(host.size(), 1, 2048).Data(host);
        f.Int(2, 0, 4).Int(100, 1, 4096);
        f.Int(1, 0, 4).Int(4, 0, 300).Data({1, 3, 0x0c, 0}).Bool(true);
        f.Int(3, 0, 4);
        seed("bluetooth-hci", f.Build());
    }

    // Conduit: register, user, open, count, a record read and write, log, close, complete.
    {
        Bytes stream;
        for (const auto& r : {Request(2), Request(4), Request(5).Put(0).Put(0xc0).Text("MemoDB"), Request(9).Put(1),
                              Request(11).Put(0).Put(0), Request(13).Put(1).Put(0).Put(0).Put(1).Text("abc"),
                              Request(27).Text("Conduit log"), Request(28).Text("Device log"), Request(25).Put(0),
                              Request(6).Put(1), Request(0).Put(0)})
            stream = Cat(stream, r.Frame());
        Fdp f;
        f.Bool(false).Int(stream.size(), 0, 65536).Data(stream);
        seed("conduit-ipc", f.Build());
        Fdp g;
        g.Bool(true).Bool(true).Data(Text("name \"C:\\\\Conduits\\\\memo.dll\"\npriority 1\ntitle \"Memo\"\ndirectory \"Memo\"\n"));
        seed("conduit-ipc", g.Build());
    }

    // NetLib: find Net.lib, open, a socket, connect to the echo port, send and receive, close. In
    // the simulated modes and in internet mode (refused on loopback in the fuzz namespace).
    for (uint32_t mode : {1u, 4u}) {
        const uint32_t ip = mode == 4 ? 0x7f000001 : 0x0a000002;
        Bytes memory(600, 0);
        std::memcpy(memory.data(), "Net.lib", 8);
        const Bytes address{0, 2, 0, 7, uint8_t(ip >> 24), uint8_t(ip >> 16), uint8_t(ip >> 8), uint8_t(ip)};
        std::memcpy(memory.data() + 0x80, address.data(), address.size());
        std::memcpy(memory.data() + 0xc0, "echo me!", 8);
        Fdp f;
        f.Int(mode, 0, 5);
        f.Int(0, 0, 7).Int(0x100, 0, 0xffff).Data(memory);
        const auto trap = [&](uint16_t index, const Bytes& stack, bool refnum) {
            f.Int(3, 0, 7).Bool(true).Bool(true).Data(stack).Bool(refnum).Bool(true).Int(index, 0, 28);
        };
        Bytes find(32, 0);
        find[2] = 0x01, find[6] = 0x02;  // name at 0x100, refnum out at 0x200
        trap(0, find, false);
        trap(4, Stack({{2, 0x300, 4}}), true);                                                   // NetLibOpen
        trap(10, Stack({{2, 2, 1}, {4, 1, 1}, {6, 0, 2}, {12, 0x300, 4}}), true);                // socket
        trap(15, Stack({{2, 4, 2}, {4, 0x180, 4}, {8, 8, 2}, {10, 0, 4}, {14, 0x300, 4}}), true);  // connect
        trap(17, Stack({{2, 4, 2}, {4, 0x1c0, 4}, {8, 8, 2}, {22, 0x300, 4}}), true);              // send
        trap(18, Stack({{2, 4, 2}, {4, 0x1d0, 4}, {8, 8, 2}, {24, 0x300, 4}}), true);              // receive
        trap(19, Stack({{2, 8, 2}, {4, 0x1e0, 4}, {16, 0, 4}, {20, 0x300, 4}}), true);             // select
        trap(11, Stack({{2, 4, 2}, {8, 0x300, 4}}), true);                                         // close
        trap(5, Stack({}), true);                                                                  // NetLibClose
        seed("netlib", f.Build());
    }

    // JNI link: a dial and an answer, the handshake carried back and forth, a channel opened and used.
    for (bool pairing : {false, true}) {
        Fdp f;
        f.Data(Text("Beta\\."));
        f.Bool(pairing);
        if (pairing) f.Bool(false);
        else f.Bool(false);
        const auto carry = [&](bool fromA) {
            f.Bool(fromA).Int(10, 0, 5000);
            if (!pairing) f.Int(0, 0, 15).Int(1, 0, 13);
            else f.Int(0, 0, 3);
            f.Int(4096, 0, 4096).Int(4, 0, 7).Int(0, 0, 4095).Int(1, 1, 255).Int(1, 0, 15).Int(0, -1, 4160);
        };
        for (int i = 0; i < 6; ++i) carry(i % 2 == 0);
        if (!pairing) {
            f.Bool(true).Int(10, 0, 5000).Int(0, 0, 15).Int(4, 0, 13).Data(Text("m515\\.IrDA\\.")).Int(1, 0, 4);
            for (int i = 0; i < 2; ++i) carry(true), carry(false);
            f.Bool(true).Int(10, 0, 5000).Int(2, 0, 15).Int(6, 0, 13).Int(1, 0, 15).Int(0, 0, 3000).Int(0, -4, 3004).Int(0, -4, 3064)
                .Int(16, 1, 3000).Data(Bytes(16, 0x55));
            for (int i = 0; i < 2; ++i) carry(true), carry(false);
            f.Bool(false).Int(10, 0, 5000).Int(0, 0, 15).Int(8, 0, 13);
        }
        seed("jni-link", f.Build());
    }

    // JNI sync: make a profile, list, save and read a record, queue an install from the file part.
    seed("jni-sync", SyncInput(R"({"op":"create","name":"Fuzz"})", {}));
    seed("jni-sync", SyncInput(R"({"op":"profiles","device":"palm-1"})", {}));
    seed("jni-sync", SyncInput(R"({"op":"assign","profile":1,"device":"palm-1","kind":"palm"})", {}));
    seed("jni-sync", SyncInput(R"({"op":"save","profile":1,"kind":"contacts","record":{"category":"Work","fields":{"name":"A"},"extras":{"raw":"00ff"}}})", {}));
    seed("jni-sync", SyncInput(R"({"op":"records","profile":1,"kind":"contacts"})", {}));
    seed("jni-sync", SyncInput(R"({"op":"import","profile":1,"kind":"memos","rows":[{"fields":{"text":"m"}},{"fields":{"text":"n"}}]})", {}));
    {
        Bytes pdb(78, 0);
        std::memcpy(pdb.data(), "Fuzz", 4);
        std::memcpy(pdb.data() + 60, "DATAFuzz", 8);
        seed("jni-sync", SyncInput(R"({"op":"install","profile":1,"device":"palm-1","name":"fuzz.pdb","path":"x"})", pdb));
        seed("jni-sync", SyncInput(R"({"op":"installer-files","profile":1,"name":"fuzz.pdb","path":"x"})", pdb));
    }
    seed("jni-sync", SyncInput(R"({"op":"log","profile":1})", {}));
}
