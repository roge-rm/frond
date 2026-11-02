#include <QtTest>
#include "FirmwareStore.h"
#include "SessionStore.h"
#include "AudioQualification.h"

using namespace frondSessions;

static QByteArray header(quint32 version, const QByteArray& device, quint32 magicValue = frondSessions::magic) {
    QByteArray bytes;
    for (quint32 value : {magicValue, version})
        for (int i = 0; i < 4; i++) bytes.append(char(value >> (8 * i)));
    bytes.append(device.leftJustified(32, '\0', true));
    return bytes + "body";
}
static void write(const QString& path, const QByteArray& data) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(data);
}

class SessionStoreTest : public QObject {
    Q_OBJECT
    QTemporaryDir directory{QDir::currentPath() + "/build/session-store-XXXXXX"};
    QString folder() const { return directory.filePath("sessions"); }
 private slots:
    void initTestCase() { QVERIFY(directory.isValid()); QVERIFY(QDir().mkpath(folder())); }
    void cleanup() { QDir(folder()).removeRecursively(); QDir().mkpath(folder()); }

    void headers() {
        write(folder() + "/a.frond", header(6, "palm-tx"));
        const auto h = readHeader(folder() + "/a.frond");
        QVERIFY(h.readable && openable(h));
        QCOMPARE(h.device, QString("palm-tx"));
        QVERIFY(!openable(readHeader(folder() + "/missing.frond")));
        write(folder() + "/old.frond", header(5, "palm-tx"));
        QVERIFY(!openable(readHeader(folder() + "/old.frond")));
        write(folder() + "/unknown.frond", header(6, "palm-zzz"));
        QVERIFY(!openable(readHeader(folder() + "/unknown.frond")));
        write(folder() + "/text.frond", "session");
        QVERIFY(!openable(readHeader(folder() + "/text.frond")));
    }
    void listRows() {
        write(folder() + "/Tx.frond", header(6, "palm-tx"));
        write(folder() + "/Old.frond", header(5, "palm-tx"));
        QVERIFY(create(folder(), "Fresh", "palm-e2"));
        QFile::setPermissions(folder() + "/Tx.frond", QFile::ReadOwner | QFile::WriteOwner);
        QFile tx(folder() + "/Tx.frond"); QVERIFY(tx.open(QIODevice::ReadWrite));
        tx.setFileTime(QDateTime::currentDateTime().addDays(-1), QFileDevice::FileModificationTime);
        QFile old(folder() + "/Old.frond"); QVERIFY(old.open(QIODevice::ReadWrite));
        old.setFileTime(QDateTime::currentDateTime().addDays(-2), QFileDevice::FileModificationTime);
        const auto sessions = list(folder());
        QCOMPARE(sessions.size(), 3);
        QCOMPARE(sessions[0].name, QString("Fresh"));
        QVERIFY(!sessions[0].saved && !sessions[0].old);
        QCOMPARE(sessions[0].device, QString("palm-e2"));
        QCOMPARE(sessions[1].name, QString("Tx"));
        QVERIFY(sessions[1].saved && !sessions[1].old);
        QCOMPARE(sessions[1].size, qint64(header(6, "palm-tx").size()));
        QCOMPARE(sessions[2].name, QString("Old"));
        QVERIFY(sessions[2].old);
        // Saving replaces the marker; the session is listed once.
        write(folder() + "/Fresh.frond", header(6, "palm-e2"));
        QCOMPARE(list(folder()).size(), 3);
        // Available sessions open; a reserved device stays unavailable.
        write(folder() + "/Zire.frond", header(6, "palm-zire-72"));
        write(folder() + "/Clie.frond", header(6, "sony-clie-tg50"));
        write(folder() + "/Reserved.frond", header(6, "palm-iiix"));
        for (const auto& info : list(folder())) {
            if (info.name == "Zire") QVERIFY(info.saved && !info.old && info.device == "palm-zire-72");
            if (info.name == "Clie") QVERIFY(info.saved && !info.old && info.device == "sony-clie-tg50");
            if (info.name == "Reserved") QVERIFY(info.old);
        }
    }
    void names() {
        QCOMPARE(freeName(folder(), "Palm TX"), QString("Palm TX"));
        QVERIFY(create(folder(), "Palm TX", "palm-tx"));
        QCOMPARE(freeName(folder(), "Palm TX"), QString("Palm TX 2"));
        write(folder() + "/Palm TX 2.frond", header(6, "palm-tx"));
        QCOMPARE(freeName(folder(), "Palm TX"), QString("Palm TX 3"));
        QCOMPARE(cleanName("  a/b  "), QString("a-b"));
        QCOMPARE(cleanName(" .. "), QString());
    }
    void renameAndRemove() {
        write(folder() + "/One.frond", header(6, "palm-tx"));
        QVERIFY(create(folder(), "Two", "palm-e2"));
        QVERIFY(!rename(folder(), describe(folder() + "/One.frond"), "Two").isEmpty());
        QVERIFY(!rename(folder(), describe(folder() + "/One.frond"), "  ").isEmpty());
        QVERIFY(rename(folder(), describe(folder() + "/One.frond"), " Uno ").isEmpty());
        QVERIFY(QFileInfo::exists(folder() + "/Uno.frond") && !QFileInfo::exists(folder() + "/One.frond"));
        QVERIFY(rename(folder(), describe(folder() + "/Two.frond"), "Dos").isEmpty());
        QVERIFY(QFileInfo::exists(folder() + "/Dos.frond.new"));
        QCOMPARE(describe(folder() + "/Dos.frond").device, QString("palm-e2"));
        // A session another Frond has open is left alone.
        QLockFile lock(folder() + "/Uno.frond.lock"); QVERIFY(lock.tryLock());
        QVERIFY(!remove(describe(folder() + "/Uno.frond")).isEmpty());
        QVERIFY(!rename(folder(), describe(folder() + "/Uno.frond"), "Three").isEmpty());
        lock.unlock();
        QVERIFY(remove(describe(folder() + "/Uno.frond")).isEmpty());
        QVERIFY(remove(describe(folder() + "/Dos.frond")).isEmpty());
        QCOMPARE(list(folder()).size(), 0);
    }
    void importing() {
        const auto outside = directory.filePath("elsewhere");
        QDir().mkpath(outside);
        write(outside + "/Trip.frond", header(6, "palm-e2"));
        QString name;
        QVERIFY(importFile(folder(), outside + "/Trip.frond", &name).isEmpty());
        QCOMPARE(name, QString("Trip"));
        QVERIFY(importFile(folder(), outside + "/Trip.frond", &name).isEmpty());
        QCOMPARE(name, QString("Trip 2"));
        QVERIFY(QFileInfo::exists(outside + "/Trip.frond"));
        write(outside + "/text.frond", "session");
        QCOMPARE(importFile(folder(), outside + "/text.frond"), QString("That isn't a Frond session."));
        write(outside + "/old.frond", header(5, "palm-tx"));
        QVERIFY(importFile(folder(), outside + "/old.frond").contains("older Frond"));
        write(outside + "/new.frond", header(7, "palm-tx"));
        QVERIFY(importFile(folder(), outside + "/new.frond").contains("newer Frond"));
        write(outside + "/other.frond", header(6, "palm-zzz"));
        QVERIFY(importFile(folder(), outside + "/other.frond").contains("can't run"));
        QVERIFY(importFile(folder(), outside + "/none.frond").contains("read"));
        QCOMPARE(list(folder()).size(), 2);
        QVERIFY(QDir(folder()).entryList({"*.part"}).isEmpty());
        // Export replaces what is there.
        write(outside + "/out.frond", "old");
        QVERIFY(exportFile(describe(folder() + "/Trip.frond"), outside + "/out.frond").isEmpty());
        QCOMPARE(readHeader(outside + "/out.frond").device, QString("palm-e2"));
    }
    void exportSafety() {
        write(folder() + "/Trip.frond", header(6, "palm-tx"));
        write(folder() + "/Other.frond", header(6, "palm-e2"));
        const auto trip = describe(folder() + "/Trip.frond");
        const auto size = QFileInfo(trip.path).size();
        // Onto itself, also through a link: refused and nothing lost.
        QVERIFY(!exportFile(trip, folder() + "/Trip.frond").isEmpty());
        QVERIFY(QFile::link(trip.path, directory.filePath("link.frond")));
        QVERIFY(!exportFile(trip, directory.filePath("link.frond")).isEmpty());
        QCOMPARE(QFileInfo(trip.path).size(), size);
        // Onto a session that is open elsewhere.
        QLockFile lock(folder() + "/Other.frond.lock"); QVERIFY(lock.tryLock());
        QVERIFY(!exportFile(trip, folder() + "/Other.frond").isEmpty());
        QCOMPARE(readHeader(folder() + "/Other.frond").device, QString("palm-e2"));
        lock.unlock();
        // A copy that fails keeps the old target.
        write(directory.filePath("keep.frond"), "keep");
        QVERIFY(!exportFile(describe(folder() + "/Missing.frond"), directory.filePath("keep.frond")).isEmpty());
        QFile keep(directory.filePath("keep.frond")); QVERIFY(keep.open(QIODevice::ReadOnly));
        QCOMPARE(keep.readAll(), QByteArray("keep"));
        QVERIFY(QDir(directory.path()).entryList({"*.part"}).isEmpty());
        // Onto an idle managed session replaces it.
        QVERIFY(exportFile(trip, folder() + "/Other.frond").isEmpty());
        QCOMPARE(readHeader(folder() + "/Other.frond").device, QString("palm-tx"));
    }
    void reserving() {
        QCOMPARE(reserve(folder(), "Palm TX", "palm-tx"), QString("Palm TX"));
        QCOMPARE(reserve(folder(), "Palm TX", "palm-tx"), QString("Palm TX 2"));
        QVERIFY(!create(folder(), "Palm TX", "palm-tx"));  // never takes a held name
        QCOMPARE(describe(folder() + "/Palm TX.frond").device, QString("palm-tx"));
    }
    void removeKeepsOthersLock() {
        write(folder() + "/Busy.frond", header(6, "palm-tx"));
        QLockFile lock(folder() + "/Busy.frond.lock"); QVERIFY(lock.tryLock());
        QVERIFY(!remove(describe(folder() + "/Busy.frond")).isEmpty());
        QVERIFY(QFileInfo::exists(folder() + "/Busy.frond") && QFileInfo::exists(folder() + "/Busy.frond.lock"));
        lock.unlock();
        QVERIFY(remove(describe(folder() + "/Busy.frond")).isEmpty());
        QVERIFY(!QFileInfo::exists(folder() + "/Busy.frond.lock"));
    }
    void sessionNorDigest() {
        const QByteArray nor(16 << 20, char(0x61));
        auto bytes = header(6, "palm-tx").left(40);
        auto append = [&](quint32 value) { for (int i = 0; i < 4; ++i) bytes.append(char(value >> (8 * i))); };
        for (quint32 value : {0u, 0u, 4u, quint32(nor.size()), 0u, 0u, 0u, 0u}) append(value);
        const auto prefix = bytes;
        bytes += qCompress("meta" + nor + "trailing guest data").mid(4);
        const auto path = filePath(folder(), "Digest"); write(path, bytes);
        QCOMPARE(frondAudio::sessionNorMd5(path), QCryptographicHash::hash(nor, QCryptographicHash::Md5).toHex());
        QVERIFY(!frondAudio::qualified(frondFirmware::Device::tx, path, {}));
        write(path, bytes.left(bytes.size() / 2)); QVERIFY(frondAudio::sessionNorMd5(path).isEmpty());
        bytes = prefix; bytes[50] = char(0x20); write(path, bytes);
        QVERIFY(frondAudio::sessionNorMd5(path).isEmpty());
        bytes = prefix; bytes[52] = 1; write(path, bytes);
        QVERIFY(frondAudio::sessionNorMd5(path).isEmpty());
        write(path, prefix + QByteArray(65536, 'x')); QVERIFY(frondAudio::sessionNorMd5(path).isEmpty());
        const auto raw = directory.filePath("nor.bin"); write(raw, nor);
        QVERIFY(!frondAudio::qualified(frondFirmware::Device::tx, {}, raw));
    }
    void machineControlVisibilityPreferences() {
        const auto one = filePath(folder(), "TG50 One"), two = filePath(folder(), "TG50 Two");
        write(one, header(6, "sony-clie-tg50"));
        write(two, header(6, "sony-clie-tg50"));
        const auto guest = QFileInfo(one).size();
        Preferences loaded;
        QVERIFY(!readPreferences(one, &loaded));
        QVERIFY(loaded.showKeyboard && loaded.showJogControls);
        write(preferencesPath(one), R"({"version":1,"scale":7,"smooth":true,"audio":false,"fastAudio":true,"microphone":true})");
        QVERIFY(readPreferences(one, &loaded));
        QVERIFY(loaded.showKeyboard && loaded.showJogControls && loaded.microphone);
        QVERIFY(writePreferences(two, loaded).isEmpty());
        for (bool keyboard : {false, true}) for (bool jog : {false, true}) {
            loaded.showKeyboard = keyboard; loaded.showJogControls = jog;
            QVERIFY(writePreferences(one, loaded).isEmpty());
            Preferences roundTrip;
            QVERIFY(readPreferences(one, &roundTrip));
            QCOMPARE(roundTrip.showKeyboard, keyboard); QCOMPARE(roundTrip.showJogControls, jog);
            QCOMPARE(roundTrip.scale, 7);
            QVERIFY(roundTrip.smooth && !roundTrip.audio && roundTrip.fastAudio && roundTrip.microphone);
            QVERIFY(readPreferences(two, &roundTrip));
            QVERIFY(roundTrip.showKeyboard && roundTrip.showJogControls);
            QCOMPARE(QFileInfo(one).size(), guest);
        }
    }
    void machinePreferences() {
        const auto one = filePath(folder(), "One"), two = filePath(folder(), "Two");
        write(one, header(6, "palm-tx"));
        QVERIFY(create(folder(), "Two", "palm-tx"));
        Preferences prefs{7, true, false, true}, loaded;
        QVERIFY(writePreferences(one, prefs).isEmpty());
        QVERIFY(writePreferences(two, Preferences{}).isEmpty());
        QVERIFY(readPreferences(one, &loaded)); QCOMPARE(loaded.scale, 7);
        QVERIFY(loaded.smooth && !loaded.audio && loaded.fastAudio);
        QVERIFY(rename(folder(), describe(one), "Moved").isEmpty());
        const auto moved = filePath(folder(), "Moved");
        QVERIFY(!QFileInfo::exists(preferencesPath(one)));
        QVERIFY(readPreferences(moved, &loaded)); QCOMPARE(loaded.scale, 7);
        QVERIFY(readPreferences(two, &loaded)); QCOMPARE(loaded.scale, 4); QVERIFY(loaded.audio);
        QLockFile lock(moved + ".lock"); QVERIFY(lock.tryLock());
        QVERIFY(!remove(describe(moved)).isEmpty());
        QVERIFY(QFileInfo::exists(preferencesPath(moved))); lock.unlock();
        const auto copy = directory.filePath("copy.frond");
        QVERIFY(exportFile(describe(moved), copy).isEmpty());
        QVERIFY(readPreferences(copy, &loaded)); QCOMPARE(loaded.scale, 7);
        QString imported;
        QVERIFY(importFile(folder(), copy, &imported).isEmpty());
        QVERIFY(readPreferences(filePath(folder(), imported), &loaded));
        QCOMPARE(loaded.scale, 7); QVERIFY(!loaded.audio);
        QVERIFY(remove(describe(moved)).isEmpty());
        QVERIFY(!QFileInfo::exists(preferencesPath(moved)));
        QVERIFY(rename(folder(), describe(two), "Pending").isEmpty());
        QVERIFY(readPreferences(filePath(folder(), "Pending"), &loaded));
        QVERIFY(remove(describe(filePath(folder(), "Pending"))).isEmpty());
        QVERIFY(!QFileInfo::exists(preferencesPath(filePath(folder(), "Pending"))));
        write(preferencesPath(filePath(folder(), imported)), "{}");
        QVERIFY(!readPreferences(filePath(folder(), imported), &loaded));
    }
    void firmwareCopy() {
        using frondFirmware::Device;
        const auto root = directory.filePath("firmware");
        const auto nor = directory.filePath("e2-nor"), nand = directory.filePath("e2-nand"), bad = directory.filePath("bad");
        {
            QFile f(nor); QVERIFY(f.open(QIODevice::WriteOnly)); QVERIFY(f.resize(8 << 20));
            f.seek(0x20008); f.write("\xef\xbe\xed\xfe", 4);
            f.seek(0x20080); f.write("mlaP", 4); f.write("rpsh", 4);
            QFile n(nand); QVERIFY(n.open(QIODevice::WriteOnly)); QVERIFY(n.resize(34603008));
            QFile b(bad); QVERIFY(b.open(QIODevice::WriteOnly)); QVERIFY(b.resize(1000));
        }
        QVERIFY(!frondFirmware::ready(root, Device::e2));
        Device device = Device::automatic;
        QVERIFY(!frondFirmware::importImages(root, Device::automatic, bad, {}, &device).isEmpty());
        QVERIFY(!frondFirmware::importImages(root, Device::tx, nor, {}).isEmpty());
        QVERIFY(!frondFirmware::importImages(root, Device::tx, bad, {}).isEmpty());
        QVERIFY(!QFileInfo::exists(root + "/palm-tx") && !frondFirmware::ready(root, Device::tx));
        // Detect picks the device; the images land in <root>/<device name>/.
        QVERIFY(frondFirmware::importImages(root, Device::automatic, nor, nand, &device).isEmpty());
        QCOMPARE(device, Device::e2);
        QCOMPARE(QFileInfo(root + "/palm-e2/nor.bin").size(), qint64(8 << 20));
        QCOMPARE(QFileInfo(root + "/palm-e2/nand.bin").size(), qint64(34603008));
        QVERIFY(frondFirmware::ready(root, Device::e2));
        QCOMPARE(frondFirmware::storedNand(root, Device::e2), root + "/palm-e2/nand.bin");
        // A failed import keeps what was stored.
        QVERIFY(!frondFirmware::importImages(root, Device::e2, nor, bad).isEmpty());
        QVERIFY(frondFirmware::ready(root, Device::e2));
        // An import without NAND drops the old NAND (optional on the E2).
        QVERIFY(frondFirmware::importImages(root, Device::e2, nor, {}).isEmpty());
        QVERIFY(frondFirmware::storedNand(root, Device::e2).isEmpty());
        QVERIFY(frondFirmware::ready(root, Device::e2));
        QVERIFY(QDir(root + "/palm-e2").entryList({"*.part"}).isEmpty());
        QVERIFY(!frondFirmware::ready(root, Device::tx));
    }
};
QTEST_APPLESS_MAIN(SessionStoreTest)
#include "session-store.moc"
