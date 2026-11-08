// Files… edits a .frondsd or .frondmsp in place (desktop/CardFile.h, FatImage): few writes, one
// generation per visit, and a cut-off change settles to a card that loads. The same cases as
// Android's CardFileTest, and leftovers in Android's format settle here.
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtEndian>
#include <map>
#include <zlib.h>
#include "CardFiles.h"
#include "SdImage.h"

using namespace frondCardFile;
namespace cf = frondCardFiles;

namespace {
struct Crash {};

QByteArray readAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
bool writeAll(const QString& path, const QByteArray& b) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(b) == b.size();
}
QByteArray randomBytes(int n, quint32 seed) {
    QRandomGenerator g(seed);
    QByteArray b(n, '\0');
    for (auto& c : b) c = char(g.bounded(256));
    return b;
}
QByteArray blank(unsigned mib = 32) {
    const auto bytes = frondSd::create(mib);
    return QByteArray(reinterpret_cast<const char*>(bytes.data()), int(bytes.size()));
}
quint32 crcOf(const QByteArray& b) { return quint32(::crc32(0, reinterpret_cast<const Bytef*>(b.constData()), uInt(b.size()))); }

void put(FatImage& fat, const QString& folder, const QString& name, const QByteArray& data) {
    qint64 at = 0;
    fat.add(folder, name, [&](char* b, qint64 n) {
        const auto m = std::min<qint64>(n, data.size() - at);
        std::memcpy(b, data.constData() + at, size_t(m));
        at += m;
        return m;
    }, data.size());
}
void visit(const QString& file, const std::function<void(FatImage&)>& f) {
    auto c = CardFile::open(file, true);
    FatImage fat(*c);
    f(fat);
}
QByteArray readFile(FatImage& fat, const QString& path) {
    const auto e = fat.find(path);
    if (!e) return {};
    QByteArray out;
    fat.read(*e, [&](const char* b, qint64 n) { out.append(b, int(n)); });
    return out;
}
// Every path with its bytes ("dir" for folders), and the free space; the card passes the FAT check.
QPair<QMap<QString, QString>, qint64> snapshot(const QString& file) {
    auto c = CardFile::open(file, false);
    FatImage fat(*c);
    const auto problems = fat.check();
    if (!problems.isEmpty()) qWarning() << problems;
    QMap<QString, QString> out;
    std::function<void(const QString&)> walk = [&](const QString& path) {
        for (const auto& e : fat.list(path)) {
            const auto full = FatImage::joined(path, e.name);
            if (e.directory) { out[full] = "dir"; walk(full); }
            else {
                const auto bytes = readFile(fat, full);
                out[full] = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex()) + ':' + QString::number(bytes.size());
            }
        }
    };
    walk("/");
    if (!problems.isEmpty()) out["!problems"] = problems.join("; ");
    return {out, fat.freeBytes()};
}
// The card file as its 512-byte blocks that aren't all zero: "FRONDSP1", the length, then each
// block's offset and bytes (little endian).
QByteArray sparse(const QByteArray& b) {
    QByteArray out("FRONDSP1");
    QByteArray n(8, '\0');
    qToLittleEndian<quint64>(quint64(b.size()), n.data());
    out += n;
    for (qint64 at = 0; at < b.size(); at += 512) {
        auto block = b.mid(int(at), 512);
        block.resize(512, '\0');
        if (block.count('\0') == 512) continue;
        qToLittleEndian<quint64>(quint64(at), n.data());
        out += n + block;
    }
    return out;
}
QByteArray unsparse(const QByteArray& b) {
    if (!b.startsWith("FRONDSP1") || b.size() < 16) return {};
    QByteArray out(int(qFromLittleEndian<quint64>(b.constData() + 8)), '\0');
    for (int i = 16; i + 520 <= b.size(); i += 520) {
        const auto at = int(qFromLittleEndian<quint64>(b.constData() + i));
        out.replace(at, std::min(512, int(out.size()) - at), b.mid(i + 8, std::min(512, int(out.size()) - at)));
    }
    return out;
}
// The generation, then each path with "dir" or its size and CRC-32, by path.
QString expected(const QString& file) {
    QMap<QString, QString> out;
    {
        auto c = CardFile::open(file, false);
        FatImage fat(*c);
        if (!fat.check().isEmpty()) return "problems " + fat.check().join("; ");
        std::function<void(const QString&)> walk = [&](const QString& path) {
            for (const auto& e : fat.list(path)) {
                const auto full = FatImage::joined(path, e.name);
                if (e.directory) { out[full] = "dir"; walk(full); }
                else {
                    const auto bytes = readFile(fat, full);
                    out[full] = QStringLiteral("%1\t%2").arg(bytes.size()).arg(crcOf(bytes), 8, 16, QChar('0'));
                }
            }
        };
        walk("/");
    }
    QString text = QStringLiteral("generation %1\n").arg(verify(file).value_or(0));
    for (auto it = out.begin(); it != out.end(); ++it) text += it.key() + '\t' + it.value() + '\n';
    return text;
}
const QList<QPair<QString, QByteArray>> leftoverCases{{"commit", "journal"}, {"intent", "data"}};
}  // namespace

class CardFileTest : public QObject {
    Q_OBJECT
    QTemporaryDir root;

    // A 32 MiB FAT16 card as a v2 card file at `generation`, or v1.
    QString card(const QString& tag, const QString& name = "sd.frondsd", quint64 generation = 5, bool v1 = false) {
        const auto dir = root.filePath("cardfile-" + tag);
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
        const auto payload = blank();
        const auto file = dir + '/' + name;
        if (v1) {
            QByteArray head(64, '\0');
            head.replace(0, 8, QByteArray("FRONDSD\0", 8));
            qToLittleEndian<quint32>(1, head.data() + 8);
            qToLittleEndian<quint32>(512, head.data() + 12);
            qToLittleEndian<quint64>(quint64(payload.size()), head.data() + 16);
            qToLittleEndian<quint32>(crcOf(payload), head.data() + 24);
            writeAll(file, head + payload);
        } else writeAll(file, encodeV2(payload, generation));
        if (verify(file) != std::optional<quint64>(v1 ? 0 : generation)) qFatal("card made wrong");
        return file;
    }

    // Cut off at each step, as a killed process would be, then settled by the next open or load.
    void crashTest(const QString& tag, const std::function<void(FatImage&)>& prepare, const std::function<void(FatImage&)>& change) {
        const QList<QPair<QString, bool>> steps{{"writing", false}, {"data", false}, {"journal", true}, {"partly", true}, {"applied", true}};
        for (const auto& [step, isNew] : steps) {
            const auto file = card(tag + '-' + step);
            visit(file, prepare);
            const auto old = snapshot(file);
            const auto reference = card(tag + '-' + step + "-ref");
            visit(reference, [&](FatImage& fat) { prepare(fat); change(fat); });
            const auto fresh = snapshot(reference);
            {
                auto c = CardFile::open(file, true);
                auto* raw = c.get();
                const auto wanted = step.toLatin1();
                c->crashHook = [raw, wanted](const char* at) {
                    if (wanted == at) { raw->dead = true; throw Crash(); }
                };
                FatImage fat(*c);
                bool crashed = false;
                try { change(fat); } catch (const Crash&) { crashed = true; }
                QVERIFY2(crashed, qPrintable(tag + ' ' + step + " didn't crash"));
            }
            QVERIFY2(unsettled(file), qPrintable(tag + ' ' + step + " leaves a journal"));
            QVERIFY(settle(file));
            QVERIFY(!unsettled(file));
            QCOMPARE(verify(file), std::optional<quint64>(7));
            QCOMPARE(snapshot(file), isNew ? fresh : old);
            // The card still takes changes, and the next visit moves the generation on again.
            visit(file, [](FatImage& fat) { put(fat, "/", "after.txt", "\x09"); });
            QVERIFY(snapshot(file).first.contains("/after.txt"));
            QCOMPARE(verify(file), std::optional<quint64>(8));
        }
    }

 private slots:
    void anEditWritesOnlyWhatItTouches() {
        for (const QString name : {"sd.frondsd", "ms.frondmsp"}) {
            const auto file = card("few-" + name, name);
            visit(file, [](FatImage& fat) { fat.makeFolder("/", "PALM"); });
            const auto before = readAll(file);
            const auto data = randomBytes(5000, 1);
            qint64 bytes = 0;
            QList<QPair<qint64, qint64>> writes;
            {
                auto c = CardFile::open(file, true);
                {
                    FatImage fat(*c);
                    put(fat, "/PALM", "A long file name.prc", data);
                }
                writes = c->writes;
                bytes = c->cardBytesWritten;
            }
            const auto after = readAll(file);
            QCOMPARE(after.size(), before.size());
            // The data clusters (5000 bytes in 2 KiB clusters), a FAT sector in each copy, the folder
            // sector, one table entry and the header.
            QVERIFY2(bytes <= 3 * 2048 + 3 * 512 + 4 + 64, qPrintable(QString::number(bytes)));
            QVERIFY(writes.size() <= 8);
            for (int i = 0; i < before.size(); ++i)
                if (before[i] != after[i])
                    QVERIFY2(std::any_of(writes.begin(), writes.end(), [&](const QPair<qint64, qint64>& w) { return i >= w.first && i < w.first + w.second; }),
                             qPrintable(QString("byte %1 changed outside a write").arg(i)));
            QCOMPARE(verify(file), std::optional<quint64>(7));
            auto c = CardFile::open(file, false);
            FatImage fat(*c);
            QCOMPARE(readFile(fat, "/PALM/A long file name.prc"), data);
        }
    }

    void theGenerationMovesOncePerVisit() {
        const auto file = card("generation");
        int marked = 0;
        {
            auto c = CardFile::open(file, true);
            c->beforeFirstWrite = [&] { marked++; };
            { FatImage fat(*c); QVERIFY(fat.list("/").isEmpty()); }
            QCOMPARE(marked, 0);
            { FatImage fat(*c); fat.makeFolder("/", "PALM"); }
            { FatImage fat(*c); put(fat, "/PALM", "one.txt", "\x01\x02\x03"); }
            { FatImage fat(*c); fat.remove("/PALM/one.txt"); }
            QCOMPARE(c->generation(), quint64(6));
        }
        QCOMPARE(marked, 1);
        QCOMPARE(verify(file), std::optional<quint64>(6));
        // Looking only changes nothing.
        { auto c = CardFile::open(file, true); FatImage fat(*c); fat.list("/PALM"); }
        QCOMPARE(verify(file), std::optional<quint64>(6));
        visit(file, [](FatImage& fat) { fat.makeFolder("/", "DCIM"); });
        QCOMPARE(verify(file), std::optional<quint64>(7));
    }

    void aV1CardBecomesV2() {
        const auto file = card("v1", "sd.frondsd", 0, true);
        visit(file, [](FatImage& fat) { put(fat, "/", "x.txt", "\x07"); });
        QCOMPARE(verify(file), std::optional<quint64>(1));
        QCOMPARE(QFileInfo(file).size(), 64 + (qint64(32) << 20) + 4 * 32);
        QVERIFY(snapshot(file).first.contains("/x.txt"));
    }

    void aFailedChangeLeavesALoadableCard() {
        const auto file = card("failed");
        const auto old = snapshot(file);
        {
            auto c = CardFile::open(file, true);
            FatImage fat(*c);
            // The input is short: some data was written, then the change failed.
            const QByteArray data(5000, '\0');
            bool failed = false;
            qint64 at = 0;
            try {
                fat.add("/", "x", [&](char* b, qint64 n) {
                    const auto m = std::min<qint64>(n, data.size() - at);
                    std::memcpy(b, data.constData() + at, size_t(m));
                    at += m;
                    return m;
                }, 9000);
            } catch (const Error&) { failed = true; }
            QVERIFY(failed);
        }
        QVERIFY(!unsettled(file));
        QCOMPARE(verify(file), std::optional<quint64>(6));
        QCOMPARE(snapshot(file), old);
    }

    void oneEditorAtATime() {
        const auto file = card("lock");
        {
            auto c = CardFile::open(file, true);
            QVERIFY(inUse(file));
            QVERIFY(!ready(file));
            bool refused = false;
            try { CardFile::open(file, true); } catch (const Error& e) { refused = e.message == inUseMessage(); }
            QVERIFY(refused);
        }
        QVERIFY(!inUse(file));
        QVERIFY(ready(file));
    }

    void aWriteProtectedCardRefusesChanges() {
        const auto file = card("protected");
        const auto before = readAll(file);
        {
            auto c = CardFile::open(file, false);
            FatImage fat(*c);
            bool refused = false;
            try { fat.makeFolder("/", "PALM"); } catch (const Error&) { refused = true; }
            QVERIFY(refused);
        }
        QCOMPARE(readAll(file), before);
    }

    void addSurvivesCrashes() {
        const auto data = randomBytes(300001, 7);
        crashTest("add", [](FatImage& fat) { fat.makeFolder("/", "PALM"); },
                  [&](FatImage& fat) { put(fat, "/PALM", "A long file name.prc", data); });
    }
    void makeFolderSurvivesCrashes() {
        crashTest("mkdir", [](FatImage& fat) { fat.makeFolder("/", "PALM"); }, [](FatImage& fat) { fat.makeFolder("/PALM", "Launcher"); });
    }
    void copySurvivesCrashes() {
        const auto data = randomBytes(300001, 7);
        crashTest("copy", [&](FatImage& fat) {
            fat.makeFolder("/", "PALM");
            put(fat, "/PALM", "a.pdb", data);
            fat.makeFolder("/", "Backup");
        }, [](FatImage& fat) { fat.copy("/PALM", "/Backup"); });
    }

    // A crash that leaves only the intent file, on a v1 card: the settle makes it a v2 card.
    void aV1CardSettlesToV2() {
        const auto file = card("v1-crash", "sd.frondsd", 0, true);
        const auto old = snapshot(file);
        const auto data = randomBytes(300001, 7);
        {
            auto c = CardFile::open(file, true);
            auto* raw = c.get();
            c->crashHook = [raw](const char* at) { if (QByteArray(at) == "data") { raw->dead = true; throw Crash(); } };
            FatImage fat(*c);
            bool crashed = false;
            try { put(fat, "/", "x.bin", data); } catch (const Crash&) { crashed = true; }
            QVERIFY(crashed);
        }
        // As a v1 card the whole CRC no longer matches.
        QVERIFY(!verify(file));
        QVERIFY(settle(file));
        QCOMPARE(verify(file), std::optional<quint64>(1));
        QCOMPARE(snapshot(file), old);
    }

    // Leftovers written as Android's CardFile.kt writes them, encoded here by hand from its format:
    // a commit file is replayed, an intent file alone gets its chunk CRCs fixed.
    void androidLeftoversSettle() {
        auto be32 = [](quint32 v) { QByteArray b(4, '\0'); qToBigEndian(v, b.data()); return b; };
        auto be64 = [](quint64 v) { QByteArray b(8, '\0'); qToBigEndian(v, b.data()); return b; };
        auto le32 = [](quint32 v) { QByteArray b(4, '\0'); qToLittleEndian(v, b.data()); return b; };
        auto le64 = [](quint64 v) { QByteArray b(8, '\0'); qToLittleEndian(v, b.data()); return b; };
        const qint64 payloadBytes = qint64(32) << 20;
        const int count = 32;
        // Bytes near the end of the card, past anything a fresh card uses.
        const qint64 at = payloadBytes - 3 * 2048;
        const auto scribble = randomBytes(2048, 3);
        auto header = [&](const QByteArray& table, quint64 generation) {
            QByteArray h(64, '\0');
            h.replace(0, 8, QByteArray("FRONDSD\0", 8));
            h.replace(8, 4, le32(2)); h.replace(12, 4, le32(512)); h.replace(16, 8, le64(quint64(payloadBytes)));
            h.replace(28, 8, le64(generation)); h.replace(36, 8, le64(quint64(64 + payloadBytes))); h.replace(44, 4, le32(1 << 20));
            h.replace(24, 4, le32(crcOf(h + table)));
            return h;
        };

        // A change that got as far as its commit file.
        {
            const auto file = card("android-commit");
            auto payload = readAll(file).mid(64, int(payloadBytes));
            auto changed = payload;
            changed.replace(int(at), scribble.size(), scribble);
            QByteArray table;
            for (int i = 0; i < count; ++i) table += le32(crcOf(changed.mid(i << 20, 1 << 20)));
            const int chunk = int(at >> 20);
            QByteArray commit = QByteArray("FRONDFC1");
            commit += be32(3) + be64(quint64(64 + payloadBytes + 4 * count));
            commit += be64(quint64(64 + at)) + be32(quint32(scribble.size())) + scribble;
            commit += be64(quint64(64 + payloadBytes + 4 * chunk)) + be32(4) + table.mid(4 * chunk, 4);
            commit += be64(0) + be32(64) + header(table, 6);
            commit += QByteArray("FCEND\0\0\0", 8);
            commit += be32(crcOf(commit));
            QVERIFY(writeAll(commitOf(file), commit));
            QVERIFY(writeAll(intentOf(file), QByteArray("FRONDFI1") + le64(quint64(payloadBytes)) + le64(6) + le32(quint32(chunk))));
            QVERIFY(frondCardFile::ready(file));
            QVERIFY(!unsettled(file));
            QCOMPARE(verify(file), std::optional<quint64>(6));
            QCOMPARE(readAll(file).mid(int(64 + at), scribble.size()), scribble);
        }
        // Data written into free clusters, then cut off before the commit.
        {
            const auto file = card("android-intent");
            const int chunk = int(at >> 20);
            QFile f(file);
            QVERIFY(f.open(QIODevice::ReadWrite));
            f.seek(64 + at);
            f.write(scribble);
            f.close();
            QVERIFY(!verify(file));
            QVERIFY(writeAll(intentOf(file), QByteArray("FRONDFI1") + le64(quint64(payloadBytes)) + le64(6) + le32(quint32(chunk))));
            QVERIFY(settle(file));
            QVERIFY(!unsettled(file));
            QCOMPARE(verify(file), std::optional<quint64>(6));
            QVERIFY(snapshot(file).first.isEmpty());
        }
        // A damaged commit file is dropped; the intent still makes the card sound.
        {
            const auto file = card("android-damaged");
            QVERIFY(writeAll(commitOf(file), QByteArray("FRONDFC1") + QByteArray(40, 'x')));
            QVERIFY(writeAll(intentOf(file), QByteArray("FRONDFI1") + le64(quint64(payloadBytes)) + le64(6)));
            QVERIFY(settle(file));
            QCOMPARE(verify(file), std::optional<quint64>(6));
        }
    }

    // Replace keeps the old entry until the new one is complete: a cancel, a failing source, a
    // full card or a crash leaves the old one; a finished replace leaves only the new one.
    void replaceKeepsTheOldOne() {
        const auto file = card("replace");
        const auto first = randomBytes(9000, 21), second = randomBytes(150000, 22);
        QTemporaryDir host;
        writeAll(host.filePath("a.txt"), second);
        visit(file, [&](FatImage& fat) {
            put(fat, "/", "a.txt", first);
            fat.makeFolder("/", "src");
            put(fat, "/src", "a.txt", second);
        });
        const auto old = snapshot(file);
        cf::Resolver replace([](const QString&) { return cf::Decision{cf::Clash::Replace, false}; });
        visit(file, [&](FatImage& fat) {
            // Cancelled part way through a host file.
            int calls = 0;
            bool cancelled = false;
            try { cf::addFile(fat, "/", host.filePath("a.txt"), replace, [&] { return ++calls > 3; }); } catch (const Cancelled&) { cancelled = true; }
            QVERIFY(cancelled && calls > 3);
            // Cancelled part way through a copy on the card.
            calls = 0;
            cancelled = false;
            try { cf::paste(fat, {"/src/a.txt"}, "/", false, replace, [&] { return ++calls > 3; }); } catch (const Cancelled&) { cancelled = true; }
            QVERIFY(cancelled);
            // A source that ends early.
            bool failed = false;
            qint64 given = 0;
            try {
                fat.replace("/a.txt", [&] {
                    fat.add("/", "a.txt", [&](char*, qint64 n) { const auto m = std::min<qint64>(n, 30000 - given); given += m; return m; }, 90000);
                });
            } catch (const Cancelled&) {
            } catch (const Error&) { failed = true; }
            QVERIFY(failed);
            // A new file that doesn't fit beside the old one.
            failed = false;
            const qint64 tooBig = fat.freeBytes() + fat.clusterBytes();
            try {
                fat.replace("/a.txt", [&] { fat.add("/", "a.txt", [](char* b, qint64 n) { std::memset(b, 1, size_t(n)); return n; }, tooBig); });
            } catch (const Cancelled&) {
            } catch (const Error&) { failed = true; }
            QVERIFY(failed);
        });
        QVERIFY(!unsettled(file));
        QCOMPARE(snapshot(file), old);
        // Done: only the new one, and the old one's clusters are free again.
        visit(file, [&](FatImage& fat) {
            QCOMPARE(cf::addFile(fat, "/", host.filePath("a.txt"), replace), QString("a.txt"));
            QCOMPARE(readFile(fat, "/a.txt"), second);
            cf::paste(fat, {"/src/a.txt"}, "/", true, replace);
            QVERIFY(!fat.find("/src/a.txt"));
            QCOMPARE(readFile(fat, "/a.txt"), second);
            QVERIFY2(fat.check().isEmpty(), qPrintable(fat.check().join("; ")));
        });
        qint64 cluster = 0;
        visit(file, [&](FatImage& fat) { cluster = fat.clusterBytes(); });
        const auto after = snapshot(file);
        QCOMPARE(after.second, old.second + (qint64(first.size()) + cluster - 1) / cluster * cluster);
        QCOMPARE(after.first.keys(), QStringList({"/a.txt", "/src"}));
    }

    void replaceSurvivesCrashes() {
        const auto first = randomBytes(9000, 31), second = randomBytes(40000, 32);
        crashTest("replace", [&](FatImage& fat) { put(fat, "/", "a.txt", first); },
                  [&](FatImage& fat) { fat.replace("/a.txt", [&] { put(fat, "/", "a.txt", second); }); });
    }

    // ---- Leftovers shared with Android ----
    // tests/fixtures/card-leftovers/<side>/: a card cut off at the commit file ("commit") and at
    // the data ("intent"), written by each app's CardFile, with what it holds once settled. Each
    // app settles both sides' fixtures. FROND_FIXTURE_OUT=<dir> writes this side's again.

    void writeLeftoverFixtures() {
        const auto out = qEnvironmentVariable("FROND_FIXTURE_OUT");
        if (out.isEmpty()) return;
        QDir(out).removeRecursively();
        QDir().mkpath(out);
        for (const auto& [name, step] : leftoverCases) {
            const auto file = card("fixture-" + name);
            visit(file, [](FatImage& fat) { fat.makeFolder("/", "PALM"); });
            {
                auto c = CardFile::open(file, true);
                auto* raw = c.get();
                const auto wanted = step;
                c->crashHook = [raw, wanted](const char* at) { if (wanted == at) { raw->dead = true; throw Crash(); } };
                FatImage fat(*c);
                bool crashed = false;
                try { put(fat, "/PALM", QString::fromUtf8("Ünïcode long name for the card.prc"), randomBytes(5000, 41)); } catch (const Crash&) { crashed = true; }
                QVERIFY(crashed);
            }
            QVERIFY(writeAll(out + '/' + name + ".sparse", sparse(readAll(file))));
            if (QFile::exists(intentOf(file))) QVERIFY(QFile::copy(intentOf(file), out + '/' + name + ".files-intent"));
            if (QFile::exists(commitOf(file))) QVERIFY(QFile::copy(commitOf(file), out + '/' + name + ".files-commit"));
            QVERIFY(settle(file));
            QVERIFY(writeAll(out + '/' + name + ".expect", expected(file).toUtf8()));
        }
    }

    void leftoversFromEitherAppSettle() {
        const QString fixtures = QStringLiteral(FROND_FIXTURES) + "/card-leftovers";
        for (const auto* side : {"android", "desktop"})
            for (const auto& [name, step] : leftoverCases) {
                const auto from = fixtures + '/' + side + '/' + name;
                const auto dir = root.filePath(QStringLiteral("leftover-%1-%2").arg(side, name));
                QDir().mkpath(dir);
                const auto file = dir + "/sd.frondsd";
                const auto bytes = unsparse(readAll(from + ".sparse"));
                QVERIFY2(!bytes.isEmpty(), qPrintable(from));
                QVERIFY(writeAll(file, bytes));
                for (const auto* kind : {".files-intent", ".files-commit"})
                    if (QFile::exists(from + kind)) QVERIFY(QFile::copy(from + kind, file + kind));
                QVERIFY2(unsettled(file), qPrintable(from));
                QVERIFY(frondCardFile::ready(file));
                QVERIFY(!unsettled(file));
                QCOMPARE(expected(file), QString::fromUtf8(readAll(from + ".expect")));
            }
    }

    // Long and non-ASCII names, dates, and the file manager's paste, rename, delete and host copies.
    void fileManagerOperations() {
        const auto file = card("operations");
        const QString longName = QString::fromUtf8("A rather long file name, with spaces and dots.v2.prc");
        const QString accented = QString::fromUtf8("héllo über 日本.txt");
        QTemporaryDir host;
        QDir(host.path()).mkpath("in/Deep");
        writeAll(host.filePath("in/" + longName), randomBytes(70000, 11));
        writeAll(host.filePath("in/Deep/" + accented), "caf\xc3\xa9");
        writeAll(host.filePath("single.txt"), "one");
        int asked = 0;
        cf::Resolver keep([&](const QString&) { asked++; return cf::Decision{cf::Clash::KeepBoth, true}; });
        visit(file, [&](FatImage& fat) {
            fat.makeFolder("/", "PALM");
            auto report = cf::addHost(fat, {host.filePath("in"), host.filePath("single.txt")}, "/PALM", keep);
            QCOMPARE(report.done, 3);
            QVERIFY(fat.find("/PALM/in/Deep/" + accented));
            QCOMPARE(fat.find("/PALM/in/" + longName)->size, qint64(70000));
            QVERIFY(fat.find("/PALM/single.txt")->modified.isValid());
            // A second add of the same file asks once, "for all", and keeps both.
            report = cf::addHost(fat, {host.filePath("single.txt"), host.filePath("single.txt")}, "/PALM", keep);
            QCOMPARE(asked, 1);
            QVERIFY(fat.find("/PALM/single (2).txt") && fat.find("/PALM/single (3).txt"));
            // Copy next to itself is "(2)"; a folder can't go into itself.
            cf::Resolver never([](const QString&) { return cf::Decision{cf::Clash::Cancel, false}; });
            cf::paste(fat, {"/PALM/in"}, "/PALM", false, never);
            QVERIFY(fat.find("/PALM/in (2)/Deep/" + accented));
            bool refused = false;
            try { cf::paste(fat, {"/PALM/in"}, "/PALM/in/Deep", true, never); } catch (const Error&) { refused = true; }
            QVERIFY(refused);
            fat.makeFolder("/", "Backup");
            cf::paste(fat, {"/PALM/in (2)", "/PALM/single.txt"}, "/Backup", true, never);
            QVERIFY(!fat.find("/PALM/in (2)") && fat.find("/Backup/in (2)/Deep/" + accented) && fat.find("/Backup/single.txt"));
            cf::rename(fat, "/Backup/single.txt", QString::fromUtf8("été.txt"));
            QVERIFY(fat.find(QString::fromUtf8("/Backup/ÉTÉ.TXT")));
            QCOMPARE(cf::remove(fat, {"/Backup/in (2)"}), 1);
            QVERIFY(!fat.find("/Backup/in (2)"));
            QVERIFY(fat.check().isEmpty());
        });
        // Out to the host with the exact names and bytes; a name there already gets " (2)".
        auto c = CardFile::open(file, false);
        FatImage fat(*c);
        QDir(host.path()).mkpath("out");
        QCOMPARE(cf::exportTo(fat, {"/PALM/in"}, host.filePath("out")), 2);
        QCOMPARE(readAll(host.filePath("out/in/" + longName)), readAll(host.filePath("in/" + longName)));
        QCOMPARE(readAll(host.filePath("out/in/Deep/" + accented)), QByteArray("caf\xc3\xa9"));
        QCOMPARE(cf::exportTo(fat, {"/PALM/in"}, host.filePath("out")), 2);
        QVERIFY(QFileInfo(host.filePath("out/in (2)/Deep/" + accented)).isFile());
        QCOMPARE(cf::keepBothName(QString(260, 'a') + ".txt", [](const QString&) { return false; }).size(), 255);
    }

    // With FROND_CARD_OUT set, the edited cards are kept there (with the files they should hold in
    // <name>.files/) so the core can be made to load them.
    void writeCardsForTheCore() {
        const auto out = qEnvironmentVariable("FROND_CARD_OUT");
        if (out.isEmpty()) return;
        QDir(out).removeRecursively();
        QDir().mkpath(out);
        const QString longName = "A rather long file name for a card, with spaces and dots.v2.prc";
        const QList<QPair<QString, QByteArray>> files{
            {"/PALM/" + longName, randomBytes(70000, 11)},
            {QString::fromUtf8("/PALM/Launcher/h\u00e9llo \u00fcber \u65e5\u672c.txt"), "caf\xc3\xa9"},
            {"/PALM/Launcher/Deep/x.bin", randomBytes(5000, 12)},
            // The files the SD probe on the Palm looks for.
            {"/HOST.TXT", "Hello from host\n"},
            {"/DELETE.TXT", "Delete me\n"},
        };
        auto folders = [](FatImage& fat) {
            fat.makeFolder("/", "PALM"); fat.makeFolder("/PALM", "Launcher"); fat.makeFolder("/PALM/Launcher", "Deep");
        };
        auto fillFiles = [&](FatImage& fat) {
            for (const auto& [path, bytes] : files) {
                const int slash = int(path.lastIndexOf('/'));
                put(fat, slash == 0 ? "/" : path.left(slash), path.mid(slash + 1), bytes);
            }
        };
        auto keep = [&](const QString& name, const QString& file) {
            QFile::remove(out + '/' + name);
            QVERIFY(QFile::copy(file, out + '/' + name));
            for (const auto& [path, bytes] : files) {
                const auto host = out + '/' + name + ".files" + path;
                QDir().mkpath(QFileInfo(host).path());
                QVERIFY(writeAll(host, bytes));
            }
        };
        const auto v2 = card("core-v2");
        visit(v2, [&](FatImage& fat) { folders(fat); fillFiles(fat); });
        keep("v2.frondsd", v2);
        const auto v1 = card("core-v1", "sd.frondsd", 0, true);
        visit(v1, [&](FatImage& fat) { folders(fat); fillFiles(fat); });
        QCOMPARE(verify(v1), std::optional<quint64>(1));
        keep("v1-to-v2.frondsd", v1);
        const auto crashed = card("core-crash");
        visit(crashed, folders);
        {
            auto c = CardFile::open(crashed, true);
            auto* raw = c.get();
            c->crashHook = [raw](const char* at) { if (QByteArray(at) == "journal") { raw->dead = true; throw Crash(); } };
            FatImage fat(*c);
            bool crashed = false;
            try { fillFiles(fat); } catch (const Crash&) { crashed = true; }
            QVERIFY(crashed);
        }
        QVERIFY(settle(crashed));
        keep("crash-settled.frondsd", crashed);
        QVERIFY(writeAll(out + "/crash-settled.frondsd.list", QStringList(snapshot(crashed).first.keys()).join('\n').toUtf8() + '\n'));
    }
};

QTEST_GUILESS_MAIN(CardFileTest)
#include "card-file.moc"
