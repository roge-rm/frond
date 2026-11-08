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
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(data);
}
// Backdates a file or a folder and everything in it by an hour.
static void age(const QString& path) {
    namespace fs = std::filesystem;
    const auto when = fs::file_time_type::clock::now() - std::chrono::hours(1);
    if (fs::is_directory(path.toStdString()))
        for (const auto& entry : fs::recursive_directory_iterator(path.toStdString())) fs::last_write_time(entry.path(), when);
    fs::last_write_time(path.toStdString(), when);
}
static void put32(QByteArray& bytes, quint32 n) {
    for (int i = 0; i < 4; i++) bytes.append(char(n >> (8 * i)));
}
static void put64(QByteArray& bytes, quint64 n) {
    put32(bytes, quint32(n));
    put32(bytes, quint32(n >> 32));
}
static QByteArray v7WithSd() {
    QByteArray bytes;
    for (quint32 value : {frondSessions::magic, 7u}) put32(bytes, value);
    bytes.append(QByteArray("palm-tx").leftJustified(32, '\0', true));
    put32(bytes, 0);
    put32(bytes, 0);
    put64(bytes, 0);
    put64(bytes, 0);
    put64(bytes, 0);
    put64(bytes, 0);
    put64(bytes, 0);
    put32(bytes, 1);
    put32(bytes, 1);
    bytes.append(QByteArray("sd.frondsd").leftJustified(32, '\0', true));
    put32(bytes, 1);
    put32(bytes, 0);
    bytes.append(QByteArray(32, '\0'));
    put64(bytes, 512);
    put64(bytes, 512);
    return bytes;
}
// v8: the v7 layout with 100-byte descriptors that end with the SD content generation.
static QByteArray v8WithSd(quint64 generation) {
    auto bytes = v7WithSd();
    bytes[4] = 8;
    put64(bytes, generation);
    return bytes;
}
// .frondsd v2 of 512 bytes: header CRC, generation, table offset, 1 MiB chunks, one table entry.
static QByteArray tinyFrondsdV2(quint64 generation) {
    QByteArray file("FRONDSD");
    file.append('\0');
    put32(file, 2);
    put32(file, 512);
    put64(file, 512);
    put32(file, 0);
    put64(file, generation);
    put64(file, 64 + 512);
    put32(file, 1u << 20);
    file.append(QByteArray(16, '\0'));
    file.append(QByteArray(512, '\xa5'));
    put32(file, 0);
    return file;
}
static QByteArray tinyFrondsd() {
    QByteArray file("FRONDSD");
    file.append('\0');
    put32(file, 1);
    put32(file, 512);
    put64(file, 512);
    put32(file, 0);
    file.append(QByteArray(36, '\0'));
    file.append(QByteArray(512, '\xa5'));
    return file;
}

// .frondsd v2 with real chunk and header CRCs.
static QByteArray realFrondsdV2(quint64 generation, const QByteArray& payload) {
    QByteArray table;
    put32(table, quint32(crc32(0, reinterpret_cast<const Bytef*>(payload.constData()), uInt(payload.size()))));
    QByteArray header("FRONDSD");
    header.append('\0');
    put32(header, 2);
    put32(header, 512);
    put64(header, quint64(payload.size()));
    put32(header, 0);
    put64(header, generation);
    put64(header, 64 + quint64(payload.size()));
    put32(header, 1u << 20);
    header.append(QByteArray(16, '\0'));
    uLong crc = crc32(0, reinterpret_cast<const Bytef*>(header.constData()), 64);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(table.constData()), uInt(table.size()));
    QByteArray crcBytes;
    put32(crcBytes, quint32(crc));
    header.replace(24, 4, crcBytes);
    return header + payload + table;
}
// A journal from `from` to from + 1 over a 512-byte card: one record, then the new table.
static QByteArray journalTo(quint64 from, quint64 offset, const QByteArray& data, const QByteArray& newPayload) {
    QByteArray j("FRONDJNL");
    put32(j, 1);
    put32(j, 0);
    put64(j, 512);
    put64(j, from);
    put64(j, from + 1);
    put64(j, 1);
    put64(j, 1);
    j.append(QByteArray(8, '\0'));
    put64(j, offset);
    put32(j, quint32(data.size()));
    j.append(data);
    put32(j, quint32(crc32(0, reinterpret_cast<const Bytef*>(newPayload.constData()), uInt(newPayload.size()))));
    const auto crc = quint32(crc32(0, reinterpret_cast<const Bytef*>(j.constData()), uInt(j.size())));
    j.append("FRONDEND");
    put32(j, crc);
    put32(j, 0);
    return j;
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
        QVERIFY(QFileInfo::exists(filePath(folder(), "Tx")) && !QFileInfo::exists(folder() + "/Tx.frond"));
        // Saving replaces the marker; the session is listed once.
        write(filePath(folder(), "Fresh"), header(6, "palm-e2"));
        QCOMPARE(list(folder()).size(), 3);
        // Available sessions open; a reserved device stays unavailable.
        write(folder() + "/Zire.frond", header(6, "palm-zire-72"));
        write(folder() + "/Clie.frond", header(6, "sony-clie-tg50"));
        write(folder() + "/E3.frond", header(6, "palm-e2-franken"));
        write(folder() + "/IIIx.frond", header(8, "palm-iiix"));
        write(folder() + "/Reserved.frond", header(6, "palm-m515"));
        for (const auto& info : list(folder())) {
            if (info.name == "Zire") QVERIFY(info.saved && !info.old && info.device == "palm-zire-72");
            if (info.name == "E3") QVERIFY(info.saved && !info.old && info.device == "palm-e2-franken");
            if (info.name == "Clie") QVERIFY(info.saved && !info.old && info.device == "sony-clie-tg50");
            if (info.name == "IIIx") QVERIFY(info.saved && !info.old && info.device == "palm-iiix");
            if (info.name == "Reserved") QVERIFY(info.old);
        }
    }
    void names() {
        QCOMPARE(freeName(folder(), "Palm TX"), QString("Palm TX"));
        QVERIFY(create(folder(), "Palm TX", "palm-tx"));
        QCOMPARE(freeName(folder(), "Palm TX"), QString("Palm TX 2"));
        write(filePath(folder(), "Palm TX 2"), header(6, "palm-tx"));
        QCOMPARE(freeName(folder(), "Palm TX"), QString("Palm TX 3"));
        QCOMPARE(cleanName("  a/b  "), QString("a-b"));
        QCOMPARE(cleanName(" .. "), QString());
    }
    void duplicating() {
        QVERIFY(create(folder(), "Zire 72", "palm-zire72"));
        QCOMPARE(baseNameOf(folder(), "Zire 72"), QString("Zire 72"));
        QCOMPARE(copyName(folder(), "Zire 72"), QString("Zire 72 2"));
        QCOMPARE(baseNameOf(folder(), "7"), QString("7"));
        QCOMPARE(baseNameOf(folder(), "Palm TX -2"), QString("Palm TX -2"));
        QCOMPARE(baseNameOf(folder(), "Palm TX 2"), QString("Palm TX 2"));  // no "Palm TX" yet
        const auto dir = machineDir(folder(), "Palm TX");
        write(dir + "/Palm TX.frond", header(8, "palm-tx"));
        write(dir + "/Palm TX.frond.settings.json", "{\"version\":1,\"scale\":6,\"smooth\":true,\"audio\":false,\"fastAudio\":false}");
        write(dir + "/sd.frondsd", QByteArray(3000000, 'x'));
        age(dir);
        QCOMPARE(baseNameOf(folder(), "Palm TX 2"), QString("Palm TX"));
        QCOMPARE(baseNameOf(folder(), "Palm TX 12"), QString("Palm TX"));
        QString name;
        const auto first = duplicate(folder(), describe(filePath(folder(), "Palm TX")), &name); QVERIFY2(first.isEmpty(), qPrintable(first));
        QCOMPARE(name, QString("Palm TX 2"));
        const auto copy = machineDir(folder(), "Palm TX 2");
        QCOMPARE(QFileInfo(copy + "/sd.frondsd").size(), qint64(3000000));
        QVERIFY(QFileInfo::exists(copy + "/Palm TX 2.frond"));
        QVERIFY(!QFileInfo::exists(copy + "/Palm TX.frond"));
        Preferences prefs;
        QVERIFY(readPreferences(filePath(folder(), "Palm TX 2"), &prefs));
        QCOMPARE(prefs.scale, 6);
        QVERIFY(prefs.smooth && !prefs.audio);
        // Independent files.
        write(dir + "/sd.frondsd", QByteArray(10, 'y'));
        QCOMPARE(QFileInfo(copy + "/sd.frondsd").size(), qint64(3000000));
        // Duplicating the copy counts on from the base name.
        QVERIFY(duplicate(folder(), describe(filePath(folder(), "Palm TX 2")), &name).isEmpty());
        QCOMPARE(name, QString("Palm TX 3"));
        QVERIFY(duplicate(folder(), describe(filePath(folder(), "Palm TX")), &name).isEmpty());
        QCOMPARE(name, QString("Palm TX 4"));
        // A held lock and a cancel leave no partial copy. A full disk takes the same path as a cancel, and
        // the free space check fails the copy before any file is written.
        QLockFile held(filePath(folder(), "Palm TX") + ".lock");
        held.setStaleLockTime(0);
        QVERIFY(held.tryLock());
        QVERIFY(!duplicate(folder(), describe(filePath(folder(), "Palm TX")), &name).isEmpty());
        held.unlock();
        QVERIFY(!taken(folder(), "Palm TX 5"));
        QVERIFY(duplicate(folder(), describe(filePath(folder(), "Palm TX")), &name, [](qint64, qint64) { return false; }) == "Duplicate cancelled.");
        QVERIFY(!taken(folder(), "Palm TX 5"));
        QVERIFY(QDir(folder()).entryList({".dup-*"}, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        // A hidden folder left by a crash goes once it is old and unlocked, and never shows in the list.
        const auto stale = folder() + "/.dup-1-2";
        write(stale + "/Palm TX 9.frond", header(8, "palm-tx"));
        QCOMPARE(list(folder()).size(), 5);
        QVERIFY(QFileInfo::exists(stale));  // too new
        age(stale);
        {
            QLockFile busy(stale + ".lock");
            busy.setStaleLockTime(0);
            QVERIFY(busy.tryLock());
            list(folder());
            QVERIFY(QFileInfo::exists(stale));  // locked
        }
        list(folder());
        QVERIFY(!QFileInfo::exists(stale));
        // A finished first save with its full card copy but no sd.frondsd still gets the card.
        write(machineDir(folder(), "Cardless") + "/Cardless.frond", v8WithSd(1));
        write(machineDir(folder(), "Cardless") + "/sd.frondsd.journal-1.full", realFrondsdV2(1, QByteArray(512, 'a')));
        QVERIFY2(duplicate(folder(), describe(filePath(folder(), "Cardless")), &name).isEmpty(), "cardless");
        QVERIFY(QFileInfo::exists(machineDir(folder(), name) + "/sd.frondsd"));
        // A machine that has not been saved copies its marker and settings.
        QVERIFY(create(folder(), "Fresh", "palm-tx"));
        QVERIFY(writePreferences(filePath(folder(), "Fresh"), Preferences{5, false, true, false}).isEmpty());
        QVERIFY(duplicate(folder(), describe(filePath(folder(), "Fresh")), &name).isEmpty());
        QCOMPARE(name, QString("Fresh 2"));
        QVERIFY(readPreferences(filePath(folder(), "Fresh 2"), &prefs));
        QCOMPARE(prefs.scale, 5);
        QCOMPARE(describe(filePath(folder(), "Fresh 2")).device, QString("palm-tx"));
    }
    void renameAndRemove() {
        write(filePath(folder(), "One"), header(6, "palm-tx"));
        QVERIFY(create(folder(), "Two", "palm-e2"));
        QVERIFY(!rename(folder(), describe(filePath(folder(), "One")), "Two").isEmpty());
        QVERIFY(!rename(folder(), describe(filePath(folder(), "One")), "  ").isEmpty());
        QVERIFY(rename(folder(), describe(filePath(folder(), "One")), " Uno ").isEmpty());
        QVERIFY(QFileInfo::exists(filePath(folder(), "Uno")) && !QFileInfo::exists(filePath(folder(), "One")));
        QVERIFY(rename(folder(), describe(filePath(folder(), "Two")), "Dos").isEmpty());
        QVERIFY(QFileInfo::exists(markerPath(filePath(folder(), "Dos"))));
        QCOMPARE(describe(filePath(folder(), "Dos")).device, QString("palm-e2"));
        // A session another Frond has open is left alone.
        QLockFile lock(filePath(folder(), "Uno") + ".lock"); QVERIFY(lock.tryLock());
        QVERIFY(!remove(describe(filePath(folder(), "Uno"))).isEmpty());
        QVERIFY(!rename(folder(), describe(filePath(folder(), "Uno")), "Three").isEmpty());
        lock.unlock();
        QVERIFY(remove(describe(filePath(folder(), "Uno"))).isEmpty());
        QVERIFY(remove(describe(filePath(folder(), "Dos"))).isEmpty());
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
        write(outside + "/new.frond", header(9, "palm-tx"));
        QVERIFY(importFile(folder(), outside + "/new.frond").contains("newer Frond"));
        write(outside + "/v7.frond", header(7, "palm-tx"));
        QVERIFY(importFile(folder(), outside + "/v7.frond").isEmpty());
        write(outside + "/v8/v8.frond", v8WithSd(3));
        write(outside + "/v8/sd.frondsd", tinyFrondsdV2(3));
        write(outside + "/v8/sd.frondsd.journal-4", "journal");
        QVERIFY(importFile(folder(), outside + "/v8", &name).isEmpty());
        QCOMPARE(name, QString("v8"));
        QVERIFY(!QFileInfo::exists(machineDir(folder(), "v8") + "/sd.frondsd.journal-4"));
        write(outside + "/v8/sd.frondsd", tinyFrondsdV2(2));
        QVERIFY(!importFile(folder(), outside + "/v8").isEmpty());
        write(outside + "/v8/sd.frondsd", tinyFrondsd());
        QVERIFY(!importFile(folder(), outside + "/v8").isEmpty());
        write(outside + "/missing.frond", v7WithSd());
        QVERIFY(!importFile(folder(), outside + "/missing.frond").isEmpty());
        QVERIFY(!QFileInfo::exists(filePath(folder(), "missing")));
        write(outside + "/other.frond", header(6, "palm-zzz"));
        QVERIFY(importFile(folder(), outside + "/other.frond").contains("can't run"));
        QVERIFY(importFile(folder(), outside + "/none.frond").contains("read"));
        QCOMPARE(list(folder()).size(), 4);
        QVERIFY(QDir(folder()).entryList({"*.part"}).isEmpty());
        // Export copies the folder; a .frond path names the destination folder by its stem.
        write(outside + "/out.frond", "old");
        QVERIFY(exportFile(describe(filePath(folder(), "Trip")), outside + "/out.frond").isEmpty());
        QCOMPARE(readHeader(outside + "/out/out.frond").device, QString("palm-e2"));
    }
    // A save the core didn't finish: the copy gets sd.frondsd settled to the .frond's generation,
    // from the journal or the full copy it needs, and no journals.
    void copiesSettleUnfinishedSave() {
        const auto outside = directory.filePath("unfinished");
        const QByteArray oldPayload(512, '\xa5');
        auto newPayload = oldPayload;
        newPayload.replace(8, 4, "abcd");
        write(outside + "/j/j.frond", v8WithSd(4));
        write(outside + "/j/sd.frondsd", realFrondsdV2(3, oldPayload));
        write(outside + "/j/sd.frondsd.journal-4", journalTo(3, 8, "abcd", newPayload));
        write(outside + "/j/sd.frondsd.journal-9", "stale");
        QString name;
        QVERIFY(importFile(folder(), outside + "/j", &name).isEmpty());
        const auto dest = machineDir(folder(), name);
        QFile got(dest + "/sd.frondsd");
        QVERIFY(got.open(QIODevice::ReadOnly));
        QCOMPARE(got.readAll(), realFrondsdV2(4, newPayload));
        QVERIFY(QDir(dest).entryList({"*journal*"}, QDir::Files | QDir::Hidden).isEmpty());
        QVERIFY(exportFile(describe(filePath(folder(), name)), outside + "/out").isEmpty());
        QFile exported(outside + "/out/sd.frondsd");
        QVERIFY(exported.open(QIODevice::ReadOnly));
        QCOMPARE(exported.readAll(), realFrondsdV2(4, newPayload));
        // A damaged journal makes no copy.
        auto damaged = journalTo(3, 8, "abcd", newPayload);
        damaged[80] = 'x';
        write(outside + "/j/sd.frondsd.journal-4", damaged);
        QVERIFY(!importFile(folder(), outside + "/j").isEmpty());
        // A first save's full copy, with no sd.frondsd yet.
        write(outside + "/f/f.frond", v8WithSd(1));
        write(outside + "/f/sd.frondsd.journal-1.full", realFrondsdV2(1, newPayload));
        QVERIFY(importFile(folder(), outside + "/f", &name).isEmpty());
        QFile full(machineDir(folder(), name) + "/sd.frondsd");
        QVERIFY(full.open(QIODevice::ReadOnly));
        QCOMPARE(full.readAll(), realFrondsdV2(1, newPayload));
        QVERIFY(QDir(machineDir(folder(), name)).entryList({"*journal*"}, QDir::Files | QDir::Hidden).isEmpty());
        // A loose X/X.frond picked as a file settles the same way, journal or full copy.
        write(outside + "/j/sd.frondsd.journal-4", journalTo(3, 8, "abcd", newPayload));
        QVERIFY(importFile(folder(), outside + "/j/j.frond", &name).isEmpty());
        QFile loose(machineDir(folder(), name) + "/sd.frondsd");
        QVERIFY(loose.open(QIODevice::ReadOnly));
        QCOMPARE(loose.readAll(), realFrondsdV2(4, newPayload));
        QVERIFY(importFile(folder(), outside + "/f/f.frond", &name).isEmpty());
        QFile looseFull(machineDir(folder(), name) + "/sd.frondsd");
        QVERIFY(looseFull.open(QIODevice::ReadOnly));
        QCOMPARE(looseFull.readAll(), realFrondsdV2(1, newPayload));
        write(outside + "/j/sd.frondsd.journal-4", damaged);
        QVERIFY(!importFile(folder(), outside + "/j/j.frond").isEmpty());
    }
    void exportSafety() {
        write(filePath(folder(), "Trip"), header(6, "palm-tx"));
        write(filePath(folder(), "Other"), header(6, "palm-e2"));
        const auto trip = describe(filePath(folder(), "Trip"));
        const auto size = QFileInfo(trip.path).size();
        // Onto itself, also through a link: refused and nothing lost.
        QVERIFY(!exportFile(trip, filePath(folder(), "Trip")).isEmpty());
        QVERIFY(QFile::link(trip.path, directory.filePath("link.frond")));
        QVERIFY(!exportFile(trip, directory.filePath("link.frond")).isEmpty());
        QCOMPARE(QFileInfo(trip.path).size(), size);
        // Onto a session that is open elsewhere.
        QLockFile lock(filePath(folder(), "Other") + ".lock"); QVERIFY(lock.tryLock());
        QVERIFY(!exportFile(trip, filePath(folder(), "Other")).isEmpty());
        QCOMPARE(readHeader(filePath(folder(), "Other")).device, QString("palm-e2"));
        lock.unlock();
        // A copy that fails keeps the old target.
        write(directory.filePath("keep.frond"), "keep");
        QVERIFY(!exportFile(describe(filePath(folder(), "Missing")), directory.filePath("keep.frond")).isEmpty());
        QFile keep(directory.filePath("keep.frond")); QVERIFY(keep.open(QIODevice::ReadOnly));
        QCOMPARE(keep.readAll(), QByteArray("keep"));
        QVERIFY(QDir(directory.path()).entryList({"*.part"}).isEmpty());
        // Onto an idle managed session replaces it.
        QVERIFY(exportFile(trip, filePath(folder(), "Other")).isEmpty());
        QCOMPARE(readHeader(filePath(folder(), "Other")).device, QString("palm-tx"));
    }
    // Export and Save a Copy replace only a Frond session, never any other folder.
    void exportKeepsOtherFolders() {
        write(filePath(folder(), "Trip"), header(6, "palm-tx"));
        const auto trip = describe(filePath(folder(), "Trip"));
        const auto photos = directory.filePath("Photos");
        write(photos + "/a.jpg", "jpeg");
        bool existing = true;
        QVERIFY(!exportTargetError(photos, &existing).isEmpty());
        QVERIFY(!existing);
        QVERIFY(!exportFile(trip, directory.filePath("Photos.frond")).isEmpty());
        QVERIFY(QFileInfo::exists(photos + "/a.jpg"));
        // A folder that only looks like one: a .frond that isn't a session.
        const auto fake = directory.filePath("Fake");
        write(fake + "/Fake.frond", "text");
        QVERIFY(!exportFile(trip, fake).isEmpty());
        QVERIFY(QFileInfo::exists(fake + "/Fake.frond"));
        for (const auto& name : QDir(directory.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot))
            QVERIFY2(!name.contains(".frond.tmp-") && !name.endsWith(".old") && !name.endsWith(".part"), qPrintable(name));
        // A real session is replaced whole, without its old card.
        const auto old = directory.filePath("Old");
        write(old + "/Old.frond", v7WithSd());
        write(old + "/sd.frondsd", tinyFrondsd());
        QVERIFY(exportTargetError(old, &existing).isEmpty());
        QVERIFY(existing);
        QVERIFY(exportFile(trip, old).isEmpty());
        QCOMPARE(readHeader(old + "/Old.frond").version, quint32(6));
        QVERIFY(!QFileInfo::exists(old + "/sd.frondsd"));
        // Copying into an existing folder drops a card the source doesn't have.
        write(old + "/sd.frondsd", tinyFrondsd());
        QVERIFY(copyPackable(machineDirOf(trip.path), old, "Old").isEmpty());
        QVERIFY(!QFileInfo::exists(old + "/sd.frondsd"));
        // Save a Copy stages its folder and publishes it the same way.
        write(old + "/sd.frondsd", tinyFrondsd());
        const auto staging = makeStaging(old);
        QVERIFY(!staging.isEmpty());
        write(staging + "/Old/Old.frond", header(7, "palm-e2"));
        QVERIFY(publishStaged(staging, old).isEmpty());
        QCOMPARE(readHeader(old + "/Old.frond").device, QString("palm-e2"));
        QVERIFY(!QFileInfo::exists(old + "/sd.frondsd"));
        QVERIFY(!QFileInfo::exists(staging));
        const auto refused = makeStaging(photos);
        write(refused + "/Photos/Photos.frond", header(7, "palm-e2"));
        QVERIFY(!publishStaged(refused, photos).isEmpty());
        QVERIFY(QFileInfo::exists(photos + "/a.jpg") && !QFileInfo::exists(photos + "/Photos.frond"));
        removeStaging(refused);
        QVERIFY(!QFileInfo::exists(refused));
    }
    // A folder holding other sessions isn't one session, so it is never replaced.
    void exportRefusesSessionsFolder() {
        write(filePath(folder(), "Trip"), header(6, "palm-tx"));
        const auto trip = describe(filePath(folder(), "Trip"));
        const auto palm = directory.filePath("Palm");
        write(palm + "/TX.frond", header(7, "palm-tx"));
        write(palm + "/T3.frond", header(7, "palm-t3"));
        QVERIFY(!exportTargetError(palm).isEmpty());
        QVERIFY(!exportFile(trip, directory.filePath("Palm.frond")).isEmpty());
        QVERIFY(!exportFile(trip, palm + "/Palm.frond").isEmpty());
        const auto staging = makeStaging(palm);
        write(staging + "/Palm/Palm.frond", header(7, "palm-e2"));
        QVERIFY(!publishStaged(staging, palm).isEmpty());
        removeStaging(staging);
        QCOMPARE(QDir(palm).entryList(QDir::Files | QDir::Hidden), QStringList({"T3.frond", "TX.frond"}));
        // One session plus another session's file isn't one either.
        write(palm + "/Palm.frond", header(7, "palm-tx"));
        QVERIFY(!exportTargetError(palm).isEmpty());
        QVERIFY(QFileInfo::exists(palm + "/TX.frond"));
    }
    // An empty folder becomes the session folder, and a session can be called "old".
    void exportIntoEmptyFolder() {
        write(filePath(folder(), "Trip"), header(6, "palm-tx"));
        const auto trip = describe(filePath(folder(), "Trip"));
        const auto foo = directory.filePath("Foo");
        QVERIFY(QDir().mkpath(foo));
        QVERIFY(exportTargetError(foo).isEmpty());
        QVERIFY(exportFile(trip, foo + "/Foo.frond").isEmpty());
        QCOMPARE(readHeader(foo + "/Foo.frond").device, QString("palm-tx"));
        const auto old = directory.filePath("old");
        write(old + "/old.frond", header(7, "palm-e2"));
        QVERIFY(exportFile(trip, old).isEmpty());
        QCOMPARE(readHeader(old + "/old.frond").device, QString("palm-tx"));
    }
    // A replaced session waiting in a staging folder is never deleted with it, and one a stopped
    // Frond left behind goes back in place.
    void stagingKeepsOriginal() {
        const auto dest = directory.filePath("Keep");
        auto staging = makeStaging(dest);
        write(stagedOriginal(staging, dest) + "/Keep.frond", header(7, "palm-tx"));
        write(staging + "/Keep/Keep.frond", header(7, "palm-e2"));
        removeStaging(staging);
        QVERIFY(QFileInfo::exists(stagedOriginal(staging, dest) + "/Keep.frond"));
        QVERIFY(!QFileInfo::exists(staging + "/Keep"));
        const auto fresh = makeStaging(dest);  // too new to touch
        QVERIFY(QFileInfo::exists(stagedOriginal(staging, dest) + "/Keep.frond"));
        removeStaging(fresh);
        age(staging);
        removeStaging(makeStaging(directory.filePath("Other")));  // another name's staging leaves it alone
        QVERIFY(QFileInfo::exists(stagedOriginal(staging, dest) + "/Keep.frond"));
        const auto next = makeStaging(dest);
        QCOMPARE(readHeader(dest + "/Keep.frond").device, QString("palm-tx"));
        QVERIFY(!QFileInfo::exists(staging));
        removeStaging(next);
        // A plain leftover copy goes too.
        const auto left = makeStaging(dest);
        write(left + "/Keep/Keep.frond", header(7, "palm-e2"));
        age(left);
        removeStaging(makeStaging(dest));
        QVERIFY(!QFileInfo::exists(left));
        QCOMPARE(readHeader(dest + "/Keep.frond").device, QString("palm-tx"));
    }
    // An outside session's work folder left by a stopped Frond goes once it is old and free.
    void staleWorkRemoved() {
        const auto make = [&](const QString& name) {
            QVERIFY(create(folder(), name, "palm-tx"));
            write(workPath(filePath(folder(), name)), {});
            write(machineDir(folder(), name) + "/sd.frondsd", tinyFrondsd());
        };
        make("Crashed"); make("Running"); make("Fresh"); make("Saved"); make("Played"); make("Before");
        write(filePath(folder(), "Saved"), header(7, "palm-tx"));
        for (const auto& name : {"Crashed", "Running", "Saved", "Played", "Before"}) age(machineDir(folder(), name));
        // The guest wrote to Played's card after the core started; Before is renamed first.
        std::filesystem::last_write_time((machineDir(folder(), "Played") + "/sd.frondsd").toStdString(),
                                         std::filesystem::file_time_type::clock::now() - std::chrono::minutes(30));
        QVERIFY(rename(folder(), describe(filePath(folder(), "Before")), "After").isEmpty());
        QVERIFY(QFileInfo::exists(workPath(filePath(folder(), "After"))));
        QVERIFY(!QFileInfo::exists(machineDir(folder(), "After") + "/Before.frond.work"));
        age(machineDir(folder(), "After"));
        {
            QLockFile lock(filePath(folder(), "Running") + ".lock"); QVERIFY(lock.tryLock());
            QStringList names;
            for (const auto& info : list(folder())) names << info.name;
            QVERIFY(!names.contains("Crashed") && !names.contains("After"));
            QVERIFY(names.contains("Running") && names.contains("Fresh") && names.contains("Saved") && names.contains("Played"));
        }
        QVERIFY(!QFileInfo::exists(machineDir(folder(), "Crashed")));
        QVERIFY(!QFileInfo::exists(machineDir(folder(), "After")));
        QVERIFY(QFileInfo::exists(machineDir(folder(), "Played") + "/sd.frondsd"));
        QVERIFY(QFileInfo::exists(machineDir(folder(), "Running") + "/sd.frondsd"));
        QVERIFY(QFileInfo::exists(machineDir(folder(), "Fresh") + "/sd.frondsd"));
        QVERIFY(!QFileInfo::exists(workPath(filePath(folder(), "Saved"))));
        QVERIFY(QFileInfo::exists(machineDir(folder(), "Saved") + "/sd.frondsd"));
    }
    // Rename holds the lock through the move and leaves the session free afterwards.
    void renameKeepsLock() {
        write(filePath(folder(), "Trip"), header(6, "palm-tx"));
        QVERIFY(rename(folder(), describe(filePath(folder(), "Trip")), "Moved").isEmpty());
        QVERIFY(!inUse(filePath(folder(), "Moved")));
        QVERIFY(!QFileInfo::exists(filePath(folder(), "Moved") + ".lock"));
        QVERIFY(!QFileInfo::exists(machineDir(folder(), "Moved") + "/Trip.frond.lock"));
    }
    // Card and session temps left by a killed save go once they are old, unless the session is open.
    void staleTempsRemoved() {
        write(filePath(folder(), "Trip"), header(6, "palm-tx"));
        const auto dir = machineDir(folder(), "Trip");
        const auto old = QDateTime::currentDateTime().addSecs(-3600);
        for (const auto& name : {".frondsd.tmp-a", ".frondms.tmp-b", ".frond.tmp-c", ".gen-sd.frondsd-tmp-1-2"}) {
            write(dir + '/' + name, "tmp");
            QFile file(dir + '/' + name);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(old, QFileDevice::FileModificationTime));
        }
        write(dir + "/.frondsd.tmp-fresh", "tmp");
        write(dir + "/.gen-sd.frondsd-root", "root");
        {
            QLockFile lock(filePath(folder(), "Trip") + ".lock"); QVERIFY(lock.tryLock());
            list(folder());
            QVERIFY(QFileInfo::exists(dir + "/.frondsd.tmp-a"));
        }
        list(folder());
        for (const auto& name : {".frondsd.tmp-a", ".frondms.tmp-b", ".frond.tmp-c", ".gen-sd.frondsd-tmp-1-2"})
            QVERIFY2(!QFileInfo::exists(dir + '/' + name), name);
        QVERIFY(QFileInfo::exists(dir + "/.frondsd.tmp-fresh"));
        QVERIFY(QFileInfo::exists(dir + "/.gen-sd.frondsd-root"));
    }
    void reserving() {
        QCOMPARE(reserve(folder(), "Palm TX", "palm-tx"), QString("Palm TX"));
        QCOMPARE(reserve(folder(), "Palm TX", "palm-tx"), QString("Palm TX 2"));
        QVERIFY(!create(folder(), "Palm TX", "palm-tx"));  // never takes a held name
        QCOMPARE(describe(filePath(folder(), "Palm TX")).device, QString("palm-tx"));
    }
    void migrateLooseV6() {
        write(folder() + "/Trip.frond", header(6, "palm-tx"));
        write(folder() + "/Trip.frond.settings.json",
              R"({"version":1,"scale":5,"smooth":false,"audio":true,"fastAudio":false})");
        QCOMPARE(list(folder()).size(), 1);
        QVERIFY(QFileInfo::exists(filePath(folder(), "Trip")));
        QVERIFY(!QFileInfo::exists(folder() + "/Trip.frond"));
        QVERIFY(QFileInfo::exists(preferencesPath(filePath(folder(), "Trip"))));
        Preferences loaded;
        QVERIFY(readPreferences(filePath(folder(), "Trip"), &loaded));
        QCOMPARE(loaded.scale, 5);
        QCOMPARE(locate(folder(), folder() + "/Trip.frond"), filePath(folder(), "Trip"));
    }
    void copyFolderWithCardIgnoresTemps() {
        const auto v7 = v7WithSd();
        const auto sd = tinyFrondsd();
        write(filePath(folder(), "Trip"), v7);
        write(machineDir(folder(), "Trip") + "/sd.frondsd", sd);
        write(machineDir(folder(), "Trip") + "/.frond.tmp-leftover", "tmp");
        write(machineDir(folder(), "Trip") + "/.gen-sd.frondsd-1", "gen");
        QString name;
        QVERIFY(importFile(folder(), machineDir(folder(), "Trip"), &name).isEmpty());
        QCOMPARE(name, QString("Trip 2"));
        QVERIFY(QFileInfo::exists(filePath(folder(), "Trip 2")));
        QVERIFY(QFileInfo::exists(machineDir(folder(), "Trip 2") + "/sd.frondsd"));
        // The card's slot.json is part of the machine now.
        QCOMPARE(describe(filePath(folder(), "Trip 2")).size,
                 qint64(v7.size() + sd.size() + QFileInfo(machineDir(folder(), "Trip 2") + "/slot.json").size()));
        QVERIFY(!QFileInfo::exists(machineDir(folder(), "Trip 2") + "/.frond.tmp-leftover"));
        QVERIFY(!QFileInfo::exists(machineDir(folder(), "Trip 2") + "/.gen-sd.frondsd-1"));
        QVERIFY(exportFile(describe(filePath(folder(), "Trip")), directory.filePath("out")).isEmpty());
        QVERIFY(QFileInfo::exists(directory.filePath("out/out.frond")));
        QVERIFY(QFileInfo::exists(directory.filePath("out/sd.frondsd")));
        QVERIFY(!QFileInfo::exists(directory.filePath("out/.frond.tmp-leftover")));
        QVERIFY(rename(folder(), describe(filePath(folder(), "Trip")), "Moved").isEmpty());
        QVERIFY(QFileInfo::exists(filePath(folder(), "Moved")));
        QVERIFY(QFileInfo::exists(machineDir(folder(), "Moved") + "/sd.frondsd"));
        QVERIFY(!QFileInfo::exists(machineDir(folder(), "Trip")));
        QVERIFY(remove(describe(filePath(folder(), "Moved"))).isEmpty());
        QVERIFY(!QFileInfo::exists(machineDir(folder(), "Moved")));
        QVERIFY(QFileInfo::exists(filePath(folder(), "Trip 2")));
    }
    void removeKeepsOthersLock() {
        write(filePath(folder(), "Busy"), header(6, "palm-tx"));
        QLockFile lock(filePath(folder(), "Busy") + ".lock"); QVERIFY(lock.tryLock());
        QVERIFY(!remove(describe(filePath(folder(), "Busy"))).isEmpty());
        QVERIFY(QFileInfo::exists(filePath(folder(), "Busy")) && QFileInfo::exists(filePath(folder(), "Busy") + ".lock"));
        lock.unlock();
        QVERIFY(remove(describe(filePath(folder(), "Busy"))).isEmpty());
        QVERIFY(!QFileInfo::exists(filePath(folder(), "Busy") + ".lock"));
        QVERIFY(!QFileInfo::exists(machineDir(folder(), "Busy")));
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
        QCOMPARE(frondAudio::lowLatencyOption(frondFirmware::Device::tx, {}, raw), QString("--low-latency-queue"));
        QCOMPARE(frondAudio::lowLatencyOption(frondFirmware::deviceFromId("sony-clie-tg50"), {}, {}), QString("--low-latency-queue"));
        QCOMPARE(frondAudio::lowLatencyOption(frondFirmware::deviceFromId("palm-zire-72"), {}, {}), QString("--low-latency-queue"));
        // A Zire 72 NOR of the right size that isn't the qualified one gets the queue limits only.
        const auto zire = directory.filePath("zire.bin"); write(zire, QByteArray(14942208, char(0x61)));
        QVERIFY(!frondAudio::qualified(frondFirmware::deviceFromId("palm-zire-72"), {}, zire));
        QCOMPARE(frondAudio::lowLatencyOption(frondFirmware::deviceFromId("palm-zire-72"), {}, zire), QString("--low-latency-queue"));
        const auto e3 = frondFirmware::deviceFromId("palm-e2-franken");
        QVERIFY(frondAudio::lowLatencyAvailable(e3));
        QVERIFY(frondFirmware::capabilities(e3).fastAudio);
        QCOMPARE(QByteArray(frondFirmware::profile(e3).frontend.fastAudioNorMd5), QByteArray("a79f98bddd50c61e7eb89eee46eb81bd"));
        QVERIFY(!frondAudio::qualified(e3, {}, {}));
        QCOMPARE(frondAudio::lowLatencyOption(e3, {}, {}), QString("--low-latency-queue"));
        QVERIFY(frondAudio::lowLatencyOption(frondFirmware::deviceFromId("palm-iiix"), {}, {}).isEmpty());
        QVERIFY(frondAudio::lowLatencyOption(frondFirmware::Device::automatic, {}, {}).isEmpty());
    }
    void variantNorDigest() {
        const auto path = filePath(folder(), "E2 variant"), raw = directory.filePath("denvfs.bin");
        for (int size : {6 << 20, 8 << 20, 7 << 20}) {
            const QByteArray nor(size, char(0x61));
            const auto digest = QCryptographicHash::hash(nor, QCryptographicHash::Md5).toHex();
            write(raw, nor);
            QCOMPARE(frondAudio::matchesNor(frondFirmware::Device::e2, {}, raw, digest), size != (7 << 20));
            for (quint32 version : {6u, 7u, 8u}) {
                for (bool withCard : {false, true}) {
                    if (version == 6 && withCard) continue;
                    auto bytes = header(version, "palm-e2").left(40);
                    put32(bytes, 0); put32(bytes, 0);
                    if (version >= 7) {
                        for (quint64 value : {quint64(4), quint64(size), quint64(0), quint64(0), quint64(0)}) put64(bytes, value);
                        put32(bytes, withCard ? 1 : 0);
                        if (withCard) bytes += (version == 8 ? v8WithSd(1) : v7WithSd()).mid(92);
                    } else {
                        for (quint32 value : {4u, quint32(size), 0u, 0u, 0u, 0u}) put32(bytes, value);
                    }
                    bytes += qCompress("meta" + nor + "guest data").mid(4);
                    write(path, bytes);
                    QCOMPARE(frondAudio::sessionNorMd5(path), size == (7 << 20) ? QByteArray() : digest);
                    QVERIFY(!frondAudio::qualified(frondFirmware::Device::e2, path, raw));
                }
            }
            QCOMPARE(frondAudio::lowLatencyOption(frondFirmware::Device::e2, {}, raw), QString("--low-latency-queue"));
        }
    }
    void qualifiedDenvfsAudio() {
        const auto source = qEnvironmentVariable("FROND_TEST_E2_DENVFS_NOR");
        if (source.isEmpty()) QSKIP("FROND_TEST_E2_DENVFS_NOR not set");
        QFile input(source);
        QVERIFY(input.open(QIODevice::ReadOnly)); QCOMPARE(input.size(), qint64(6 << 20));
        auto nor = input.readAll();
        QCOMPARE(QCryptographicHash::hash(nor, QCryptographicHash::Md5).toHex(), QByteArray("c2d90f775b1737de5bcd11634bf51d7d"));
        const auto raw = directory.filePath("qualified-denvfs.bin"), saved = filePath(folder(), "Qualified E2");
        const auto fast = QString("--fast-audio"), queue = QString("--low-latency-queue");
        auto session = [&](quint32 version, const QByteArray& firmware) {
            auto bytes = header(version, "palm-e2").left(40);
            put32(bytes, 0); put32(bytes, 0);
            if (version >= 7) {
                for (quint64 n : {quint64(0), quint64(firmware.size()), quint64(0), quint64(0), quint64(0)}) put64(bytes, n);
                put32(bytes, 0);
            } else {
                for (quint32 n : {0u, quint32(firmware.size()), 0u, 0u, 0u, 0u}) put32(bytes, n);
            }
            return bytes + qCompress(firmware).mid(4);
        };
        write(raw, nor);
        QCOMPARE(frondAudio::lowLatencyOption(frondFirmware::Device::e2, {}, raw), fast);
        for (quint32 version : {6u, 7u, 8u}) {
            write(saved, session(version, nor));
            QCOMPARE(frondAudio::lowLatencyOption(frondFirmware::Device::e2, saved, raw), fast);
            nor[0] = char(nor[0] ^ 1); write(raw, nor);
            QCOMPARE(frondAudio::lowLatencyOption(frondFirmware::Device::e2, {}, raw), queue);
            QCOMPARE(frondAudio::lowLatencyOption(frondFirmware::Device::e2, saved, raw), fast);
            write(saved, session(version, nor));
            nor[0] = char(nor[0] ^ 1); write(raw, nor);
            QCOMPARE(frondAudio::lowLatencyOption(frondFirmware::Device::e2, saved, raw), queue);
        }
        QVERIFY(!frondAudio::qualified(frondFirmware::deviceFromId("palm-e2-franken"), {}, raw));
        QVERIFY(input.seek(0));
        QCOMPARE(QCryptographicHash::hash(input.readAll(), QCryptographicHash::Md5).toHex(), QByteArray("c2d90f775b1737de5bcd11634bf51d7d"));
    }
    void machineControlVisibilityPreferences() {
        const auto one = filePath(folder(), "TG50 One"), two = filePath(folder(), "TG50 Two");
        write(one, header(6, "sony-clie-tg50"));
        write(two, header(6, "sony-clie-tg50"));
        const auto guest = QFileInfo(one).size();
        Preferences loaded;
        QVERIFY(!readPreferences(one, &loaded));
        // New machines put the jog controls left of the keyboard.
        QVERIFY(loaded.showKeyboard && loaded.jogControls == JogControls::keyboard);
        // Settings from before the jog controls: shown, left of the LCD.
        write(preferencesPath(one), R"({"version":1,"scale":7,"smooth":true,"audio":false,"fastAudio":true,"microphone":true})");
        QVERIFY(readPreferences(one, &loaded));
        QVERIFY(loaded.showKeyboard && loaded.jogControls == JogControls::screen && loaded.microphone);
        QVERIFY(writePreferences(two, loaded).isEmpty());
        // The old switch maps on to the choice: shown to left of screen, off to hidden.
        for (bool shown : {false, true}) {
            Preferences old;
            write(preferencesPath(one), QByteArray(R"({"version":1,"scale":7,"smooth":true,"audio":false,"fastAudio":true,"showJogControls":)") +
                                        (shown ? "true" : "false") + "}");
            QVERIFY(readPreferences(one, &old));
            QCOMPARE(old.jogControls, shown ? JogControls::screen : JogControls::hidden);
            write(preferencesPath(one), R"({"version":1,"scale":7,"smooth":true,"audio":false,"fastAudio":true,"jogControls":"sideways","showJogControls":false})");
            QVERIFY(readPreferences(one, &old));
            QCOMPARE(old.jogControls, JogControls::hidden);
        }
        for (bool keyboard : {false, true})
        for (auto jog : {JogControls::hidden, JogControls::screen, JogControls::keyboard}) {
            loaded.showKeyboard = keyboard; loaded.jogControls = jog;
            QVERIFY(writePreferences(one, loaded).isEmpty());
            Preferences roundTrip;
            QVERIFY(readPreferences(one, &roundTrip));
            QCOMPARE(roundTrip.showKeyboard, keyboard); QCOMPARE(roundTrip.jogControls, jog);
            QCOMPARE(roundTrip.scale, 7);
            QVERIFY(roundTrip.smooth && !roundTrip.audio && roundTrip.fastAudio && roundTrip.microphone);
            // Older Frond versions still read the switch.
            QFile saved(preferencesPath(one)); QVERIFY(saved.open(QIODevice::ReadOnly));
            const auto object = QJsonDocument::fromJson(saved.readAll()).object();
            QCOMPARE(object.value("jogControls").toString(), jogControlsName(jog));
            QCOMPARE(object.value("showJogControls").toBool(), jog != JogControls::hidden);
            QVERIFY(readPreferences(two, &roundTrip));
            QVERIFY(roundTrip.showKeyboard && roundTrip.jogControls == JogControls::screen);
            QCOMPARE(QFileInfo(one).size(), guest);
        }
    }
    // A Palm IIIx session: one v8 file with no cards, its contrast and speed in the machine settings.
    void iiixSession() {
        QByteArray bytes;
        for (quint32 value : {frondSessions::magic, 8u}) put32(bytes, value);
        bytes.append(QByteArray("palm-iiix").leftJustified(32, '\0', true));
        put32(bytes, 0); put32(bytes, 0);
        for (quint64 size : {0x178000ull, 0ull, 4ull << 20, 0ull, 0ull}) put64(bytes, size);
        put32(bytes, 0);
        bytes.append("savestate");
        const auto path = filePath(folder(), "Palm IIIx");
        write(path, bytes);
        const auto info = describe(path);
        QCOMPARE(info.device, QString("palm-iiix"));
        QVERIFY(info.saved && !info.old && openable(readHeader(path)));
        Preferences prefs, loaded;
        QCOMPARE(prefs.contrast, 50); QVERIFY(!prefs.uncapped);
        QVERIFY(!prefs.contrastSet); prefs.contrast = 30; prefs.contrastSet = true; prefs.uncapped = true;
        QVERIFY(writePreferences(path, prefs).isEmpty());
        QVERIFY(readPreferences(path, &loaded));
        QCOMPARE(loaded.contrast, 30); QVERIFY(loaded.uncapped);
        { QFile saved(preferencesPath(path)); QVERIFY(saved.open(QIODevice::ReadOnly));
          const auto json = saved.readAll(); QVERIFY(json.contains(R"("speed":"uncapped")") && json.contains(R"("contrast":30)")); }
        QString name;
        const auto error = duplicate(folder(), info, &name);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(name, QString("Palm IIIx 2"));
        const auto copy = filePath(folder(), name);
        { QFile file(copy); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), bytes); }
        QCOMPARE(describe(copy).device, QString("palm-iiix"));
        QVERIFY(readPreferences(copy, &loaded));
        QCOMPARE(loaded.contrast, 30); QVERIFY(loaded.uncapped);
        // Settings from before these fields read as contrast 50 and Original.
        write(preferencesPath(copy), R"({"version":1,"scale":6,"smooth":false,"audio":true,"fastAudio":false})");
        QVERIFY(readPreferences(copy, &loaded));
        QCOMPARE(loaded.contrast, 50); QVERIFY(!loaded.uncapped); QVERIFY(!loaded.contrastSet);
    }
    void machinePreferences() {
        const auto one = filePath(folder(), "One"), two = filePath(folder(), "Two");
        write(one, header(6, "palm-tx"));
        QVERIFY(create(folder(), "Two", "palm-tx"));
        Preferences prefs{7, true, false, true}, loaded;
        QVERIFY(writePreferences(one, prefs).isEmpty());
        QVERIFY(writePreferences(two, Preferences{}).isEmpty());
        QVERIFY(readPreferences(one, &loaded)); QCOMPARE(loaded.scale, 7);
        QVERIFY(loaded.smooth && !loaded.audio && loaded.fastAudio && !loaded.fastEngine);
        // The engine is saved by name and defaults to Exact.
        prefs.fastEngine = true;
        QVERIFY(writePreferences(one, prefs).isEmpty());
        QVERIFY(readPreferences(one, &loaded) && loaded.fastEngine);
        { QFile saved(preferencesPath(one)); QVERIFY(saved.open(QIODevice::ReadOnly)); QVERIFY(saved.readAll().contains(R"("engine":"fast")")); }
        prefs.fastEngine = false;
        QVERIFY(writePreferences(one, prefs).isEmpty());
        QVERIFY(readPreferences(one, &loaded) && !loaded.fastEngine);
        // The bar is shown unless this machine hid it.
        QVERIFY(loaded.barShown);
        prefs.barShown = false;
        QVERIFY(writePreferences(one, prefs).isEmpty());
        QVERIFY(readPreferences(one, &loaded) && !loaded.barShown);
        prefs.barShown = true;
        QVERIFY(writePreferences(one, prefs).isEmpty());
        QVERIFY(readPreferences(one, &loaded) && loaded.barShown);
        { QFile saved(preferencesPath(one)); QVERIFY(saved.open(QIODevice::ReadOnly)); QVERIFY(!saved.readAll().contains("barShown")); }
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
        QVERIFY(readPreferences(directory.filePath("copy/copy.frond"), &loaded)); QCOMPARE(loaded.scale, 7);
        QString imported;
        QVERIFY(importFile(folder(), directory.filePath("copy"), &imported).isEmpty());
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
