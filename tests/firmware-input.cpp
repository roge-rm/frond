#include <QtTest>
#include <QTemporaryDir>
#include "FirmwareInput.h"

using frondFirmware::Device;
// Sparse synthetic E2 NOR: card header at 'at' with the Palm company id and the E2 HAL id.
static void e2Nor(QFile& nor, qint64 size, qint64 at, quint32 hal = 0x68737072) {
    nor.resize(0); nor.resize(size);
    auto put = [&](qint64 offset, quint32 value) {
        const char bytes[4] = {char(value), char(value >> 8), char(value >> 16), char(value >> 24)};
        nor.seek(offset); nor.write(bytes, 4);
    };
    put(at + 8, 0xfeedbeef); put(at + 0x80, 0x50616c6d); put(at + 0x84, hal);
    nor.flush();
}

// Sparse synthetic Zire 72 NOR: PalmCard small card at 0x0 and big card at 'bigAt' running
// 'bigLength' bytes, both with the Palm company id and 'hal'.
static void zireNor(QFile& nor, qint64 size = 14942208, quint32 hal = 0x615a3732, qint64 smallAt = 0,
                    qint64 bigAt = 0x20000, qint64 bigLength = 14942208 - 0x20000) {
    nor.resize(0); nor.resize(size);
    auto put = [&](qint64 offset, quint32 value) {
        const char bytes[4] = {char(value), char(value >> 8), char(value >> 16), char(value >> 24)};
        nor.seek(offset); nor.write(bytes, 4);
    };
    for (auto [at, length] : {std::pair<qint64, qint64>{smallAt, 0x10000}, {bigAt, bigLength}}) {
        if (at + 0x90 > size) continue;
        put(at + 8, 0xfeedbeef); nor.seek(at + 0x10); nor.write("PalmCard", 9);
        put(at + 0x6c, quint32(length)); put(at + 0x80, 0x50616c6d); put(at + 0x84, hal);
    }
    nor.flush();
}

// Sparse synthetic 68k ROM: the big-endian card header at 0 with the reset vector in the ROM at 0x10c00000.
static void rom68k(QFile& rom, qint64 size, quint16 version, quint16 flags = 0x0020, quint32 company = 0, quint32 hal = 0,
                   quint32 resetOffset = 0x822a) {
    rom.resize(0); rom.resize(size);
    auto be = [&](qint64 at, quint32 value, int bytes) {
        char out[4];
        for (int i = 0; i < bytes; ++i) out[i] = char(value >> (8 * (bytes - 1 - i)));
        rom.seek(at); rom.write(out, bytes);
    };
    be(0, 0x3000, 4); be(4, 0x10c00000 + resetOffset, 4); be(8, 0xfeedbeef, 4);
    be(0x0c, version, 2); be(0x0e, flags, 2);
    rom.seek(0x10); rom.write("PalmCard", 9);
    be(0x6c, quint32(size), 4);
    if (version >= 5) { be(0x7e, company, 4); be(0x82, hal, 4); }
    rom.flush();
}

class FirmwareInputTest : public QObject {
    Q_OBJECT
 private slots:
    void txFiles() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto norPath = dir.filePath("synthetic-nor");
        const auto nandPath = dir.filePath("synthetic-nand");
        QVERIFY(!frondFirmware::validateTxFiles(norPath, nandPath).isEmpty());
        QFile nor(norPath), nand(nandPath);
        QVERIFY(nor.open(QIODevice::ReadWrite)); QVERIFY(nand.open(QIODevice::ReadWrite));
        QVERIFY(nor.resize(16u << 20)); QVERIFY(nand.resize(2112u * 64 * 1024));
        // Sparse synthetic files exercise geometry without firmware data.
        QVERIFY(frondFirmware::validateTxFiles(norPath, nandPath).isEmpty());
        QVERIFY(nand.resize(128u << 20)); // NAND without spare bytes.
        QVERIFY(!frondFirmware::validateTxFiles(norPath, nandPath).isEmpty());
        QVERIFY(nand.resize(2112u * 64 * 1024));
        QVERIFY(nor.resize((16u << 20) - 1));
        QVERIFY(!frondFirmware::validateTxFiles(norPath, nandPath).isEmpty());
        QVERIFY(!frondFirmware::validateTxFiles(dir.path(), nandPath).isEmpty());
        QVERIFY(!frondFirmware::validateTxFiles(nandPath, norPath).isEmpty());
    }
    void e2Files() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto norPath = dir.filePath("synthetic-nor"), nandPath = dir.filePath("synthetic-nand");
        QFile nor(norPath), nand(nandPath);
        QVERIFY(nor.open(QIODevice::ReadWrite)); QVERIFY(nand.open(QIODevice::ReadWrite));
        e2Nor(nor, 8 << 20, 0x20000);
        QVERIFY(nand.resize(34603008));
        Device device = Device::automatic;
        // NAND is optional for the E2; when given it must be the raw size.
        QVERIFY(frondFirmware::validateFiles(Device::e2, norPath, nandPath, &device).isEmpty());
        QCOMPARE(device, Device::e2);
        QVERIFY(frondFirmware::validateFiles(Device::e2, norPath, {}).isEmpty());
        QVERIFY(frondFirmware::validateFiles(Device::automatic, norPath, nandPath, &device).isEmpty());
        QCOMPARE(device, Device::e2);
        QCOMPARE(frondFirmware::detectDevice(norPath), Device::e2);
        QVERIFY(nand.resize(34603008 - 528));
        QVERIFY(!frondFirmware::validateFiles(Device::e2, norPath, nandPath).isEmpty());
        QVERIFY(!frondFirmware::validateFiles(Device::e2, norPath, dir.filePath("missing")).isEmpty());
        // Wrong NOR size or header place.
        e2Nor(nor, 16 << 20, 0x20000);
        QVERIFY(!frondFirmware::validateFiles(Device::e2, norPath, {}).isEmpty());
        e2Nor(nor, 8 << 20, 0x40000);
        QVERIFY(frondFirmware::validateFiles(Device::e2, norPath, {}).contains("0x20000"));
        e2Nor(nor, 8 << 20, 0);
        QVERIFY(!frondFirmware::validateFiles(Device::automatic, norPath, {}).isEmpty());
        // No header, or a header for another device.
        QVERIFY(nor.resize(0)); QVERIFY(nor.resize(8 << 20));
        QVERIFY(!frondFirmware::validateFiles(Device::e2, norPath, {}).isEmpty());
        QCOMPARE(frondFirmware::detectDevice(norPath), Device::automatic);
        QVERIFY(!frondFirmware::validateFiles(Device::automatic, norPath, {}).isEmpty());
        e2Nor(nor, 8 << 20, 0x20000, 0x446d4752);
        QVERIFY(!frondFirmware::validateFiles(Device::e2, norPath, {}).isEmpty());
    }
    void e2DenvfsFiles() {
        QTemporaryDir dir;
        const auto path = dir.filePath("denvfs"), nandPath = dir.filePath("nand");
        QFile nor(path), nand(nandPath);
        QVERIFY(nor.open(QIODevice::ReadWrite)); QVERIFY(nand.open(QIODevice::ReadWrite));
        auto make = [&] {
            e2Nor(nor, 6 << 20, 0x20000);
            nor.seek(0x20010); nor.write("rePalm", 7);
            nor.seek(0x20030); nor.write("Dmitry.GR", 10);
            const quint32 length = (6 << 20) - 0x20000;
            const char bytes[4] = {char(length), char(length >> 8), char(length >> 16), char(length >> 24)};
            nor.seek(0x2006c); nor.write(bytes, 4); nor.flush();
        };
        make();
        Device resolved = Device::automatic;
        QCOMPARE(frondFirmware::detectDevice(path), Device::e2);
        QVERIFY(frondFirmware::validateFiles(Device::automatic, path, {}, &resolved).isEmpty());
        QCOMPARE(resolved, Device::e2);
        QVERIFY(frondFirmware::validateFiles(Device::e2, path, {}).isEmpty());
        QVERIFY(!frondFirmware::validateFiles(Device::tx, path, {}).isEmpty());
        const auto e3 = frondFirmware::deviceFromId("palm-e2-franken");
        const auto chosenE3 = frondFirmware::validateFiles(e3, path, {});
        QVERIFY(chosenE3.contains("This is a") && chosenE3.contains("Choose Tungsten E2"));
        QVERIFY(!chosenE3.contains("must be"));
        QVERIFY(nand.resize(34603008));
        QVERIFY(frondFirmware::validateFiles(Device::e2, path, nandPath).isEmpty());
        QVERIFY(nand.resize(34603008 - 528));
        QVERIFY(frondFirmware::validateFiles(Device::e2, path, nandPath).contains("NAND"));
        // Every field of the alternate geometry must match, including string terminators.
        for (qint64 at : {0x20010, 0x20016, 0x20030, 0x20039, 0x2006c, 0x20080, 0x20084}) {
            make(); nor.seek(at); nor.write("x", 1); nor.flush();
            QVERIFY(!frondFirmware::validateFiles(Device::e2, path, {}).isEmpty());
            QVERIFY(!frondFirmware::validateFiles(Device::automatic, path, {}).isEmpty());
        }
        make(); QVERIFY(nor.resize((6 << 20) - 1));
        QVERIFY(!frondFirmware::validateFiles(Device::e2, path, {}).isEmpty());
        make();
        nor.seek(0x20000); const auto header = nor.read(0x88);
        nor.seek(0x20000); nor.write(QByteArray(0x88, '\0'));
        nor.seek(0x40000); nor.write(header); nor.flush();
        QVERIFY(!frondFirmware::validateFiles(Device::e2, path, {}).isEmpty());
        e2Nor(nor, 6 << 20, 0x20000);
        QVERIFY(!frondFirmware::validateFiles(Device::e2, path, {}).isEmpty());
    }
    void e3Files() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto e3 = frondFirmware::deviceFromId("palm-e2-franken");
        QVERIFY(e3 != Device::automatic);
        QCOMPARE(frondFirmware::deviceId(e3), QString("palm-e2-franken"));
        QCOMPARE(frondFirmware::deviceName(e3), QString("Tungsten E3"));
        const auto& profile = frondFirmware::profile(e3);
        QCOMPARE(profile.frontend.width, uint16_t(320));
        QCOMPARE(profile.frontend.height, uint16_t(480));
        QCOMPARE(profile.frontend.silkscreenHeight, uint16_t(0));
        QVERIFY(!QString::fromLatin1(profile.frontend.buttons).split('|').contains("power"));
        const auto caps = frondFirmware::capabilities(e3);
        QVERIFY(caps.sound && caps.serial && caps.keyboard && caps.usb && caps.infrared && caps.bluetooth);
        QVERIFY(!caps.network && caps.fastAudio && !caps.microphone);
        QCOMPARE(QString::fromLatin1(profile.frontend.fastAudioNorMd5), QString("a79f98bddd50c61e7eb89eee46eb81bd"));
        QVERIFY(frondFirmware::hasHotSyncButton(e3));
        for (auto device : {Device::tx, Device::e2, frondFirmware::deviceFromId("palm-iiix")})
            QVERIFY(frondFirmware::hasHotSyncButton(device));
        QVERIFY(!frondFirmware::hasHotSyncButton(Device::automatic));
        QVERIFY(!frondFirmware::hasHotSyncButton(frondFirmware::deviceFromId("sony-clie-tg50")));
        const auto norPath = dir.filePath("synthetic-e3"), nandPath = dir.filePath("synthetic-nand");
        QFile nor(norPath), nand(nandPath);
        QVERIFY(nor.open(QIODevice::ReadWrite)); QVERIFY(nand.open(QIODevice::ReadWrite));
        e2Nor(nor, 8 << 20, 0x20000, 0x446d4752);
        QCOMPARE(frondFirmware::detectDevice(norPath), e3);
        Device resolved = Device::automatic;
        QVERIFY(frondFirmware::validateFiles(Device::automatic, norPath, {}, &resolved).isEmpty());
        QCOMPARE(resolved, e3);
        QVERIFY(frondFirmware::validateFiles(e3, norPath, {}).isEmpty());
        QVERIFY(nand.resize(34603008));
        QVERIFY(frondFirmware::validateFiles(e3, norPath, nandPath).isEmpty());
        QVERIFY(nand.resize(34603008 - 528));
        QVERIFY(!frondFirmware::validateFiles(e3, norPath, nandPath).isEmpty());
        QVERIFY(!frondFirmware::validateFiles(e3, norPath, dir.filePath("missing")).isEmpty());
        QVERIFY(frondFirmware::validateFiles(Device::tx, norPath, {}).contains("Choose Tungsten E3"));
        QVERIFY(frondFirmware::validateFiles(Device::e2, norPath, {}).contains("Choose Tungsten E3"));
        e2Nor(nor, 16 << 20, 0x20000, 0x446d4752);
        QVERIFY(!frondFirmware::validateFiles(e3, norPath, {}).isEmpty());
        e2Nor(nor, 8 << 20, 0x40000, 0x446d4752);
        QVERIFY(frondFirmware::validateFiles(e3, norPath, {}).contains("invalid card layout"));
        e2Nor(nor, 8 << 20, 0x20000);
        QVERIFY(frondFirmware::validateFiles(e3, norPath, {}).contains("Choose Tungsten E2"));
        QVERIFY(nor.resize(0)); QVERIFY(nor.resize(8 << 20));
        QVERIFY(!frondFirmware::validateFiles(e3, norPath, {}).isEmpty());
    }
    void zire72Files() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto norPath = dir.filePath("zire-nor"), nandPath = dir.filePath("nand");
        QFile nor(norPath), nand(nandPath);
        QVERIFY(nor.open(QIODevice::ReadWrite)); QVERIFY(nand.open(QIODevice::ReadWrite)); QVERIFY(nand.resize(34603008));
        zireNor(nor);
        Device device = Device::automatic;
        QVERIFY(frondFirmware::validateFiles(Device::zire72, norPath, {}, &device).isEmpty());
        QCOMPARE(device, Device::zire72);
        device = Device::automatic;
        QVERIFY(frondFirmware::validateFiles(Device::automatic, norPath, {}, &device).isEmpty());
        QCOMPARE(device, Device::zire72);
        QCOMPARE(frondFirmware::detectDevice(norPath), Device::zire72);
        // No NAND, and no other device takes it.
        QVERIFY(frondFirmware::validateFiles(Device::zire72, norPath, nandPath).contains("no NAND"));
        QVERIFY(frondFirmware::validateFiles(Device::tx, norPath, nandPath).contains("Choose Zire 72"));
        QVERIFY(frondFirmware::validateFiles(Device::e2, norPath, {}).contains("Choose Zire 72"));
        // Wrong sizes, a misplaced small card, a big card that doesn't reach the end or isn't
        // there, and another device's HAL are refused.
        zireNor(nor, 14942208 - 0x10000);
        QVERIFY(frondFirmware::validateFiles(Device::zire72, norPath, {}).contains("14,942,208"));
        zireNor(nor, 16 << 20);
        QVERIFY(!frondFirmware::validateFiles(Device::zire72, norPath, {}).isEmpty());
        zireNor(nor, 14942208, 0x615a3732, 0x8000);
        QVERIFY(frondFirmware::validateFiles(Device::zire72, norPath, {}).contains("0x20000"));
        zireNor(nor, 14942208, 0x615a3732, 0, 0x20000, 0x10000);
        QVERIFY(frondFirmware::validateFiles(Device::zire72, norPath, {}).contains("0x20000"));
        zireNor(nor, 14942208, 0x615a3732, 0, 0x40000);
        QVERIFY(!frondFirmware::validateFiles(Device::zire72, norPath, {}).isEmpty());
        zireNor(nor, 14942208, 0x68737072);
        QVERIFY(!frondFirmware::validateFiles(Device::zire72, norPath, {}).isEmpty());
        // An E2 NOR isn't a Zire's.
        e2Nor(nor, 8 << 20, 0x20000);
        QVERIFY(!frondFirmware::validateFiles(Device::zire72, norPath, {}).isEmpty());
        QCOMPARE(frondFirmware::detectDevice(norPath), Device::e2);
    }
    void noFallbackBetweenDevices() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto e2Path = dir.filePath("e2-nor"), txPath = dir.filePath("tx-nor"), nandPath = dir.filePath("nand");
        QFile e2(e2Path), tx(txPath), nand(nandPath);
        QVERIFY(e2.open(QIODevice::ReadWrite)); QVERIFY(tx.open(QIODevice::ReadWrite)); QVERIFY(nand.open(QIODevice::ReadWrite));
        e2Nor(e2, 8 << 20, 0x20000);
        QVERIFY(tx.resize(16u << 20)); QVERIFY(nand.resize(2112u * 64 * 1024));
        QCOMPARE(frondFirmware::detectDevice(txPath), Device::tx);
        Device device = Device::automatic;
        QVERIFY(frondFirmware::validateFiles(Device::automatic, txPath, nandPath, &device).isEmpty());
        QCOMPARE(device, Device::tx);
        // A TX choice with an E2 NOR, and an E2 choice with a TX NOR, are refused.
        QVERIFY(frondFirmware::validateFiles(Device::tx, e2Path, nandPath).contains("Tungsten E2"));
        QVERIFY(!frondFirmware::validateFiles(Device::e2, txPath, {}).isEmpty());
        // The TX NAND size is not an E2 NAND, and the E2 NAND size is not a TX NAND.
        QVERIFY(!frondFirmware::validateFiles(Device::e2, e2Path, nandPath).isEmpty());
        QVERIFY(nand.resize(34603008));
        QVERIFY(!frondFirmware::validateFiles(Device::tx, txPath, nandPath).isEmpty());
        QVERIFY(!frondFirmware::validateFiles(Device::tx, txPath, {}).isEmpty());
    }
    // The IIIx takes one ROM file, told from its big-endian card header: the 3.1 retail and debug
    // ROMs, the generic OS 3.5 EZ ROMs and the IIIx/V 4.1 ROM.
    void iiixRoms() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto iiix = frondFirmware::deviceFromId("palm-iiix");
        QVERIFY(iiix != Device::automatic && frondFirmware::runs68k(iiix));
        QVERIFY(std::find(frondFirmware::devices.begin(), frondFirmware::devices.end(), iiix) != frondFirmware::devices.end());
        for (auto other : {Device::tx, Device::e2, Device::zire72, frondFirmware::deviceFromId("sony-clie-tg50")})
            QVERIFY(!frondFirmware::runs68k(other));
        const auto romPath = dir.filePath("rom"), nandPath = dir.filePath("nand");
        QFile rom(romPath), nand(nandPath);
        QVERIFY(rom.open(QIODevice::ReadWrite)); QVERIFY(nand.open(QIODevice::ReadWrite)); QVERIFY(nand.resize(34603008));
        struct Kind { const char* name; qint64 size; quint16 version; quint32 company, hal, reset; };
        const Kind kinds[] = {
            {"3.1 retail", 0x128000, 3, 0, 0, 0x822a}, {"3.1 debug", 0x128000, 3, 0, 0, 0x8236},
            {"3.5 EZ", 0x178000, 4, 0, 0, 0x90e0}, {"3.5 EZ debug", 0x178000, 4, 0, 0, 0x9184},
            {"4.1", 0x1a8000, 5, 0x70616c6d, 0x73756d6f, 0xc6ea}};
        for (const auto& kind : kinds) {
            rom68k(rom, kind.size, kind.version, 0x0020, kind.company, kind.hal, kind.reset);
            QCOMPARE(frondFirmware::detectDevice(romPath), iiix);
            Device device = Device::automatic;
            QVERIFY2(frondFirmware::validateFiles(Device::automatic, romPath, {}, &device).isEmpty(), kind.name);
            QCOMPARE(device, iiix);
            device = Device::automatic;
            QVERIFY2(frondFirmware::validateFiles(iiix, romPath, {}, &device).isEmpty(), kind.name);
            QCOMPARE(device, iiix);
            // No NAND, and no ARM device takes it.
            QVERIFY(frondFirmware::validateFiles(iiix, romPath, nandPath).contains("no NAND"));
            for (auto other : {Device::tx, Device::e2, Device::zire72})
                QVERIFY2(frondFirmware::validateFiles(other, romPath, {}).contains("Choose Palm IIIx"), kind.name);
        }
        // A 68328-only ROM, a header version 2 ROM and another 4.1 device's IDs have the header but can't run.
        rom68k(rom, 0x128000, 3, 0x0010);
        QVERIFY(frondFirmware::validateFiles(iiix, romPath, {}).contains("can run"));
        QCOMPARE(frondFirmware::detectDevice(romPath), Device::automatic);
        rom68k(rom, 0x128000, 2);
        QVERIFY(frondFirmware::validateFiles(iiix, romPath, {}).contains("can run"));
        rom68k(rom, 0x1a8000, 5, 0x0020, 0x70616c6d, 0x6173746e);
        QVERIFY(frondFirmware::validateFiles(iiix, romPath, {}).contains("can run"));
        // Over the 2 MiB flash part, or truncated.
        rom68k(rom, (2 << 20) + 0x1000, 3);
        QVERIFY(!frondFirmware::validateFiles(iiix, romPath, {}).isEmpty());
        rom68k(rom, 0x128000, 3); QVERIFY(rom.resize(0x127000));
        QVERIFY(!frondFirmware::validateFiles(iiix, romPath, {}).isEmpty());
        // ARM NOR images aren't IIIx ROMs.
        e2Nor(rom, 8 << 20, 0x20000);
        QVERIFY(frondFirmware::validateFiles(iiix, romPath, {}).contains("isn't a Palm IIIx ROM"));
        QCOMPARE(frondFirmware::detectDevice(romPath), Device::e2);
        e2Nor(rom, 0x128000, 0);
        QVERIFY(frondFirmware::validateFiles(iiix, romPath, {}).contains("isn't a Palm IIIx ROM"));
        // Its hardware: the piezo, the cradle serial port and IrDA.
        const auto caps = frondFirmware::capabilities(iiix);
        QVERIFY(caps.sound && caps.serial && caps.infrared);
        QVERIFY(!caps.network && !caps.keyboard && !caps.usb && !caps.bluetooth && !caps.microphone && !caps.fastAudio);
        QCOMPARE(frondFirmware::profile(iiix).nandSize, size_t(0));
    }
    void deviceNames() {
        // Settings and the core get the fixed name; it round-trips, and other names detect.
        for (auto device : {Device::tx, Device::e2, Device::zire72})
            QCOMPARE(frondFirmware::deviceFromId(frondFirmware::deviceId(device)), device);
        QCOMPARE(frondFirmware::deviceId(Device::zire72), QString("palm-zire-72"));
        QCOMPARE(frondFirmware::deviceId(Device::tx), QString("palm-tx"));
        QCOMPARE(frondFirmware::deviceId(Device::e2), QString("palm-e2"));
        QVERIFY(frondFirmware::deviceId(Device::automatic).isEmpty());
        for (const char* other : {"", "1", "tungsten-e2", "palm-zire72", "staged-fixture"})
            QCOMPARE(frondFirmware::deviceFromId(other), Device::automatic);
        QCOMPARE(frondFirmware::deviceId(frondFirmware::deviceFromId("sony-clie-tg50")), QString("sony-clie-tg50"));
        QVERIFY(frondFirmware::capabilities(Device::e2).serial && !frondFirmware::capabilities(Device::e2).network && frondFirmware::capabilities(Device::tx).network);
        // The Zire 72 has no serial port or network but has low-latency audio; its camera isn't a setting.
        const auto zire = frondFirmware::capabilities(Device::zire72);
        QVERIFY(!zire.serial && !zire.network && zire.fastAudio && zire.usb && zire.infrared && zire.keyboard && zire.bluetooth);
    }
};
QTEST_GUILESS_MAIN(FirmwareInputTest)
#include "firmware-input.moc"
