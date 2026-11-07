#include "SdExchange.h"
#include "FatWrite.h"
#include <QStorageInfo>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtEndian>

SdExchange::SdExchange(const QString& scratchTemplate)
    : directory(scratchTemplate.isEmpty() ? QDir::tempPath() + "/frond-exchange-XXXXXX" : scratchTemplate) {}

bool SdExchange::available() {
    for (const auto* name : {"mdir", "mcopy"})
        if (QStandardPaths::findExecutable(name).isEmpty()) return false;
    return true;
}

bool SdExchange::validPath(const QString& path, bool root) {
    if (root && path == "/") return true;
    if (!path.startsWith('/') || path.size() > 240) return false;
    // Literal FAT paths only: mtools also accepts wildcards and drive syntax.
    static const QRegularExpression forbidden(R"([\x00-\x1f\x7f*?\[\]<>|":\\])");
    for (const auto& part : path.mid(1).split('/')) {
        if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') ||
            part.endsWith(' ') || part.startsWith(' ') || forbidden.match(part).hasMatch()) return false;
    }
    return true;
}

bool SdExchange::run(const QString& tool, const QString& image, const QStringList& args,
                     QString& output, QString& error) {
    QProcess process;
    auto env = QProcessEnvironment::systemEnvironment();
    for (const auto& key : env.keys()) if (key.startsWith("MTOOLS")) env.remove(key);
    env.insert("MTOOLSRC", "/dev/null");
    env.insert("LC_ALL", "C.UTF-8");
    process.setProcessEnvironment(env);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QStandardPaths::findExecutable(tool),
                  QStringList{"-i", image + "@@" + QString::number(offset)} + args);
    process.closeWriteChannel();
    if (!process.waitForStarted(3000)) {
        error = "Install mtools to exchange files (Debian: sudo apt install mtools)."; return false;
    }
    if (!process.waitForFinished(15000)) {
        process.kill(); process.waitForFinished();
        error = "SD file operation timed out. The working image has not changed."; return false;
    }
    output = QString::fromUtf8(process.readAll());
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        error = tool + ": " + output.trimmed(); return false;
    }
    return true;
}

bool SdExchange::open(const QString& source, QString& error, qint64 maxBytes) {
    if (!current.isEmpty()) { error = "An exchange is already open."; return false; }
    if (!directory.isValid()) { error = "Could not create private exchange storage."; return false; }
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return false; }
    auto boot = file.read(512);
    if (boot.size() != 512 || file.size() < (32ll << 20) || file.size() > maxBytes || file.size() % 512) {
        error = QString("Use a 32 MB to %1 FAT16 card image.")
            .arg(maxBytes >= (1ll << 30) ? QString("%1 GB").arg(maxBytes >> 30) : QString("%1 MB").arg(maxBytes >> 20));
        return false;
    }
    // The working copy and the copy each edit makes.
    const QStorageInfo storage(directory.path());
    if (storage.bytesAvailable() < 2 * file.size() + (16ll << 20)) {
        error = QString("There isn't room for the exchange copies. Free up about %1 MB and try again.")
            .arg((2 * file.size() + (16ll << 20) - storage.bytesAvailable() + (1 << 20) - 1) >> 20);
        return false;
    }
    auto word = [](const QByteArray& b, int at) { return qFromLittleEndian<quint16>(b.constData() + at); };
    auto dword = [](const QByteArray& b, int at) { return qFromLittleEndian<quint32>(b.constData() + at); };
    offset = 0;
    if (boot.mid(54, 8) != "FAT16   ") {
        // Frond cards have a single primary FAT16 partition. Never guess offsets.
        const auto type = quint8(boot[450]);
        const auto begin = dword(boot, 454), sectors = dword(boot, 458);
        if (word(boot, 510) != 0xaa55 || (type != 4 && type != 6 && type != 14) || !begin ||
            !sectors || (quint64(begin) + sectors) * 512 > quint64(file.size())) {
            error = "Exchange supports FAT16 superfloppies or a primary FAT16 partition."; return false;
        }
        offset = qint64(begin) * 512;
        if (!file.seek(offset)) { error = file.errorString(); return false; }
        boot = file.read(512);
    }
    if (boot.size() != 512 || boot.mid(54, 8) != "FAT16   " || word(boot, 11) != 512 || word(boot, 510) != 0xaa55) {
        error = "The card does not contain a supported FAT16 filesystem."; return false;
    }
    const auto path = directory.path() + "/card.img";
    if (!QFile::copy(source, path)) { error = "Could not copy the card image to private storage."; return false; }
    if (!QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner)) {
        QFile::remove(path); error = "Could not make the private image writable."; return false;
    }
    QString listing;
    if (!run("mdir", path, {"::/"}, listing, error)) { QFile::remove(path); return false; }
    current = path;
    return true;
}

bool SdExchange::list(const QString& path, QString& listing, QString& error) {
    if (current.isEmpty() || !validPath(path, true)) { error = "Enter a literal absolute card path, such as /PALM."; return false; }
    return run("mdir", current, {"::" + path}, listing, error);
}

QString SdExchange::candidate(QString& error) {
    const auto path = directory.path() + "/edit-" + QString::number(++serial) + ".img";
    if (current.isEmpty() || !QFile::copy(current, path)) { error = "Could not copy the working card image."; return {}; }
    return path;
}
void SdExchange::adopt(const QString& path) {
    const auto previous = current; current = path; QFile::remove(previous);
}

static QByteArray digest(const QString& path) {
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) return {};
    return hash.result();
}

namespace {
using frondFolderCard::dirEntry;
using frondFolderCard::le16;
using frondFolderCard::le32;
using frondFolderCard::putLe16;
using frondFolderCard::shortDisplay;
using frondFolderCard::shortName;

// In-place FAT16 writer for one image file at the partition offset. Writes go straight to the
// file it is given (a candidate copy); the caller drops the file when anything fails.
class FatVolume {
 public:
    struct Slot { QString name, shortText; bool dir{false}; uint32_t cluster{0}; int first{0}, count{0}; };
    struct Folder { uint32_t cluster{0}; };  // 0 is the fixed root

    FatVolume(const QString& path, qint64 offset) : file(path), base(offset) {}

    bool open(QString& error) {
        if (!file.open(QIODevice::ReadWrite)) { error = file.errorString(); return false; }
        const auto boot = readAt(base, 512);
        if (boot.size() != 512 || le16(boot, 11) != 512) return fail(error, "The card does not contain a supported FAT16 filesystem.");
        perCluster = uint8_t(boot[13]);
        const uint32_t reserved = le16(boot, 14);
        fats = uint8_t(boot[16]);
        rootEntries = le16(boot, 17);
        fatSectors = le16(boot, 22);
        const uint32_t total = le16(boot, 19) ? le16(boot, 19) : le32(boot, 32);
        if (!perCluster || (perCluster & (perCluster - 1)) || !reserved || !fats || !fatSectors || !rootEntries)
            return fail(error, "The card's FAT16 header is damaged.");
        fatStart = base + qint64(reserved) * 512;
        rootStart = fatStart + qint64(fats) * fatSectors * 512;
        const uint32_t rootSectors = (rootEntries * 32 + 511) / 512;
        dataStart = rootStart + qint64(rootSectors) * 512;
        if (total < reserved + fats * fatSectors + rootSectors) return fail(error, "The card's FAT16 header is damaged.");
        clusters = (total - reserved - fats * fatSectors - rootSectors) / perCluster;
        clusterBytes = qint64(perCluster) * 512;
        if (clusters < 4085 || clusters >= 65525 || (clusters + 2) * 2 > fatSectors * 512 || base + qint64(total) * 512 > file.size())
            return fail(error, "The card's FAT16 header is damaged.");
        fat = readAt(fatStart, int(fatSectors) * 512);
        if (fat.size() != int(fatSectors) * 512) return fail(error, "Could not read the card's FAT.");
        original = fat;
        return true;
    }
    qint64 clusterSize() const { return clusterBytes; }

    // The folder at an absolute path, found by long or short name in any case.
    bool find(const QStringList& parts, Folder& out, QString& error) {
        out = {};
        for (const auto& part : parts) {
            QList<Slot> existing;
            if (!list(out, existing, error)) return false;
            const auto hit = std::find_if(existing.begin(), existing.end(), [&](const Slot& s) { return same(s, part); });
            if (hit == existing.end()) return fail(error, QString("There's no folder %1 on the card.").arg(part));
            if (!hit->dir) return fail(error, QString("%1 is a file.").arg(part));
            if (!hit->cluster) return fail(error, QString("The card's folder %1 is damaged.").arg(part));
            out.cluster = hit->cluster;
        }
        return true;
    }
    bool list(const Folder& folder, QList<Slot>& result, QString& error) {
        QByteArray bytes;
        if (!folderBytes(folder, bytes, error)) return false;
        result.clear();
        QString parts[20];
        int longFirst = -1, longCount = 0, longSum = -1;
        for (int i = 0; i < bytes.size() / 32; ++i) {
            const int at = i * 32;
            const uint8_t first = uint8_t(bytes[at]), attr = uint8_t(bytes[at + 11]);
            if (first == 0) break;
            if (first == 0xe5) { longFirst = -1; continue; }
            if ((attr & 0x3f) == 0x0f) {
                const int order = first & 0x3f;
                if (first & 0x40) {
                    for (auto& p : parts) p.clear();
                    longFirst = i; longCount = order; longSum = uint8_t(bytes[at + 13]);
                }
                if (longFirst >= 0 && order >= 1 && order <= 20 && uint8_t(bytes[at + 13]) == longSum) {
                    static const int offsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                    QString text;
                    for (int j = 0; j < 13; ++j) {
                        const auto c = le16(bytes, at + offsets[j]);
                        if (c == 0 || c == 0xffff) break;
                        text += QChar(c);
                    }
                    parts[order - 1] = text;
                } else longFirst = -1;
                continue;
            }
            const auto raw = bytes.mid(at, 11);
            const auto shortText = shortDisplay(raw);
            QString longName;
            bool complete = longFirst >= 0 && longCount <= 20 && longSum == frondFolderCard::shortChecksum(raw);
            for (int k = 0; complete && k < longCount; ++k) { if (parts[k].isEmpty()) complete = false; else longName += parts[k]; }
            const int start = complete ? longFirst : i;
            longFirst = -1;
            if (attr & 0x08) continue;
            if (raw[0] == '.' ) continue;
            QString shown = shortText;
            if (!complete) {
                // Case bits from other writers (NT-style) lower-case a short-only name.
                const auto flags = uint8_t(bytes[at + 12]);
                const auto base = QString::fromLatin1(raw.left(8)).trimmed(), ext = QString::fromLatin1(raw.mid(8, 3)).trimmed();
                shown = (flags & 8 ? base.toLower() : base) + (ext.isEmpty() ? QString() : "." + (flags & 16 ? ext.toLower() : ext));
            }
            result.push_back({complete ? longName : shown, shortText, bool(attr & 0x10), le16(bytes, at + 26), start, i - start + 1});
        }
        return true;
    }
    static bool same(const Slot& s, const QString& name) {
        return s.name.toUpper() == name.toUpper() || s.shortText.toUpper() == name.toUpper();
    }

    bool addFile(const Folder& folder, const QString& name, const QString& source, QString& error) {
        QFile in(source);
        if (!in.open(QIODevice::ReadOnly)) return fail(error, "Could not read the host file.");
        const qint64 size = in.size();
        if (size >= (qint64(1) << 32)) return fail(error, "The file is too large for a FAT16 card.");
        QList<Slot> existing;
        if (!list(folder, existing, error) || !refuse(existing, name, error)) return false;
        QList<uint32_t> run;
        if (!allocate(int((size + clusterBytes - 1) / clusterBytes), run, error)) return false;
        qint64 left = size;
        for (const auto c : run) {
            auto chunk = in.read(std::min<qint64>(left, clusterBytes));
            if (chunk.size() != std::min<qint64>(left, clusterBytes)) return fail(error, "Could not read the host file.");
            left -= chunk.size();
            chunk = chunk.leftJustified(int(clusterBytes), '\0');
            if (!writeAt(clusterAt(c), chunk)) return fail(error, "Could not write the card image.");
        }
        return addEntry(folder, existing, name, 0x20, run.isEmpty() ? 0 : run.first(), uint32_t(size), error) && flushFat(error);
    }
    bool addFolder(const Folder& parent, const QString& name, QString& error) {
        QList<Slot> existing;
        if (!list(parent, existing, error) || !refuse(existing, name, error)) return false;
        QList<uint32_t> run;
        if (!allocate(1, run, error)) return false;
        const auto when = QDateTime::currentDateTime();
        QByteArray dot(11, ' '), dotdot(11, ' ');
        dot[0] = '.'; dotdot[0] = '.'; dotdot[1] = '.';
        QByteArray block = dirEntry(".", dot, 0x10, run[0], 0, when).right(32) + dirEntry("..", dotdot, 0x10, parent.cluster, 0, when).right(32);
        block = block.leftJustified(int(clusterBytes), '\0');
        if (!writeAt(clusterAt(run[0]), block)) return fail(error, "Could not write the card image.");
        return addEntry(parent, existing, name, 0x10, run[0], 0, error) && flushFat(error);
    }

 private:
    static bool fail(QString& error, const QString& text) { error = text; return false; }
    qint64 clusterAt(uint32_t c) const { return dataStart + qint64(c - 2) * clusterBytes; }
    QByteArray readAt(qint64 at, int length) { return file.seek(at) ? file.read(length) : QByteArray(); }
    bool writeAt(qint64 at, const QByteArray& bytes) { return file.seek(at) && file.write(bytes) == bytes.size(); }
    uint32_t fatAt(uint32_t c) const { return le16(fat, int(c) * 2); }
    void setFat(uint32_t c, uint32_t v) { putLe16(fat, int(c) * 2, v); }

    // The clusters of a chain. The start must be a used data cluster and the chain must end properly.
    bool chain(uint32_t start, QList<uint32_t>& out, QString& error) const {
        out.clear();
        if (start < 2 || start >= clusters + 2 || !fatAt(start)) return fail(error, "The card's FAT is damaged.");
        uint32_t c = start;
        while (c >= 2 && c < clusters + 2) {
            if (out.size() > int(clusters)) return fail(error, "The card's FAT has a loop.");
            out.push_back(c);
            c = fatAt(c);
        }
        if (c < 0xfff8) return fail(error, "The card's FAT is damaged.");
        return true;
    }
    bool folderBytes(const Folder& folder, QByteArray& out, QString& error) {
        if (!folder.cluster) {
            out = readAt(rootStart, rootEntries * 32);
            return out.size() == int(rootEntries) * 32 || fail(error, "Could not read the card's top folder.");
        }
        QList<uint32_t> list;
        if (!chain(folder.cluster, list, error)) return false;
        out.clear();
        for (const auto c : list) {
            const auto part = readAt(clusterAt(c), int(clusterBytes));
            if (part.size() != clusterBytes) return fail(error, "Could not read a card folder.");
            out += part;
        }
        return true;
    }
    static bool refuse(const QList<Slot>& existing, const QString& name, QString& error) {
        if (std::any_of(existing.begin(), existing.end(), [&](const Slot& s) { return same(s, name); }))
            return fail(error, "That card name already exists. Choose another name; files are never overwritten.");
        return true;
    }
    // Takes the first free clusters and chains them in the in-memory FAT.
    bool allocate(int count, QList<uint32_t>& out, QString& error) {
        out.clear();
        for (uint32_t c = 2; int(out.size()) < count && c < clusters + 2; ++c)
            if (!fatAt(c)) out.push_back(c);
        if (int(out.size()) < count) return fail(error, "There isn't enough space on the card.");
        for (int i = 0; i < out.size(); ++i) setFat(out[i], i + 1 < out.size() ? out[i + 1] : 0xffff);
        return true;
    }
    bool addEntry(const Folder& folder, const QList<Slot>& existing, const QString& name, uint8_t attr, uint32_t cluster,
                  uint32_t size, QString& error) {
        QSet<QString> taken;
        // Long names count too, so a new ~N alias never equals another file's name.
        for (const auto& s : existing) { taken.insert(s.shortText.toUpper()); taken.insert(s.name.toUpper()); }
        const auto shortBytes = shortName(name, taken);
        if (shortBytes.isEmpty()) return fail(error, "No short name is free for that name.");
        const auto entry = dirEntry(name, shortBytes, attr, cluster, size, QDateTime::currentDateTime());
        const int count = entry.size() / 32;
        QByteArray bytes;
        QList<uint32_t> chainList;
        int first = -1;
        for (;;) {
            if (!folderBytes(folder, bytes, error)) return false;
            int run = 0;
            for (int i = 0; i < bytes.size() / 32 && first < 0; ++i) {
                const uint8_t b = uint8_t(bytes[i * 32]);
                run = (b == 0 || b == 0xe5) ? run + 1 : 0;
                if (run == count) first = i - count + 1;
            }
            if (first >= 0) break;
            if (!folder.cluster) return fail(error, "The card's top folder is full. Use a subfolder.");
            if (!chain(folder.cluster, chainList, error)) return false;
            // A FAT folder holds at most 65536 entries (2 MB).
            if ((chainList.size() + 1) * clusterBytes > 65536 * 32)
                return fail(error, "That folder is full (a card folder holds at most 65536 names). Use another folder.");
            QList<uint32_t> added;
            if (!allocate(1, added, error)) return false;
            setFat(chainList.last(), added[0]);
            if (!writeAt(clusterAt(added[0]), QByteArray(int(clusterBytes), '\0'))) return fail(error, "Could not write the card image.");
        }
        if (folder.cluster && !chain(folder.cluster, chainList, error)) return false;
        for (int k = 0; k < count; ++k) {
            const qint64 at = qint64(first + k) * 32;
            const qint64 position = folder.cluster ? clusterAt(chainList[int(at / clusterBytes)]) + at % clusterBytes : rootStart + at;
            if (!writeAt(position, entry.mid(k * 32, 32))) return fail(error, "Could not write the card image.");
        }
        return true;
    }
    bool flushFat(QString& error) {
        for (int copy = 0; copy < fats; ++copy)
            if (!writeAt(fatStart + qint64(copy) * fatSectors * 512, fat)) return fail(error, "Could not write the card image.");
        return file.flush() || fail(error, "Could not write the card image.");
    }

    QFile file;
    qint64 base, fatStart{0}, rootStart{0}, dataStart{0}, clusterBytes{0};
    uint32_t perCluster{0}, fats{0}, rootEntries{0}, fatSectors{0}, clusters{0};
    QByteArray fat, original;
};

QStringList pathParts(const QString& path) { return path.mid(1).split('/'); }
}  // namespace

bool SdExchange::put(const QString& source, const QString& destination, QString& error) {
    if (current.isEmpty() || !validPath(destination)) { error = "Enter a complete card filename, such as /PALM/song.sunvox."; return false; }
    QString output;
    QTemporaryDir transfer(directory.path() + "/transfer-XXXXXX");
    const auto verified = transfer.path() + "/verified";
    if (!transfer.isValid() || !QFileInfo(source).isFile() || !QFileInfo(source).isReadable()) {
        error = "Could not read the host file."; return false;
    }
    const auto next = candidate(error);
    if (next.isEmpty()) return false;
    // Write the entry and data in place, then read the stored bytes back with mtools, a
    // reader of its own. Only a complete, verified candidate replaces the working revision.
    auto parts = pathParts(destination);
    const auto name = parts.takeLast();
    FatVolume volume(next, offset);
    FatVolume::Folder parent;
    bool ok = volume.open(error) && volume.find(parts, parent, error) && volume.addFile(parent, name, source, error);
    ok = ok && run("mcopy", next, {"::" + destination, verified}, output, error);
    if (ok && (digest(source).isEmpty() || digest(source) != digest(verified))) {
        error = "The card copy did not match the host file. The working image has not changed."; ok = false;
    }
    if (ok) adopt(next); else QFile::remove(next);
    return ok;
}

bool SdExchange::get(const QString& source, const QString& destination, QString& error) {
    if (current.isEmpty() || !validPath(source)) { error = "Enter a complete card filename."; return false; }
    QTemporaryDir transfer(directory.path() + "/transfer-XXXXXX");
    QString output;
    const auto staged = transfer.path() + "/output";
    if (!transfer.isValid()) { error = "Could not create transfer storage."; return false; }
    if (!run("mcopy", current, {"::" + source, staged}, output, error)) return false;
    // Qt's copy refuses existing files, including symlinks. No overwrite prompt
    // can grant permission to overwrite a host image, session or unrelated file.
    if (!QFile::copy(staged, destination)) { error = "Could not create the host file. Choose a new filename in a writable folder."; return false; }
    return true;
}

bool SdExchange::mkdir(const QString& path, QString& error) {
    if (current.isEmpty() || !validPath(path)) { error = "Enter a new absolute card folder path."; return false; }
    const auto next = candidate(error);
    if (next.isEmpty()) return false;
    QString output;
    auto parts = pathParts(path);
    const auto name = parts.takeLast();
    FatVolume volume(next, offset);
    FatVolume::Folder parent;
    if (!(volume.open(error) && volume.find(parts, parent, error) && volume.addFolder(parent, name, error)) ||
        !run("mdir", next, {"::" + path}, output, error)) { QFile::remove(next); return false; }
    adopt(next); return true;
}
