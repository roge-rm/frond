#include <QtTest>
#include <zlib.h>
#include "../zodiac-rom/ZodiacRom.h"

using namespace frondZodiacRom;
using Bytes = std::vector<uint8_t>;

static void be(Bytes& b, uint32_t v, int n) { for (int i = n - 1; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i))); }
static void le(Bytes& b, uint32_t v, int n) { for (int i = 0; i < n; ++i) b.push_back(uint8_t(v >> (8 * i))); }

static Bytes deflated(const Bytes& in, int windowBits) {
    z_stream z{};
    deflateInit2(&z, 6, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY);
    Bytes out(deflateBound(&z, uLong(in.size())) + 64);
    z.next_in = const_cast<Bytef*>(in.data()); z.avail_in = uInt(in.size());
    z.next_out = out.data(); z.avail_out = uInt(out.size());
    deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}

// A made-up "ROM" and the values Expected would hold for it.
static Bytes payload() {
    Bytes rom;
    for (int i = 0; i < 5000; ++i) rom.push_back(uint8_t((i * 7) ^ (i >> 3)));
    return rom;
}
static Expected expectedFor(const Bytes& rom, std::string& hash) {
    hash = sha256Hex(rom.data(), rom.size());
    return {rom.size(), hash.c_str()};
}

// A resource database with a decoy resource before and after the ROMZ ones.
static Bytes prc(const Bytes& romz2, const char* type = "ROMZ", uint16_t id = 2) {
    const Bytes decoy(40, 0x55), other(30, 0x66);
    Bytes b(0x20, 0);
    std::memcpy(b.data(), "update", 6);
    be(b, 1, 2); be(b, 0, 2); be(b, 0, 4); be(b, 0, 4); be(b, 0, 4); be(b, 0, 4); be(b, 0, 4); be(b, 0, 4);
    be(b, 0x61707072, 4); be(b, 0x54574156, 4); be(b, 0, 4); be(b, 0, 4);
    be(b, 3, 2);
    const uint32_t first = 0x4e + 3 * 10;
    auto entry = [&](const char* t, uint16_t i, uint32_t at) { b.insert(b.end(), t, t + 4); be(b, i, 2); be(b, at, 4); };
    entry("ROMZ", 1, first);
    entry(type, id, first + uint32_t(decoy.size()));
    entry("code", 0, first + uint32_t(decoy.size() + romz2.size()));
    b.insert(b.end(), decoy.begin(), decoy.end());
    b.insert(b.end(), romz2.begin(), romz2.end());
    b.insert(b.end(), other.begin(), other.end());
    return b;
}

static Bytes zip(const Bytes& file, const char* name, bool deflate) {
    const Bytes packed = deflate ? deflated(file, -15) : file;
    const uint32_t crc = crc32(0, file.data(), uInt(file.size()));
    Bytes b;
    le(b, 0x04034b50, 4); le(b, 20, 2); le(b, 0, 2); le(b, deflate ? 8 : 0, 2); le(b, 0, 4);
    le(b, crc, 4); le(b, uint32_t(packed.size()), 4); le(b, uint32_t(file.size()), 4);
    le(b, uint32_t(strlen(name)), 2); le(b, 0, 2);
    b.insert(b.end(), name, name + strlen(name));
    b.insert(b.end(), packed.begin(), packed.end());
    const uint32_t directory = uint32_t(b.size());
    le(b, 0x02014b50, 4); le(b, 20, 2); le(b, 20, 2); le(b, 0, 2); le(b, deflate ? 8 : 0, 2); le(b, 0, 4);
    le(b, crc, 4); le(b, uint32_t(packed.size()), 4); le(b, uint32_t(file.size()), 4);
    le(b, uint32_t(strlen(name)), 2); le(b, 0, 6); le(b, 0, 2); le(b, 0, 4); le(b, 0, 4);
    b.insert(b.end(), name, name + strlen(name));
    const uint32_t size = uint32_t(b.size()) - directory;
    le(b, 0x06054b50, 4); le(b, 0, 4); le(b, 1, 2); le(b, 1, 2); le(b, size, 4); le(b, directory, 4); le(b, 0, 2);
    return b;
}

class ZodiacRomTest : public QObject {
    Q_OBJECT
 private slots:
    void realConstants() {
        QCOMPARE(zodiac().length, size_t(8192000));
        QCOMPARE(sha256Hex(nullptr, 0), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
        const Bytes abc{'a', 'b', 'c'};
        QCOMPARE(sha256Hex(abc.data(), 3), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    }
    void prcAndZips() {
        const auto rom = payload();
        std::string hash;
        const auto expected = expectedFor(rom, hash);
        const auto file = prc(deflated(rom, 31));
        auto r = extract(file.data(), file.size(), expected);
        QVERIFY(r.status == Result::Status::ok);
        QCOMPARE(r.rom, rom);
        for (bool deflate : {false, true}) {
            const auto z = zip(file, "Zodiac/zodiac_1.1_update.PRC", deflate);
            r = extract(z.data(), z.size(), expected);
            QVERIFY(r.status == Result::Status::ok);
            QCOMPARE(r.rom, rom);
        }
    }
    void rawRom() {
        const auto rom = payload();
        std::string hash;
        const auto expected = expectedFor(rom, hash);
        auto r = extract(rom.data(), rom.size(), expected);
        QVERIFY(r.status == Result::Status::ok);
        QCOMPARE(r.rom, rom);
        auto other = rom;
        other[10] ^= 1;
        r = extract(other.data(), other.size(), expected);
        QVERIFY(r.status == Result::Status::notUpdate);
    }
    void wrongHashOrLength() {
        const auto rom = payload();
        std::string hash;
        auto expected = expectedFor(rom, hash);
        const auto file = prc(deflated(rom, 31));
        expected.sha256 = "0000000000000000000000000000000000000000000000000000000000000000";
        auto r = extract(file.data(), file.size(), expected);
        QVERIFY(r.status == Result::Status::error);
        QCOMPARE(r.error, std::string("This isn't the Zodiac 1.1 update."));
        expected = expectedFor(rom, hash);
        expected.length = rom.size() - 1;
        r = extract(file.data(), file.size(), expected);
        QVERIFY(r.status == Result::Status::error);
        expected.length = rom.size() + 1;
        r = extract(file.data(), file.size(), expected);
        QVERIFY(r.status == Result::Status::error);
    }
    void missingRomz() {
        const auto rom = payload();
        std::string hash;
        const auto expected = expectedFor(rom, hash);
        for (auto file : {prc(deflated(rom, 31), "ROMZ", 3), prc(deflated(rom, 31), "code", 2)}) {
            auto r = extract(file.data(), file.size(), expected);
            QVERIFY(r.status == Result::Status::error);
            QCOMPARE(r.error, std::string("This isn't the Zodiac 1.1 update."));
            const auto z = zip(file, "x.prc", true);
            r = extract(z.data(), z.size(), expected);
            QVERIFY(r.status == Result::Status::error);
        }
        const auto z = zip(Bytes(100, 1), "readme.txt", false);
        const auto r = extract(z.data(), z.size(), expected);
        QVERIFY(r.status == Result::Status::error);
        QCOMPARE(r.error, std::string("This isn't the Zodiac 1.1 update."));
    }
    void damaged() {
        const auto rom = payload();
        std::string hash;
        const auto expected = expectedFor(rom, hash);
        const auto gz = deflated(rom, 31);
        // A gzip stream cut short, one with its tail damaged, and a PRC list that runs off the end.
        auto cut = prc(Bytes(gz.begin(), gz.begin() + gz.size() / 2));
        QVERIFY(extract(cut.data(), cut.size(), expected).status == Result::Status::error);
        auto bad = gz;
        bad[bad.size() - 6] ^= 0xff;
        auto damagedPrc = prc(bad);
        QVERIFY(extract(damagedPrc.data(), damagedPrc.size(), expected).status == Result::Status::error);
        auto file = prc(gz);
        file.resize(0x60);
        QVERIFY(extract(file.data(), file.size(), expected).status == Result::Status::error);
        // A truncated zip loses its directory; a damaged entry is refused.
        file = prc(gz);
        auto z = zip(file, "u.prc", true);
        z.resize(z.size() - 30);
        QVERIFY(extract(z.data(), z.size(), expected).status == Result::Status::error);
        z = zip(file, "u.prc", true);
        z[40] ^= 0xff; z[41] ^= 0xff; z[42] ^= 0xff;
        QVERIFY(extract(z.data(), z.size(), expected).status == Result::Status::error);
        z = zip(file, "u.prc", true);
        z.resize(60);
        QVERIFY(extract(z.data(), z.size(), expected).status == Result::Status::error);
        for (size_t n : {size_t(0), size_t(3), size_t(100)}) {
            const Bytes junk(n, 0x41);
            QVERIFY(extract(junk.data(), junk.size(), expected).status == Result::Status::notUpdate);
        }
    }
};

QTEST_APPLESS_MAIN(ZodiacRomTest)
#include "zodiac-rom.moc"
