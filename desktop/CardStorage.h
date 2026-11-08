#pragma once
// The storage library's CardFile and FatImage (build/source/src/common/storage) for the desktop:
// QString names and paths, QDateTime dates, and the library's errors as frondCardFile::Error.
// The crash safety, journals and FAT code are all the library's.
#include "storage/CardFile.h"
#include "storage/FatImage.h"

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTime>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace frondCardFile {

// A failed card change or read. `what()` is for logs; `message` is shown.
class Error : public std::exception {
 public:
    explicit Error(QString text) : message(std::move(text)), utf8(message.toUtf8()) {}
    const char* what() const noexcept override { return utf8.constData(); }
    QString message;
 private:
    QByteArray utf8;
};
// A change stopped by its caller; what was done stays, the rest is rolled back.
class Cancelled : public Error {
 public:
    Cancelled() : Error(QStringLiteral("Cancelled.")) {}
};

inline QString inUseMessage() { return QString::fromUtf8(frondStorage::inUseMessage); }
inline QString damagedMessage() { return QString::fromUtf8(frondStorage::damagedMessage); }
inline QString inFilesMessage() { return QString::fromUtf8(frondStorage::inFilesMessage); }

inline std::filesystem::path pathOf(const QString& file) { return std::filesystem::path(QFile::encodeName(file).toStdString()); }
inline std::string utf8Of(const QString& s) { return s.toStdString(); }
inline QString textOf(const std::string& s) { return QString::fromStdString(s); }

// Runs `f`, turning the library's errors into ours. Ours, thrown from callbacks, pass as they are.
template <class F>
auto translated(F&& f) -> decltype(f()) {
    try {
        return f();
    } catch (const frondStorage::Cancelled&) {
        throw Cancelled();
    } catch (const frondStorage::Error& e) {
        throw Error(textOf(e.message));
    }
}

// Whether a cut-off change is waiting beside the card file.
inline bool unsettled(const QString& card) { return frondStorage::unsettled(pathOf(card)); }
// Whether a card file is open in a CardFile now (this process or another).
inline bool inUse(const QString& card) { return frondStorage::inUse(pathOf(card)); }
// Finishes a cut-off change, if there is one. False when the card is open in a CardFile now, or
// can't be written; true when there was nothing to do or it's done.
inline bool settle(const QString& card) { return frondStorage::settleCard(pathOf(card)); }
// A card file nobody has open in Files…, with any change a crash cut off finished.
inline bool ready(const QString& card) { return frondStorage::ready(pathOf(card)); }
// As ready(), with why not: empty when ready, else the message to show.
inline QString notReady(const QString& card) {
    const auto r = frondStorage::readiness(pathOf(card));
    return r.ok ? QString() : textOf(r.message);
}
// What the core's load checks: the header and every CRC. The generation, or nothing.
inline std::optional<quint64> verify(const QString& card) {
    const auto g = frondStorage::verifiedGeneration(pathOf(card));
    return g ? std::optional<quint64>(quint64(*g)) : std::nullopt;
}
// A v2 card file of `payload` (whole sectors) at `generation`, as the card tool makes one.
inline QByteArray encodeV2(const QByteArray& payload, quint64 generation) {
    std::vector<uint8_t> file;
    if (!frondSdFile::encodeV2(reinterpret_cast<const uint8_t*>(payload.constData()), uint64_t(payload.size()), generation, file))
        return {};
    return QByteArray(reinterpret_cast<const char*>(file.data()), int(file.size()));
}

using Blocks = frondStorage::Blocks;

class CardFile : public frondStorage::CardFile {
 public:
    // Opens a card file for a visit, settling a cut-off change first. Throws Error(inUseMessage())
    // when another CardFile (this process or another) has it.
    CardFile(const QString& card, bool writable) try : frondStorage::CardFile(pathOf(card), writable) {
    } catch (const frondStorage::Error& e) {
        throw Error(textOf(e.message));
    }
    static std::unique_ptr<CardFile> open(const QString& card, bool writable) { return std::make_unique<CardFile>(card, writable); }
    qint64 payloadBytes() const { return qint64(frondStorage::CardFile::payloadBytes()); }
    quint64 generation() const { return quint64(frondStorage::CardFile::generation()); }
};

// A raw card image, for the SD exchange's private copies: writes go straight in, and a commit
// writes its sectors and syncs. The caller keeps the image's earlier copy until a change is checked.
class RawBlocks : public frondStorage::RawBlocks {
 public:
    RawBlocks(const QString& image, bool writable) try : frondStorage::RawBlocks(pathOf(image), writable) {
    } catch (const frondStorage::Error& e) {
        throw Error(textOf(e.message));
    }
};

struct Entry {
    QString name, shortName;
    bool directory{false};
    qint64 size{0};
    quint32 firstCluster{0};
    QDateTime modified;  // invalid when the card has none
    frondStorage::Entry raw;
};

class FatImage {
    frondStorage::FatImage fat;

 public:
    // Reads the card's layout. Throws Error.
    explicit FatImage(Blocks& blocks) try : fat(blocks), bytesWritten(fat.bytesWritten), committed(fat.committed) {
    } catch (const frondStorage::Error& e) {
        throw Error(textOf(e.message));
    }

    qint64 clusterBytes() const { return qint64(fat.clusterBytes()); }
    qint64 freeBytes() const { return qint64(fat.freeBytes()); }
    qint64 totalBytes() const { return qint64(fat.totalBytes()); }
    int fatBits() const { return fat.fatBits(); }
    // Bytes written to the card, and whether any change was committed.
    const int64_t& bytesWritten;
    const bool& committed;

    // A fresh FAT12 or FAT16 file system over the whole card.
    static void format(Blocks& blocks) { translated([&] { frondStorage::FatImage::format(blocks); }); }

    // A folder, "/" or "/PALM/Launcher": folders first, then files, by name.
    QList<Entry> list(const QString& path) {
        return translated([&] {
            QList<Entry> out;
            for (const auto& e : fat.list(utf8Of(path))) out.append(entryOf(e));
            return out;
        });
    }
    std::optional<Entry> find(const QString& path) {
        return translated([&]() -> std::optional<Entry> {
            const auto e = fat.find(utf8Of(path));
            if (!e) return std::nullopt;
            return entryOf(*e);
        });
    }
    // The file's bytes, a cluster at a time.
    void read(const Entry& entry, const std::function<void(const char*, qint64)>& out) {
        translated([&] { fat.read(entry.raw, [&](const char* b, int64_t n) { out(b, qint64(n)); }); });
    }
    // A new file of `size` bytes from `input` (reads up to n bytes, returns how many, 0 at the end).
    void add(const QString& path, const QString& name, const std::function<qint64(char*, qint64)>& input, qint64 size) {
        translated([&] {
            fat.add(utf8Of(path), utf8Of(name), [&](char* b, int64_t n) { return int64_t(input(b, qint64(n))); }, int64_t(size));
        });
    }
    void makeFolder(const QString& path, const QString& name) { translated([&] { fat.makeFolder(utf8Of(path), utf8Of(name)); }); }
    // A file, or a folder with everything in it.
    void remove(const QString& path) { translated([&] { fat.remove(utf8Of(path)); }); }
    // Into toFolder, under newName when given (a rename when the folder is the same).
    void move(const QString& from, const QString& toFolder, const QString& newName = {}) {
        translated([&] { fat.move(utf8Of(from), utf8Of(toFolder), utf8Of(newName)); });
    }
    // A file or folder into toFolder under newName (or its own). `bytes` gets the amount so far.
    void copy(const QString& from, const QString& toFolder, const QString& newName = {}, const std::function<bool()>& cancelled = {},
              const std::function<void(qint64)>& bytes = {}) {
        std::function<void(int64_t)> progress;
        if (bytes) progress = [&](int64_t n) { bytes(qint64(n)); };
        translated([&] { fat.copy(utf8Of(from), utf8Of(toFolder), utf8Of(newName), cancelled, progress); });
    }
    // Runs `work` (an add, copy or move that makes an entry of the victim's name) and removes
    // `victim` in the same commit. A cancel, a failure or a crash leaves the old one.
    void replace(const QString& victim, const std::function<void()>& work) { translated([&] { fat.replace(utf8Of(victim), work); }); }
    // The bytes in a file, or in every file under a folder.
    qint64 sizeOf(const QString& path) { return translated([&] { return qint64(fat.sizeOf(utf8Of(path))); }); }

    // Whether every FAT copy matches the first, and the problems a check as fsck would do it finds.
    bool fatCopiesMatch() { return translated([&] { return fat.fatCopiesMatch(); }); }
    QStringList check() {
        return translated([&] {
            QStringList out;
            for (const auto& p : fat.check()) out << textOf(p);
            return out;
        });
    }

    static QStringList parts(const QString& path) { return path.split('/', Qt::SkipEmptyParts); }
    static QString joined(const QString& folder, const QString& name) {
        return folder == QStringLiteral("/") || folder.isEmpty() ? '/' + name : folder + '/' + name;
    }
    // Whether `inner` is `outer` or below it.
    static bool within(const QString& outer, const QString& inner) {
        const auto o = parts(outer.toLower()), i = parts(inner.toLower());
        return i.size() >= o.size() && i.mid(0, o.size()) == o;
    }
    // Throws when FAT can't hold the name.
    static void checkName(const QString& name) { translated([&] { frondStorage::fat::checkName(utf8Of(name)); }); }

 private:
    static QDateTime dateOf(frondStorage::fat::Stamp s) {
        if (s.date == 0) return {};
        const QDate d(1980 + (s.date >> 9), (s.date >> 5) & 15, s.date & 31);
        const QTime t(s.time >> 11, (s.time >> 5) & 63, (s.time & 31) * 2);
        if (!d.isValid()) return {};
        return QDateTime(d, t.isValid() ? t : QTime(0, 0));
    }
    static Entry entryOf(const frondStorage::Entry& e) {
        return Entry{textOf(e.name), textOf(e.shortName), e.directory, qint64(e.size), e.firstCluster, dateOf(e.modified), e};
    }
};

}  // namespace frondCardFile
