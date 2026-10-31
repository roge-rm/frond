#include <QtTest>
#include "FirmwareStore.h"
#include "SessionStore.h"

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
