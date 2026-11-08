#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <QDateTime>
#include <QDir>
#include <QMap>
#include <QProcess>
#include <QStandardPaths>
#include <QRegularExpression>
#include "FolderCard.h"
#include "SdExchange.h"
#include "SdImage.h"

static QByteArray read(const QString& path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll();
}
static bool write(const QString& path, const QByteArray& data) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
static QByteArray digest(const QString& path) {
    QFile file(path); QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) return {}; return hash.result();
}
static bool createCard(const QString& path, unsigned size) {
    const auto bytes = frondSd::create(size);
    return write(path, QByteArray::fromRawData(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

// Every FAT copy of a card image made by createCard is the same.
static bool fatCopiesMatch(const QString& image) {
    QFile f(image);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(frondFolderCard::partitionOffset)) return false;
    const auto boot = f.read(512);
    const int reserved = frondFolderCard::le16(boot, 14), fats = uint8_t(boot[16]), sectors = frondFolderCard::le16(boot, 22);
    QByteArray first;
    for (int i = 0; i < fats; ++i) {
        if (!f.seek(frondFolderCard::partitionOffset + qint64(reserved + i * sectors) * 512)) return false;
        const auto copy = f.read(qint64(sectors) * 512);
        if (i == 0) first = copy; else if (copy != first) return false;
    }
    return fats > 0;
}
static QString mtools(const QString& tool, const QString& image, const QStringList& args) {
    QProcess p;
    p.setProcessEnvironment(frondFolderCard::mtoolsEnvironment());
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStandardPaths::findExecutable(tool),
            QStringList{"-i", image + "@@" + QString::number(frondFolderCard::partitionOffset)} + args);
    p.closeWriteChannel();
    if (!p.waitForFinished(30000) || p.exitCode() != 0) return {};
    return QString::fromUtf8(p.readAll());
}
// What mtools finds under `path` of the card image, copied out with names kept exactly.
static QString extract(const QString& image, const QString& into) {
    QDir().mkpath(into);
    mtools("mcopy", image, {"-s", "-m", "::/*", into});
    return into;
}
// The names past ASCII that importFolderCard is tested with, and a few plain ones.
static const QStringList kNames{QString::fromUtf8("äpfel.txt"), QString::fromUtf8("ÄB.txt"), QString::fromUtf8("naïve café.txt"),
                                QString::fromUtf8("émile.TXT"), QString::fromUtf8("日本語.md"), QString::fromUtf8("ñ"),
                                "README.TXT", "x", "a rather long file name.txt", "PLAIN.TXT", "lower.txt"};


// Raw access to a createCard image (partition at 63 sectors).
struct Raw {
    QString path; int reserved, fats, fatSectors, rootEntries, perCluster;
    explicit Raw(const QString& p) : path(p) {
        QFile f(p); f.open(QIODevice::ReadOnly); f.seek(frondFolderCard::partitionOffset);
        const auto b = f.read(512);
        reserved = frondFolderCard::le16(b, 14); fats = uint8_t(b[16]); fatSectors = frondFolderCard::le16(b, 22);
        rootEntries = frondFolderCard::le16(b, 17); perCluster = uint8_t(b[13]);
    }
    qint64 part(qint64 sector) const { return frondFolderCard::partitionOffset + sector * 512; }
    qint64 rootAt() const { return part(reserved + qint64(fats) * fatSectors); }
    qint64 clusterAt(uint32_t c) const { return rootAt() + rootEntries * 32 + qint64(c - 2) * perCluster * 512; }
    QByteArray get(qint64 at, int n) const { QFile f(path); f.open(QIODevice::ReadOnly); f.seek(at); return f.read(n); }
    void put(qint64 at, const QByteArray& d) const { QFile f(path); f.open(QIODevice::ReadWrite); f.seek(at); f.write(d); }
    void setFat(uint32_t c, uint32_t v) const {
        QByteArray two(2, 0); frondFolderCard::putLe16(two, 0, v);
        for (int i = 0; i < fats; ++i) put(part(reserved + qint64(i) * fatSectors) + c * 2, two);
    }
    // Writes entry bytes into the first free root slot.
    void addRoot(const QByteArray& entry) const {
        const auto root = get(rootAt(), rootEntries * 32);
        for (int i = 0; i < rootEntries; ++i)
            if (root[i * 32] == 0) { put(rootAt() + i * 32, entry); return; }
    }
    // Entry cluster of the folder with a short-only plain name inside the directory bytes.
    static int find(const QByteArray& dir, const QByteArray& shortBytes) {
        for (int i = 0; i < dir.size() / 32; ++i) if (dir.mid(i * 32, 11) == shortBytes && (dir[i * 32 + 11] & 0x10)) return int(frondFolderCard::le16(dir, i * 32 + 26));
        return -1;
    }
};

class ExchangeTest : public QObject {
    Q_OBJECT
 private slots:
    void initTestCase() { QVERIFY2(SdExchange::available(), "Install mtools before running SD exchange tests"); }
    void paths() {
        for (const auto* path : {"/HOST.TXT", "/PALM/Long file name.sunvox", "/-argument", "/song's tune"})
            QVERIFY(SdExchange::validPath(path));
        for (const auto* path : {"relative", "/", "/..", "/a/../b", "/a/./b", "/a//b", "/a/", "/a*", "/a?", "/a[b]", "/a:b", "/a\\b", "/a\nb", "/a.", "/a ", "/ a"})
            QVERIFY2(!SdExchange::validPath(path), path);
        QVERIFY(SdExchange::validPath("/", true));
    }
    void roundtrip_data() {
        QTest::addColumn<unsigned>("size");
        for (unsigned size : {32, 64, 128}) QTest::newRow(qPrintable(QString::number(size))) << size;
    }
    void roundtrip() {
        QFETCH(unsigned, size);
        QTemporaryDir files;
        const auto original = files.filePath("source.img"), input = files.filePath(QString::fromUtf8("hôte song.bin"));
        QVERIFY(createCard(original, size));
        QByteArray data(256 * 1024 + 137, Qt::Uninitialized);
        for (qsizetype i = 0; i < data.size(); ++i) data[i] = char(i * 73);
        QVERIFY(write(input, data));
        const auto before = digest(original);
        QString error, listing;
        SdExchange exchange(files.filePath("work-XXXXXX"));
        QVERIFY2(exchange.open(original, error), qPrintable(error));
        QVERIFY2(exchange.mkdir("/PALM", error), qPrintable(error));
        QVERIFY2(exchange.mkdir("/PALM/Programs", error), qPrintable(error));
        QVERIFY2(exchange.put(input, "/PALM/Programs/Long song file.bin", error), qPrintable(error));
        QVERIFY2(exchange.list("/PALM/Programs", listing, error), qPrintable(error));
        QVERIFY(listing.contains("Long song file.bin"));
        const auto current = digest(exchange.image());
        QVERIFY(!exchange.put(input, "/palm/programs/long SONG file.BIN", error));
        QVERIFY(!exchange.put(input, "/PALM/Programs/LONGSO~1.BIN", error));
        QVERIFY(!exchange.put(input, "/PALM", error));
        QVERIFY(!exchange.put(input, "/missing/file.bin", error));
        QVERIFY(!exchange.mkdir("/PALM", error));
        QCOMPARE(digest(exchange.image()), current);
        const auto output = files.filePath(QString::fromUtf8("sortie été.bin"));
        QVERIFY2(exchange.get("/PALM/Programs/Long song file.bin", output, error), qPrintable(error));
        QCOMPARE(read(output), data);
        QVERIFY(!exchange.get("/PALM/Programs/Long song file.bin", output, error));
        QVERIFY(!exchange.get("/PALM/Programs/Long song file.bin", original, error));
        const auto link = files.filePath("host-symlink");
        QVERIFY(QFile::link(original, link));
        QVERIFY(!exchange.get("/PALM/Programs/Long song file.bin", link, error));
        const auto dangling = files.filePath("dangling");
        QVERIFY(QFile::link(files.filePath("missing-target"), dangling));
        QVERIFY(!exchange.get("/PALM/Programs/Long song file.bin", dangling, error));
        QVERIFY(!QFileInfo::exists(files.filePath("missing-target")));
        const auto missing = files.filePath("not-created");
        QVERIFY(!exchange.get("/not-here", missing, error));
        QVERIFY(!QFileInfo::exists(missing));
        QCOMPARE(digest(original), before);
        QCOMPARE(read(input), data);
        // Reopen a committed candidate just as the core would import it.
        SdExchange reopened(files.filePath("reopen-XXXXXX"));
        QVERIFY2(reopened.open(exchange.image(), error), qPrintable(error));
        QVERIFY2(reopened.get("/PALM/Programs/Long song file.bin", files.filePath("again"), error), qPrintable(error));
        QCOMPARE(read(files.filePath("again")), data);
    }
    // Files and folders with names past ASCII come back exactly, with the short names the
    // folder import gives, and the FAT copies stay equal.
    void exactNames() {
        QTemporaryDir files; QString error, listing;
        const auto source = files.filePath("source.img");
        QVERIFY(createCard(source, 32));
        SdExchange exchange(files.filePath("work-XXXXXX"));
        QVERIFY2(exchange.open(source, error), qPrintable(error));
        QVERIFY2(exchange.mkdir("/Dir", error), qPrintable(error));
        QSet<QString> takenFiles, takenFolders;
        QMap<QString, QByteArray> expectedShort;
        for (const auto& name : kNames) {
            const auto input = files.filePath("in.bin");
            QVERIFY(write(input, name.toUtf8() + QByteArray(5000, 'z')));
            QVERIFY2(exchange.put(input, "/Dir/" + name, error), qPrintable(name + ": " + error));
            QVERIFY(fatCopiesMatch(exchange.image()));
            const auto s = frondFolderCard::shortName(name, takenFiles);
            takenFiles.insert(frondFolderCard::shortDisplay(s));
            expectedShort[name] = s;
        }
        for (const auto& name : kNames) {
            QVERIFY2(exchange.mkdir("/Dir/Sub " + name, error), qPrintable(name + ": " + error));
            QVERIFY(fatCopiesMatch(exchange.image()));
            QVERIFY2(exchange.mkdir("/Dir/Sub " + name + "/" + name, error), qPrintable(name + ": " + error));
            QVERIFY2(exchange.put(files.filePath("in.bin"), "/Dir/Sub " + name + "/" + name + ".dat", error), qPrintable(error));
        }
        QVERIFY2(exchange.list("/Dir", listing, error), qPrintable(error));
        // mdir columns: 8 base, space, 3 extension.
        for (const auto& name : kNames) {
            const auto e = expectedShort[name];
            const auto base = QString::fromLatin1(e.left(8)).trimmed(), ext = QString::fromLatin1(e.mid(8, 3)).trimmed();
            bool found = false;
            for (const auto& line : listing.split('\n'))
                if (line.size() > 12 && line.left(8).trimmed() == base && line.mid(9, 3).trimmed() == ext &&
                    (frondFolderCard::shortDisplay(e) == name || line.trimmed().endsWith(name))) found = true;
            QVERIFY2(found, qPrintable(name + " as " + frondFolderCard::shortDisplay(e) + "\n" + listing));
        }
        const auto out = extract(exchange.image(), files.filePath("out"));
        QStringList got = QDir(out + "/Dir").entryList(QDir::Files);
        QStringList want = kNames;
        got.sort(); want.sort();
        QCOMPARE(got, want);
        for (const auto& name : kNames) {
            QFile f(out + "/Dir/" + name); QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(f.readAll(), name.toUtf8() + QByteArray(5000, 'z'));
            QVERIFY(QFileInfo(out + "/Dir/Sub " + name + "/" + name + ".dat").isFile());
            QVERIFY(QFileInfo(out + "/Dir/Sub " + name).isDir());
            QVERIFY(QFileInfo(out + "/Dir/Sub " + name + "/" + name).isDir());
        }
        QStringList folders = QDir(out + "/Dir").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(folders.size(), kNames.size());
        // get() by the exact and by another-case name.
        QVERIFY2(exchange.get(QString::fromUtf8("/dir/ÄPFEL.TXT"), files.filePath("got"), error), qPrintable(error));
        QVERIFY(read(files.filePath("got")).startsWith(QString::fromUtf8("äpfel.txt").toUtf8()));
        // Folders of a card hold "." and "..", so mtools can list into them.
        QVERIFY(!mtools("mdir", exchange.image(), {QString::fromUtf8("::/Dir/Sub 日本語.md/日本語.md")}).isEmpty());
        // Reopens as a fresh image.
        SdExchange again(files.filePath("again-XXXXXX"));
        QVERIFY2(again.open(exchange.image(), error), qPrintable(error));
    }
    void nameClashAnyCase() {
        QTemporaryDir files; QString error;
        const auto source = files.filePath("source.img"), input = files.filePath("in");
        QVERIFY(createCard(source, 32)); QVERIFY(write(input, "data"));
        SdExchange exchange(files.filePath("work-XXXXXX"));
        QVERIFY2(exchange.open(source, error), qPrintable(error));
        QVERIFY2(exchange.put(input, QString::fromUtf8("/äpfel.txt"), error), qPrintable(error));
        QVERIFY2(exchange.mkdir(QString::fromUtf8("/Ölfarben"), error), qPrintable(error));
        const auto before = digest(exchange.image());
        for (const auto* name : {"/ÄPFEL.TXT", "/Äpfel.Txt", "/_PFEL~1.TXT", "/_pfel~1.txt", "/ÖLFARBEN", "/ölfarben"}) {
            const auto n = QString::fromUtf8(name);
            QVERIFY2(!exchange.put(input, n, error), name);
            QVERIFY2(!exchange.mkdir(n, error), name);
            QVERIFY(error.contains("already"));
            QCOMPARE(digest(exchange.image()), before);
        }
    }
    void subfolderGrows() {
        QTemporaryDir files; QString error;
        const auto source = files.filePath("source.img"), input = files.filePath("in");
        QVERIFY(createCard(source, 32)); QVERIFY(write(input, "grow"));
        SdExchange exchange(files.filePath("work-XXXXXX"));
        QVERIFY2(exchange.open(source, error), qPrintable(error));
        QVERIFY2(exchange.mkdir("/Full", error), qPrintable(error));
        // A 2 KB cluster holds 64 slots; each of these names takes five long entries and the short one.
        QStringList names;
        for (int i = 0; i < 24; ++i) names << QString("a long file name that needs three long entries %1.txt").arg(i, 2, 10, QChar('0'));
        for (const auto& n : names) QVERIFY2(exchange.put(input, "/Full/" + n, error), qPrintable(n + ": " + error));
        QVERIFY(fatCopiesMatch(exchange.image()));
        QVERIFY2(exchange.mkdir("/Full/after", error), qPrintable(error));
        QVERIFY2(exchange.put(input, "/Full/" + names.first().toUpper().replace(".TXT", ".bin"), error), qPrintable(error));
        const auto out = extract(exchange.image(), files.filePath("out"));
        QStringList got = QDir(out + "/Full").entryList(QDir::Files);
        QCOMPARE(got.size(), 25);
        for (const auto& n : names) QVERIFY2(got.contains(n), qPrintable(n));
        QVERIFY(QFileInfo(out + "/Full/after").isDir());
        QVERIFY(fatCopiesMatch(exchange.image()));
    }
    void fullRootRefuses() {
        QTemporaryDir files; QString error;
        const auto source = files.filePath("source.img"), input = files.filePath("in");
        QVERIFY(createCard(source, 32)); QVERIFY(write(input, "data"));
        {
            // Fill every free slot of the fixed root with empty files.
            QFile f(source); QVERIFY(f.open(QIODevice::ReadWrite));
            QVERIFY(f.seek(frondFolderCard::partitionOffset)); const auto boot = f.read(512);
            const int reserved = frondFolderCard::le16(boot, 14), fats = uint8_t(boot[16]), sectors = frondFolderCard::le16(boot, 22);
            const int entries = frondFolderCard::le16(boot, 17);
            const qint64 root = frondFolderCard::partitionOffset + qint64(reserved + fats * sectors) * 512;
            QVERIFY(f.seek(root)); auto bytes = f.read(entries * 32);
            for (int i = 0; i < entries; ++i) {
                if (bytes[i * 32] != 0) continue;
                QByteArray e(32, '\0');
                e.replace(0, 11, QString("F%1      ").arg(i, 4, 10, QChar('0')).left(8).toLatin1() + "TXT");
                e[11] = 0x20;
                bytes.replace(i * 32, 32, e);
            }
            QVERIFY(f.seek(root)); QCOMPARE(f.write(bytes), qint64(bytes.size()));
        }
        SdExchange exchange(files.filePath("work-XXXXXX"));
        QVERIFY2(exchange.open(source, error), qPrintable(error));
        const auto before = digest(exchange.image());
        QVERIFY(!exchange.put(input, "/NEW.TXT", error));
        QVERIFY2(error.contains("top folder is full"), qPrintable(error));
        QVERIFY(!exchange.mkdir("/NEWDIR", error));
        QVERIFY2(error.contains("top folder is full"), qPrintable(error));
        QCOMPARE(digest(exchange.image()), before);
        QVERIFY(exchange.mkdir("/F0000000.TXT/x", error) == false);
        QCOMPARE(digest(exchange.image()), before);
    }
    void tooBigForFreeSpace() {
        QTemporaryDir files; QString error;
        const auto source = files.filePath("source.img"), input = files.filePath("in"), small = files.filePath("small");
        QVERIFY(createCard(source, 32)); QVERIFY(write(small, "ok"));
        QFile big(input); QVERIFY(big.open(QIODevice::WriteOnly)); QVERIFY(big.resize(20ll << 20)); big.close();
        SdExchange exchange(files.filePath("work-XXXXXX"));
        QVERIFY2(exchange.open(source, error), qPrintable(error));
        QVERIFY2(exchange.put(input, "/ONE.BIN", error), qPrintable(error));
        const auto before = digest(exchange.image());
        QVERIFY(!exchange.put(input, "/TWO.BIN", error));
        QVERIFY2(error.contains("enough space"), qPrintable(error));
        QCOMPARE(digest(exchange.image()), before);
        QVERIFY2(exchange.put(small, "/SMALL.TXT", error), qPrintable(error));
        QVERIFY(fatCopiesMatch(exchange.image()));
    }
    void damagedFolderChains() {
        QTemporaryDir files; QString error;
        const auto source = files.filePath("source.img"), input = files.filePath("in");
        QVERIFY(createCard(source, 32)); QVERIFY(write(input, "data"));
        const Raw raw(source);
        const auto when = QDateTime::currentDateTime();
        raw.addRoot(frondFolderCard::dirEntry("BAD", QByteArray("BAD        "), 0x10, 0xffff, 0, when));
        raw.addRoot(frondFolderCard::dirEntry("FREE", QByteArray("FREE       "), 0x10, 5000, 0, when));
        raw.addRoot(frondFolderCard::dirEntry("ZERO", QByteArray("ZERO       "), 0x10, 0, 0, when));
        SdExchange exchange(files.filePath("work-XXXXXX"));
        QVERIFY2(exchange.open(source, error), qPrintable(error));
        const auto before = digest(exchange.image());
        for (const auto* folder : {"/BAD", "/FREE", "/ZERO"}) {
            QVERIFY2(!exchange.put(input, QString(folder) + "/x", error), folder);
            QVERIFY2(!exchange.mkdir(QString(folder) + "/x", error), folder);
            QCOMPARE(digest(exchange.image()), before);
        }
    }
    // A new ~N alias never equals the long name of another file.
    void aliasAvoidsLongNames() {
        QTemporaryDir files; QString error, listing;
        const auto source = files.filePath("source.img"), input = files.filePath("in");
        QVERIFY(createCard(source, 32)); QVERIFY(write(input, "data"));
        const Raw raw(source);
        raw.addRoot(frondFolderCard::dirEntry("_PFEL~1.TXT", QByteArray("ZZZZZZZ TXT"), 0x20, 0, 0, QDateTime::currentDateTime()).left(64));
        SdExchange exchange(files.filePath("work-XXXXXX"));
        QVERIFY2(exchange.open(source, error), qPrintable(error));
        QVERIFY2(exchange.put(input, QString::fromUtf8("/äpfel.txt"), error), qPrintable(error));
        QVERIFY2(exchange.list("/", listing, error), qPrintable(error));
        bool seen = false;
        for (const auto& line : listing.split('\n'))
            if (line.endsWith(QString::fromUtf8("äpfel.txt"))) { seen = true; QVERIFY2(!line.startsWith("_PFEL~1 "), qPrintable(line)); }
        QVERIFY2(seen, qPrintable(listing));
    }
    void folderCapRefuses() {
        QTemporaryDir files; QString error;
        const auto source = files.filePath("source.img"), input = files.filePath("in");
        QVERIFY(createCard(source, 32)); QVERIFY(write(input, "data"));
        SdExchange maker(files.filePath("make-XXXXXX"));
        QVERIFY2(maker.open(source, error), qPrintable(error));
        QVERIFY2(maker.mkdir("/BIG", error), qPrintable(error));
        const auto image = files.filePath("big.img");
        QVERIFY(QFile::copy(maker.image(), image));
        const Raw raw(image);
        const int clusterBytes = raw.perCluster * 512, count = 65536 * 32 / clusterBytes;
        const auto dir = raw.get(raw.rootAt(), raw.rootEntries * 32);
        const int start = Raw::find(dir, QByteArray("BIG        "));
        QVERIFY(start >= 2);
        // Chain the folder over count clusters and fill them with used entries.
        QByteArray fill(clusterBytes, '\0');
        for (int i = 0; i < clusterBytes / 32; ++i) { fill[i * 32] = 'F'; fill[i * 32 + 11] = 0x20; }
        QList<uint32_t> run{uint32_t(start)};
        for (uint32_t c = 100; run.size() < count; ++c) run << c;
        for (int i = 0; i < run.size(); ++i) raw.setFat(run[i], i + 1 < run.size() ? run[i + 1] : 0xffff);
        for (int i = 1; i < run.size(); ++i) raw.put(raw.clusterAt(run[i]), fill);
        QByteArray first = raw.get(raw.clusterAt(start), clusterBytes);
        for (int i = 2; i < clusterBytes / 32; ++i) { first[i * 32] = 'F'; first[i * 32 + 11] = 0x20; }
        raw.put(raw.clusterAt(start), first);
        SdExchange exchange(files.filePath("work-XXXXXX"));
        QVERIFY2(exchange.open(image, error), qPrintable(error));
        const auto before = digest(exchange.image());
        QVERIFY(!exchange.put(input, "/BIG/x", error));
        QVERIFY2(error.contains("65536"), qPrintable(error));
        QVERIFY(!exchange.mkdir("/BIG/y", error));
        QCOMPARE(digest(exchange.image()), before);
    }
    void dotEntries() {
        QTemporaryDir files; QString error;
        const auto source = files.filePath("source.img");
        QVERIFY(createCard(source, 32));
        SdExchange exchange(files.filePath("work-XXXXXX"));
        QVERIFY2(exchange.open(source, error), qPrintable(error));
        QVERIFY2(exchange.mkdir("/A", error), qPrintable(error));
        QVERIFY2(exchange.mkdir("/A/B", error), qPrintable(error));
        const Raw raw(exchange.image());
        const int bytes = raw.perCluster * 512;
        const auto root = raw.get(raw.rootAt(), raw.rootEntries * 32);
        const int a = Raw::find(root, QByteArray("A          "));
        QVERIFY(a >= 2);
        const auto aDir = raw.get(raw.clusterAt(a), bytes);
        QCOMPARE(aDir.left(11), QByteArray(".          "));
        QCOMPARE(int(frondFolderCard::le16(aDir, 26)), a);
        QCOMPARE(aDir.mid(32, 11), QByteArray("..         "));
        QCOMPARE(int(frondFolderCard::le16(aDir, 32 + 26)), 0);
        const int b = Raw::find(aDir, QByteArray("B          "));
        QVERIFY(b >= 2 && b != a);
        const auto bDir = raw.get(raw.clusterAt(b), bytes);
        QCOMPARE(int(frondFolderCard::le16(bDir, 26)), b);
        QCOMPARE(int(frondFolderCard::le16(bDir, 32 + 26)), a);
    }
    void superfloppyAndEmptyFile() {
        QTemporaryDir files; QString error;
        const auto source = files.filePath("superfloppy.img"), empty = files.filePath("empty");
        auto image = frondSd::create(32);
        image.erase(image.begin(), image.begin() + 63 * 512);
        image.resize(32u << 20);
        frondSd::put32(image.data() + 28, 0);
        QVERIFY(write(source, QByteArray::fromRawData(reinterpret_cast<const char*>(image.data()), image.size())));
        QVERIFY(write(empty, {}));
        const auto before = digest(source);
        SdExchange exchange(files.filePath("work-XXXXXX"));
        QVERIFY2(exchange.open(source, error), qPrintable(error));
        QVERIFY2(exchange.put(empty, "/EMPTY.TXT", error), qPrintable(error));
        QVERIFY2(exchange.get("/EMPTY.TXT", files.filePath("export"), error), qPrintable(error));
        QVERIFY(QFileInfo::exists(files.filePath("export"))); QCOMPARE(QFileInfo(files.filePath("export")).size(), 0);
        QCOMPARE(digest(source), before);
    }
    // A 256 MB card opens only when the device allows it. Built sparse beside the test binary, not in /tmp.
    void largeCard() {
        QTemporaryDir files(QCoreApplication::applicationDirPath() + "/large-card-XXXXXX");
        QVERIFY(files.isValid());
        const auto source = files.filePath("source.img"), input = files.filePath("input.bin");
        {
            QFile card(source); QVERIFY(card.open(QIODevice::WriteOnly)); QVERIFY(card.resize(256ll << 20));
            for (const auto& sector : frondSd::layout(256)) {
                QVERIFY(card.seek(qint64(sector.index) * 512));
                QCOMPARE(card.write(reinterpret_cast<const char*>(sector.bytes.data()), 512), qint64(512));
            }
        }
        QVERIFY(write(input, "large card payload"));
        QString error;
        SdExchange small(files.filePath("small-XXXXXX"));
        QVERIFY(!small.open(source, error));
        QVERIFY2(error.contains("128 MB"), qPrintable(error));
        SdExchange exchange(files.filePath("work-XXXXXX"));
        QVERIFY2(exchange.open(source, error, qint64(2048) << 20), qPrintable(error));
        QVERIFY2(exchange.put(input, "/BIG.TXT", error), qPrintable(error));
        QVERIFY2(exchange.get("/BIG.TXT", files.filePath("output.bin"), error), qPrintable(error));
        QCOMPARE(read(files.filePath("output.bin")), QByteArray("large card payload"));
        QCOMPARE(QFileInfo(exchange.image()).size(), qint64(256) << 20);
    }
    void fullCardRollback() {
        QTemporaryDir files; QString error;
        const auto source = files.filePath("source.img"), large = files.filePath("large");
        QVERIFY(createCard(source, 32));
        QFile file(large); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.resize(33ll << 20)); file.close();
        SdExchange exchange(files.filePath("work-XXXXXX"));
        QVERIFY2(exchange.open(source, error), qPrintable(error));
        const auto before = digest(exchange.image());
        QVERIFY(!exchange.put(large, "/LARGE.BIN", error));
        QCOMPARE(digest(exchange.image()), before);
        QVERIFY(write(files.filePath("small"), "still usable"));
        QVERIFY2(exchange.put(files.filePath("small"), "/SMALL.TXT", error), qPrintable(error));
    }
    void subprocessFailureRollback() {
        QTemporaryDir files; QString error;
        const auto source = files.filePath("source.img"), input = files.filePath("input");
        QVERIFY(createCard(source, 32)); QVERIFY(write(input, "payload"));
        SdExchange exchange(files.filePath("work-XXXXXX"));
        QVERIFY2(exchange.open(source, error), qPrintable(error));
        const auto before = digest(exchange.image());
        const auto script = files.filePath("mcopy");
        QVERIFY(write(script, "#!/usr/bin/python3\nimport sys\np=sys.argv[sys.argv.index('-i')+1].split('@@')[0]\nwith open(p,'r+b') as f: f.write(b'partial failed operation')\nsys.exit(1)\n"));
        QVERIFY(QFile::setPermissions(script, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto oldPath = qgetenv("PATH");
        qputenv("PATH", files.path().toUtf8() + ':' + oldPath);
        const bool result = exchange.put(input, "/NEW.TXT", error);
        qputenv("PATH", oldPath);
        QVERIFY(!result); QCOMPARE(digest(exchange.image()), before); QCOMPARE(digest(source), before);
    }
    void invalidImage() {
        QTemporaryDir files; QString error;
        const auto path = files.filePath("broken.img");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.resize(32ll << 20)); file.close();
        SdExchange exchange;
        QVERIFY(!exchange.open(path, error)); QVERIFY(exchange.image().isEmpty());
        QVERIFY(createCard(path, 32));
        QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.seek(454)); QCOMPARE(file.write(QByteArray(4, '\xff')), 4); file.close();
        QVERIFY(!exchange.open(path, error)); QVERIFY(exchange.image().isEmpty());
    }
};
QTEST_GUILESS_MAIN(ExchangeTest)
#include "sd-exchange.moc"
