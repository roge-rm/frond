#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
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
