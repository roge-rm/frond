#ifndef FROND_SESSION_STORE_H
#define FROND_SESSION_STORE_H

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QRandomGenerator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QLockFile>
#include <QString>
#include <algorithm>
#include <filesystem>
#include "FirmwareInput.h"

// Each machine is a folder: sessions/<name>/ holds <name>.frond, <name>.frond.settings.json,
// the lock, sd.frondsd / ms.frondms, and <name>.frond.new until the first save.
namespace frondSessions {
constexpr quint32 magic = 0x19800819, newestVersion = 7, firstNamedVersion = 6;

// Host preferences stay beside the guest file, including before its first save.
struct Preferences {
    int scale{4};  // quarter steps
    bool smooth{false}, audio{true}, fastAudio{false}, microphone{false};
    bool showKeyboard{true}, showJogControls{true};
};
inline QString preferencesPath(const QString& path) { return path + ".settings.json"; }
inline bool readPreferences(const QString& path, Preferences* result) {
    QFile file(preferencesPath(path));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    if (object.value("version").toInt() != 1 || !object.value("scale").isDouble() ||
        !object.value("smooth").isBool() || !object.value("audio").isBool() || !object.value("fastAudio").isBool()) return false;
    const auto scale = object.value("scale").toInt();
    if (scale < 4 || scale > 8) return false;
    *result = {scale, object.value("smooth").toBool(), object.value("audio").toBool(), object.value("fastAudio").toBool(), object.value("microphone").toBool(false),
               object.value("showKeyboard").toBool(true), object.value("showJogControls").toBool(true)};
    return true;
}
// The caller holds the session lock, or has just reserved a new machine.
inline QString writePreferences(const QString& path, const Preferences& prefs) {
    QSaveFile file(preferencesPath(path));
    const QJsonObject object{{"version", 1}, {"scale", std::clamp(prefs.scale, 4, 8)},
                             {"smooth", prefs.smooth}, {"audio", prefs.audio}, {"fastAudio", prefs.fastAudio}, {"microphone", prefs.microphone},
                             {"showKeyboard", prefs.showKeyboard}, {"showJogControls", prefs.showJogControls}};
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return QStringLiteral("Couldn't save the machine settings.");
    return {};
}

struct Header {
    bool readable{false};
    quint32 magic{0}, version{0};
    QString device;  // the fixed name; empty before version 6
};

inline Header readHeader(const QString& path) {
    Header header;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return header;
    const auto bytes = file.read(8 + 32);
    header.readable = true;
    if (bytes.size() < 8) return header;
    auto at = [&](int i) {
        return quint32(uchar(bytes[i])) | (quint32(uchar(bytes[i + 1])) << 8) | (quint32(uchar(bytes[i + 2])) << 16) |
               (quint32(uchar(bytes[i + 3])) << 24);
    };
    header.magic = at(0); header.version = at(4);
    if (header.version >= firstNamedVersion && bytes.size() == 8 + 32)
        header.device = QString::fromLatin1(bytes.mid(8, 32).constData());  // stops at the first NUL
    return header;
}

// A session a version of Frond can open: right magic, a version with a device name, a device we run.
inline bool openable(const Header& header) {
    return header.readable && header.magic == magic && header.version >= firstNamedVersion &&
           header.version <= newestVersion &&
           frondFirmware::deviceFromId(header.device) != frondFirmware::Device::automatic;
}

// Why a picked file can't be imported; empty when it can.
inline QString importError(const Header& header) {
    if (!header.readable) return QStringLiteral("Couldn't read that file.");
    if (header.magic != magic) return QStringLiteral("That isn't a Frond session.");
    if (header.version > newestVersion) return QStringLiteral("That session is from a newer Frond.");
    if (header.version < firstNamedVersion) return QStringLiteral("That session is from an older Frond and can't be opened.");
    if (frondFirmware::deviceFromId(header.device) == frondFirmware::Device::automatic)
        return QStringLiteral("That session is for a device Frond can't run here yet.");
    return {};
}

inline quint32 le32(const QByteArray& bytes, int i) {
    return quint32(uchar(bytes[i])) | (quint32(uchar(bytes[i + 1])) << 8) |
           (quint32(uchar(bytes[i + 2])) << 16) | (quint32(uchar(bytes[i + 3])) << 24);
}
inline quint64 le64(const QByteArray& bytes, int i) {
    return quint64(le32(bytes, i)) | (quint64(le32(bytes, i + 4)) << 32);
}

// Header, size and magic of each v7 sibling. A stub shorter than the TOC is no media.
inline QString checkImportedSiblings(const QString& dir, const QString& frond) {
    const auto mismatch = QStringLiteral("That session's card files don't match.");
    QFile file(frond);
    if (!file.open(QIODevice::ReadOnly)) return QStringLiteral("Couldn't read that file.");
    const auto head = file.read(8 + 32);
    if (head.size() < 8) return QStringLiteral("Couldn't read that file.");
    if (le32(head, 4) != newestVersion) return {};
    struct Card {
        quint32 kind;
        quint64 physical;
    };
    QList<Card> cards;
    const auto toc = file.read(52);
    if (toc.size() == 52) {
        const auto mediaCount = le32(toc, 48);
        const qint64 descBytes = qint64(mediaCount) * 92;
        if (mediaCount && (descBytes / 92 != qint64(mediaCount) || file.size() < qint64(8 + 32 + 52) + descBytes))
            return mismatch;
        for (quint32 i = 0; i < mediaCount; ++i) {
            const auto desc = file.read(92);
            if (desc.size() != 92) return mismatch;
            const auto kind = le32(desc, 0);
            const auto name = QString::fromLatin1(desc.mid(4, 32).constData());
            const auto logical = le64(desc, 76);
            const auto physical = le64(desc, 84);
            if (kind == 1) {
                if (name != QLatin1String("sd.frondsd") || logical != physical || physical < 512 || physical % 512)
                    return mismatch;
            } else if (kind == 2) {
                if (name != QLatin1String("ms.frondms") || logical != (128ull << 20) || physical != 136577024ull)
                    return mismatch;
            } else {
                return mismatch;
            }
            cards.append({kind, physical});
        }
    }
    bool expectSd = false, expectMs = false;
    for (const auto& card : cards) {
        if (card.kind == 1) {
            if (expectSd) return mismatch;
            expectSd = true;
            QFile sd(dir + "/sd.frondsd");
            if (!sd.open(QIODevice::ReadOnly)) return mismatch;
            const auto hdr = sd.read(64);
            if (hdr.size() != 64 || hdr.left(8) != QByteArray("FRONDSD\0", 8)) return mismatch;
            if (le32(hdr, 8) != 1 || le32(hdr, 12) != 512) return mismatch;
            const auto payload = le64(hdr, 16);
            if (payload != card.physical || quint64(sd.size()) != 64 + payload) return mismatch;
            for (int i = 28; i < 64; ++i)
                if (hdr[i]) return mismatch;
        } else {
            if (expectMs) return mismatch;
            expectMs = true;
            QFile ms(dir + "/ms.frondms");
            if (!ms.open(QIODevice::ReadOnly)) return mismatch;
            const auto hdr = ms.read(64);
            if (hdr.size() != 64 || hdr.left(8) != QByteArray("FRONDMS\0", 8)) return mismatch;
            if (le32(hdr, 8) != 1 || le32(hdr, 12) != 8192 || le32(hdr, 16) != 32 || le32(hdr, 20) != 512 ||
                le32(hdr, 24) != 9)
                return mismatch;
            if (quint64(ms.size()) != 64 + card.physical) return mismatch;
        }
    }
    if (!expectSd && QFileInfo::exists(dir + "/sd.frondsd")) return mismatch;
    if (!expectMs && QFileInfo::exists(dir + "/ms.frondms")) return mismatch;
    return {};
}

struct Info {
    QString name, path;    // path is the .frond file, saved or not
    QString device;        // the fixed name; empty for one that can't be opened
    bool saved{false}, old{false};
    QDateTime when;        // the save time, or when it was created
    qint64 size{0};
};

inline QString cleanName(QString name) {
    name = name.trimmed();
    name.replace('/', '-').replace(QChar(0), '-');
    while (name.startsWith('.')) name.remove(0, 1);
    return name.trimmed();
}
inline bool ignoredName(const QString& name) {
    return name.contains(".frond.tmp-") || name.contains(".frondsd.tmp-") ||
           name.contains(".frondms.tmp-") || name.contains(".gen-");
}
inline bool packableName(const QString& name) {
    if (ignoredName(name) || name.contains('/')) return false;
    return name == "sd.frondsd" || name == "ms.frondms" || name.endsWith(".frond.settings.json") ||
           (name.endsWith(".frond") && !name.endsWith(".frond.new"));
}
inline bool tempName(const QString& name) {
    return name.contains(".frond.tmp-") || name.contains(".frondsd.tmp-") || name.contains(".frondms.tmp-") ||
           (name.startsWith(".gen-") && name.contains("-tmp-"));
}
// Files Frond keeps in a machine folder, saved or working.
inline bool machineFileName(const QString& name) {
    return packableName(name) || ignoredName(name) || name.endsWith(".frond.lock") || name.endsWith(".frond.new") ||
           name.endsWith(".frond.work");
}
inline QString packableDestName(const QString& name, const QString& sessionName) {
    if (name.endsWith(".frond.settings.json")) return sessionName + ".frond.settings.json";
    if (name.endsWith(".frond")) return sessionName + ".frond";
    return name;
}
inline QString machineDir(const QString& folder, const QString& name) { return folder + '/' + name; }
inline QString filePath(const QString& folder, const QString& name) { return machineDir(folder, name) + '/' + name + ".frond"; }
inline QString markerPath(const QString& path) { return path + ".new"; }
inline QString machineDirOf(const QString& path) { return QFileInfo(path).absolutePath(); }
inline QString machineNameOf(const QString& path) { return QFileInfo(machineDirOf(path)).fileName(); }

inline QString sessionFrondIn(const QString& dir) {
    const auto named = dir + '/' + QFileInfo(dir).fileName() + ".frond";
    if (QFileInfo::exists(named) || QFileInfo::exists(markerPath(named))) return named;
    const auto android = dir + "/session.frond";
    if (QFileInfo::exists(android)) return android;
    QString found;
    int count = 0;
    for (const auto& entry : QDir(dir).entryInfoList(QDir::Files)) {
        if (!packableName(entry.fileName()) || !entry.fileName().endsWith(".frond")) continue;
        found = entry.absoluteFilePath();
        ++count;
    }
    return count == 1 ? found : named;
}

inline bool taken(const QString& folder, const QString& name) {
    if (QFileInfo(machineDir(folder, name)).isDir()) return true;
    const auto loose = folder + '/' + name + ".frond";
    return QFileInfo::exists(loose) || QFileInfo::exists(markerPath(loose)) || QFileInfo::exists(preferencesPath(loose));
}

// "Palm TX", then "Palm TX 2" and so on.
inline QString freeName(const QString& folder, const QString& base) {
    if (!taken(folder, base)) return base;
    int n = 2;
    while (taken(folder, base + ' ' + QString::number(n))) n++;
    return base + ' ' + QString::number(n);
}

inline qint64 packableSize(const QString& dir) {
    qint64 n = 0;
    for (const auto& entry : QDir(dir).entryInfoList(QDir::Files))
        if (packableName(entry.fileName())) n += entry.size();
    return n;
}

inline QString resolveFrond(const QString& path) {
    if (QFileInfo(path).isDir()) return sessionFrondIn(path);
    return path;
}

inline Info describe(const QString& path) {
    Info info;
    const auto frond = resolveFrond(path);
    info.path = frond;
    info.name = QFileInfo(frond).completeBaseName();
    const auto parent = machineDirOf(frond);
    const auto parentName = QFileInfo(parent).fileName();
    if (QFileInfo(frond).fileName() == parentName + ".frond" || QFileInfo(frond).fileName() == "session.frond")
        info.name = parentName;
    QFileInfo file(frond);
    if (file.isFile()) {
        const auto header = readHeader(frond);
        info.saved = true; info.when = file.lastModified(); info.size = packableSize(machineDirOf(frond));
        info.old = !openable(header);
        if (!info.old) info.device = header.device;
        return info;
    }
    QFile marker(markerPath(frond));
    if (marker.open(QIODevice::ReadOnly)) {
        info.device = QString::fromUtf8(marker.readAll()).trimmed();
        info.when = QFileInfo(marker).lastModified();
    }
    info.old = frondFirmware::deviceFromId(info.device) == frondFirmware::Device::automatic;
    if (info.old) info.device.clear();
    return info;
}

inline bool inUse(const QString& path) {
    QLockFile lock(resolveFrond(path) + ".lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) return true;
    lock.unlock();
    return false;
}

// Deletes save temps that no running save owns: only in folders nobody has open, and only old ones.
inline void removeStaleTemps(const QString& dir) {
    if (inUse(sessionFrondIn(dir))) return;
    const auto cutoff = QDateTime::currentDateTime().addSecs(-10 * 60);
    for (const auto& entry : QDir(dir).entryInfoList(QDir::Files | QDir::Hidden | QDir::System))
        if (tempName(entry.fileName()) && entry.lastModified() < cutoff) QFile::remove(entry.absoluteFilePath());
}

inline QString workPath(const QString& path) { return path + ".work"; }

// A folder an outside session runs from is marked with <name>.frond.work until its first save.
// The marker is touched once the core has copied the cards in, so a card file newer than the
// marker holds the guest's writes.
inline void touchWork(const QString& frond) {
    QFile work(workPath(frond));
    if (work.open(QIODevice::ReadWrite)) work.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
}

// If Frond stopped before the first save, the folder goes once it is 10 minutes old, nobody
// has it open and its cards are still the copies. One whose cards changed stays and is listed,
// so it can be opened or deleted. True when it was removed.
inline bool removeStaleWork(const QString& frond) {
    const QFileInfo work(workPath(frond));
    if (!work.isFile()) return false;
    if (inUse(frond)) return false;
    if (QFileInfo(frond).isFile()) {
        QFile::remove(work.absoluteFilePath());
        return false;
    }
    if (work.lastModified() >= QDateTime::currentDateTime().addSecs(-10 * 60)) return false;
    const auto dir = machineDirOf(frond);
    QDir folder(dir);
    const QStringList markers{work.fileName(), QFileInfo(markerPath(frond)).fileName(),
                              QFileInfo(preferencesPath(frond)).fileName(), QFileInfo(frond).fileName() + ".lock"};
    for (const auto& entry : folder.entryInfoList(QDir::Files | QDir::Hidden | QDir::System))
        if (!markers.contains(entry.fileName()) && entry.lastModified() > work.lastModified()) return false;
    QFile::remove(markerPath(frond));
    QFile::remove(preferencesPath(frond));
    QFile::remove(work.absoluteFilePath());
    for (const auto& entry : folder.entryInfoList(QDir::Files | QDir::Hidden | QDir::System))
        if (machineFileName(entry.fileName())) QFile::remove(entry.absoluteFilePath());
    QDir().rmdir(dir);
    return true;
}

inline bool moveInto(const QString& from, const QString& to) {
    if (!QFileInfo::exists(from)) return true;
    if (QFileInfo::exists(to)) return true;
    return QFile::rename(from, to);
}

// Loose sessions/<name>.frond (and .new / .settings.json / .lock) become sessions/<name>/.
inline void migrateLoose(const QString& folder) {
    QDir dir(folder);
    QStringList stems;
    for (const auto& entry : dir.entryInfoList(QDir::Files)) {
        const auto name = entry.fileName();
        if (ignoredName(name)) continue;
        QString stem;
        if (name.endsWith(".frond.new")) stem = name.chopped(int(sizeof(".frond.new") - 1));
        else if (name.endsWith(".frond.settings.json")) stem = name.chopped(int(sizeof(".frond.settings.json") - 1));
        else if (name.endsWith(".frond.lock")) stem = name.chopped(int(sizeof(".frond.lock") - 1));
        else if (name.endsWith(".frond")) stem = name.chopped(int(sizeof(".frond") - 1));
        else continue;
        if (stem.isEmpty() || stems.contains(stem)) continue;
        stems << stem;
    }
    for (const auto& stem : stems) {
        const auto loose = folder + '/' + stem + ".frond";
        if (inUse(loose)) continue;
        const auto dest = machineDir(folder, stem);
        if (QFileInfo(dest).isFile()) continue;
        if (!QDir().mkpath(dest)) continue;
        moveInto(loose, dest + '/' + stem + ".frond");
        moveInto(markerPath(loose), dest + '/' + stem + ".frond.new");
        moveInto(preferencesPath(loose), dest + '/' + stem + ".frond.settings.json");
        moveInto(loose + ".lock", dest + '/' + stem + ".frond.lock");
    }
}

// Newest first.
inline QList<Info> list(const QString& folder) {
    migrateLoose(folder);
    QList<Info> sessions;
    for (const auto& entry : QDir(folder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (ignoredName(entry.fileName())) continue;
        const auto frond = sessionFrondIn(entry.absoluteFilePath());
        if (!QFileInfo::exists(frond) && !QFileInfo::exists(markerPath(frond))) continue;
        removeStaleTemps(entry.absoluteFilePath());
        if (removeStaleWork(frond)) continue;
        sessions << describe(frond);
    }
    std::stable_sort(sessions.begin(), sessions.end(), [](const Info& a, const Info& b) { return a.when > b.when; });
    return sessions;
}

// A session that has not been saved yet. Returns false if the marker couldn't be written or already exists.
inline bool create(const QString& folder, const QString& name, const QString& deviceId) {
    const auto dir = machineDir(folder, name);
    if (!QDir().mkpath(dir)) return false;
    QFile marker(markerPath(filePath(folder, name)));
    if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        QDir().rmdir(dir);
        return false;
    }
    return marker.write(deviceId.toUtf8()) >= 0;
}

// Takes the first free name starting at 'base' by writing its marker. Returns the name, or empty on failure.
inline QString reserve(const QString& folder, const QString& base, const QString& deviceId) {
    for (int tries = 0; tries < 100; tries++) {
        const auto name = freeName(folder, base);
        if (create(folder, name, deviceId)) return name;
        if (!QDir().mkpath(folder)) return {};
    }
    return {};
}

inline QString copyPackable(const QString& fromDir, const QString& toDir, const QString& newName) {
    if (!QDir().mkpath(toDir)) return QStringLiteral("Couldn't save the session.");
    bool any = false;
    for (const auto& entry : QDir(fromDir).entryInfoList(QDir::Files)) {
        if (!packableName(entry.fileName())) continue;
        const auto dest = toDir + '/' + packableDestName(entry.fileName(), newName);
        QFile::remove(dest);
        if (!QFile::copy(entry.absoluteFilePath(), dest)) return QStringLiteral("Couldn't save the session.");
        any = true;
    }
    if (!any) return QStringLiteral("Couldn't save the session.");
    // A card the source doesn't have must not stay behind in an existing folder.
    for (const auto* card : {"sd.frondsd", "ms.frondms"}) {
        if (QFileInfo::exists(fromDir + '/' + card)) continue;
        if (QFileInfo::exists(toDir + '/' + card) && !QFile::remove(toDir + '/' + card))
            return QStringLiteral("Couldn't save the session.");
    }
    return {};
}

inline QString locate(const QString& folder, const QString& path) {
    migrateLoose(folder);
    if (QFileInfo::exists(path) || QFileInfo::exists(markerPath(path))) return path;
    if (QFileInfo(path).isDir()) {
        const auto frond = sessionFrondIn(path);
        if (QFileInfo::exists(frond) || QFileInfo::exists(markerPath(frond))) return frond;
    }
    const auto name = QFileInfo(path).completeBaseName();
    const auto moved = filePath(folder, name);
    if (QFileInfo::exists(moved) || QFileInfo::exists(markerPath(moved))) return moved;
    return path;
}

inline QString rename(const QString& folder, const Info& info, const QString& wanted) {
    const auto name = cleanName(wanted);
    if (name.isEmpty()) return QStringLiteral("Type a name.");
    if (name == info.name) return {};
    if (taken(folder, name)) return QStringLiteral("There is already a session called %1.").arg(name);
    const auto oldDir = machineDirOf(info.path);
    const auto newDir = machineDir(folder, name);
    // Moves the files one by one into a new folder whose lock is taken first, so the session is
    // locked throughout and neither lock file is pulled out from under its QLockFile.
    QLockFile lock(info.path + ".lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) return QStringLiteral("This session is open in another Frond.");
    const auto oldFrond = QFileInfo(info.path).fileName();
    const auto newFrond = name + ".frond";
    if (!QDir().mkdir(newDir)) return QStringLiteral("Couldn't rename the session.");
    QLockFile newLock(newDir + '/' + newFrond + ".lock");
    newLock.setStaleLockTime(0);
    if (!newLock.tryLock()) {
        QDir().rmdir(newDir);
        return QStringLiteral("Couldn't rename the session.");
    }
    const auto renamed = [&](const QString& file) {
        for (const auto& suffix : {QString(), QStringLiteral(".new"), QStringLiteral(".settings.json"), QStringLiteral(".work")})
            if (file == oldFrond + suffix) return newFrond + suffix;
        return file;
    };
    QList<QPair<QString, QString>> moved;
    bool ok = true;
    for (const auto& entry : QDir(oldDir).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
        if (entry.fileName() == oldFrond + ".lock") continue;
        const auto to = newDir + '/' + renamed(entry.fileName());
        std::error_code error;
        std::filesystem::rename(entry.absoluteFilePath().toStdString(), to.toStdString(), error);
        if (error) { ok = false; break; }
        moved.append({entry.absoluteFilePath(), to});
    }
    if (!ok) {
        for (const auto& [from, to] : moved) {
            std::error_code back;
            std::filesystem::rename(to.toStdString(), from.toStdString(), back);
        }
        newLock.unlock();
        QDir().rmdir(newDir);
        return QStringLiteral("Couldn't rename the session.");
    }
    lock.unlock();
    QDir().rmdir(oldDir);
    return {};
}

inline QString remove(const Info& info) {
    QLockFile lock(info.path + ".lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) return QStringLiteral("This session is open in another Frond.");
    const auto dir = machineDirOf(info.path);
    if (QFileInfo(dir).isDir() && QFileInfo(dir).fileName() == info.name) {
        lock.unlock();
        if (!QDir(dir).removeRecursively()) return QStringLiteral("Couldn't delete the session.");
        return {};
    }
    for (const auto& path : {info.path, markerPath(info.path), preferencesPath(info.path)})
        if (QFileInfo::exists(path) && !QFile::remove(path)) return QStringLiteral("Couldn't delete the session.");
    return {};
}

inline QString importFolder(const QString& folder, const QString& from, QString* newName = nullptr) {
    const auto frond = sessionFrondIn(from);
    const auto header = readHeader(frond);
    if (const auto error = importError(header); !error.isEmpty()) return error;
    if (!QDir().mkpath(folder)) return QStringLiteral("Couldn't save the session.");
    auto base = cleanName(QFileInfo(from).fileName());
    if (base.isEmpty()) base = frondFirmware::deviceName(frondFirmware::deviceFromId(header.device));
    const auto name = reserve(folder, base, header.device);
    if (name.isEmpty()) return QStringLiteral("Couldn't save the session.");
    const auto dest = machineDir(folder, name);
    QFile::remove(markerPath(filePath(folder, name)));
    if (const auto error = copyPackable(from, dest, name); !error.isEmpty()) {
        QDir(dest).removeRecursively();
        return error;
    }
    if (const auto error = checkImportedSiblings(dest, dest + '/' + name + ".frond"); !error.isEmpty()) {
        QDir(dest).removeRecursively();
        return error;
    }
    if (newName) *newName = name;
    return {};
}

// Copies a session file or machine folder in as a new session. Returns an error message, or empty and the new name.
inline QString importFile(const QString& folder, const QString& from, QString* newName = nullptr) {
    if (QFileInfo(from).isDir()) return importFolder(folder, from, newName);
    const auto header = readHeader(from);
    if (const auto error = importError(header); !error.isEmpty()) return error;
    if (!QDir().mkpath(folder)) return QStringLiteral("Couldn't save the session.");
    auto base = cleanName(QFileInfo(from).completeBaseName());
    if (base.isEmpty()) base = frondFirmware::deviceName(frondFirmware::deviceFromId(header.device));
    const auto name = reserve(folder, base, header.device);
    if (name.isEmpty()) return QStringLiteral("Couldn't save the session.");
    const auto destDir = machineDir(folder, name);
    const auto target = filePath(folder, name);
    const auto fail = [&] {
        QDir(destDir).removeRecursively();
        return QStringLiteral("Couldn't save the session.");
    };
    QFile::remove(markerPath(target));
    const auto part = target + ".part";
    QFile::remove(part);
    if (!QFile::copy(from, part) || !QFile::rename(part, target)) return fail();
    const auto sourceDir = QFileInfo(from).absolutePath();
    for (const auto* sibling : {"sd.frondsd", "ms.frondms"}) {
        const auto src = sourceDir + '/' + sibling;
        if (QFileInfo::exists(src) && !QFile::copy(src, destDir + '/' + sibling)) return fail();
    }
    if (const auto error = checkImportedSiblings(destDir, target); !error.isEmpty()) {
        QDir(destDir).removeRecursively();
        return error;
    }
    Preferences prefs;
    if (readPreferences(from, &prefs)) {
        if (const auto error = writePreferences(target, prefs); !error.isEmpty()) return fail();
    }
    if (newName) *newName = name;
    return {};
}

// A folder holding one real session, named <folder>.frond or session.frond, and only that
// session's own files: its settings, lock, cards and their temps and generations.
inline bool isMachineFolder(const QString& dir) {
    const QFileInfo info(dir);
    if (!info.isDir() || info.isSymLink()) return false;
    const auto entries = QDir(dir).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    QString stem;
    for (const auto& candidate : {info.fileName(), QStringLiteral("session")}) {
        const auto file = dir + '/' + candidate + ".frond";
        const QFileInfo session(file);
        if (!session.isFile() || session.isSymLink()) continue;
        const auto header = readHeader(file);
        if (!header.readable || header.magic != magic) return false;
        stem = candidate;
        break;
    }
    if (stem.isEmpty()) return false;
    for (const auto& entry : entries) {
        const auto name = entry.fileName();
        if (!entry.isFile() || entry.isSymLink() || !machineFileName(name)) return false;
        if (ignoredName(name) || name == "sd.frondsd" || name == "ms.frondms") continue;
        // Every .frond, settings, lock or marker file has to belong to the one session.
        const auto own = stem + ".frond";
        if (name != own && name != own + ".settings.json" && name != own + ".lock" && name != own + ".new" && name != own + ".work")
            return false;
    }
    return true;
}

// Deletes a machine folder file by file, so anything that isn't Frond's stays, and the folder with it.
inline bool removeMachineFolder(const QString& dir) {
    QDir folder(dir);
    for (const auto& entry : folder.entryInfoList(QDir::Files | QDir::Hidden | QDir::System))
        if (machineFileName(entry.fileName())) QFile::remove(entry.absoluteFilePath());
    return QDir().rmdir(dir);
}

inline bool emptyFolder(const QString& dir) {
    const QFileInfo info(dir);
    return info.isDir() && !info.isSymLink() &&
           QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty();
}

// Why `dir` can't be written as a machine folder; empty when it can. *existing is set when it
// is a session that would be replaced. An empty folder becomes the new session's folder.
inline QString exportTargetError(const QString& dir, bool* existing = nullptr) {
    if (existing) *existing = false;
    const QFileInfo info(dir);
    if (!info.exists() && !info.isSymLink()) return {};
    if (emptyFolder(dir)) return {};
    if (!isMachineFolder(dir))
        return QStringLiteral("There is already something called %1 there that isn't a Frond session.").arg(info.fileName());
    if (existing) *existing = true;
    return {};
}

// Where publishStaged keeps the session it replaces until the copy is in place. The ".old"
// suffix keeps it apart from the staged copy, which is named like the destination.
inline QString stagedOriginal(const QString& staging, const QString& dest) {
    return staging + '/' + QFileInfo(dest).fileName() + ".old";
}

// Deletes what this code put in a staging folder, then the folder. A replaced session still
// waiting in it is never deleted, so the folder stays.
inline void removeStaging(const QString& staging) {
    if (staging.isEmpty()) return;
    for (const auto& entry : QDir(staging).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden))
        if (!entry.fileName().endsWith(".old")) removeMachineFolder(entry.absoluteFilePath());
    removeMachineFolder(staging);
    QDir().rmdir(staging);
}

inline QDateTime newestIn(const QString& dir) {
    auto newest = QFileInfo(dir).lastModified();
    QDirIterator it(dir, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        newest = std::max(newest, it.fileInfo().lastModified());
    }
    return newest;
}

// Staging folders for `dest` left by a Frond that stopped mid-copy: once nothing in them has
// changed for 10 minutes and `dest` isn't open, a replaced session goes back if `dest` is gone,
// and the rest is deleted. A replaced session is never deleted here.
inline void removeStaleStaging(const QString& dest) {
    const QFileInfo info(dest);
    const auto destFrond = dest + '/' + info.fileName() + ".frond";
    if (QFileInfo::exists(destFrond + ".lock") && inUse(destFrond)) return;
    const auto cutoff = QDateTime::currentDateTime().addSecs(-10 * 60);
    const auto prefix = '.' + info.fileName() + ".frond.tmp-";
    for (const auto& entry : QDir(info.absolutePath()).entryInfoList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)) {
        if (!entry.fileName().startsWith(prefix) || entry.isSymLink()) continue;
        const auto staging = entry.absoluteFilePath();
        if (newestIn(staging) >= cutoff) continue;
        const auto original = stagedOriginal(staging, dest);
        if (QFileInfo(original).isDir() && !QFileInfo::exists(dest) && !QFileInfo(dest).isSymLink()) {
            std::error_code error;
            std::filesystem::rename(original.toStdString(), dest.toStdString(), error);
        }
        removeStaging(staging);
    }
}

// A new hidden folder beside `dest` that holds the copy until it is complete.
inline QString makeStaging(const QString& dest) {
    const QFileInfo info(dest);
    removeStaleStaging(dest);
    for (int tries = 0; tries < 100; tries++) {
        QString suffix;
        for (int i = 0; i < 6; i++) suffix += QChar('a' + int(QRandomGenerator::global()->bounded(26)));
        const auto staging = info.absolutePath() + "/." + info.fileName() + ".frond.tmp-" + suffix;
        if (QFileInfo::exists(staging)) continue;
        if (QDir().mkdir(staging)) return staging;
        if (!QDir().mkpath(info.absolutePath())) return {};
    }
    return {};
}

// Moves the finished copy staging/<name> to `dest`. An existing session at `dest` moves into the
// staging folder first and is deleted only once the copy is in place. If it can't be put back
// after a failed swap it stays in the staging folder and the message says where.
inline QString publishStaged(const QString& staging, const QString& dest) {
    const auto staged = staging + '/' + QFileInfo(dest).fileName();
    const auto name = QFileInfo(dest).fileName();
    std::error_code error;
    if (emptyFolder(dest) && !QDir().rmdir(dest)) return QStringLiteral("Couldn't save the session.");
    if (QFileInfo::exists(dest) || QFileInfo(dest).isSymLink()) {
        if (!isMachineFolder(dest)) return QStringLiteral("There is already something called %1 there that isn't a Frond session.").arg(name);
        const auto old = stagedOriginal(staging, dest);
        std::filesystem::rename(dest.toStdString(), old.toStdString(), error);
        if (error) return QStringLiteral("Couldn't replace %1.").arg(name);
        std::filesystem::rename(staged.toStdString(), dest.toStdString(), error);
        if (error) {
            std::error_code back;
            std::filesystem::rename(old.toStdString(), dest.toStdString(), back);
            if (back) return QStringLiteral("Couldn't replace %1. The original session is now in %2.").arg(name, QDir::toNativeSeparators(old));
            return QStringLiteral("Couldn't replace %1.").arg(name);
        }
        removeMachineFolder(old);
    } else {
        std::filesystem::rename(staged.toStdString(), dest.toStdString(), error);
        if (error) return QStringLiteral("Couldn't save the session.");
    }
    removeStaging(staging);
    return {};
}

inline QString exportDestDir(const QString& to) {
    const QFileInfo target(to);
    // <name>/<name>.frond is the machine folder itself.
    if (target.suffix() == "frond" && QFileInfo(target.absolutePath()).fileName() == target.completeBaseName())
        return target.absolutePath();
    if (target.suffix() == "frond") return target.absolutePath() + '/' + target.completeBaseName();
    return target.absoluteFilePath();
}

// Copies packable files out as a machine folder. `to` is a directory, or a .frond path whose stem
// is the folder. Only a folder that is already a Frond session is replaced.
inline QString exportFile(const Info& info, const QString& to) {
    const auto destDir = exportDestDir(to);
    const auto destName = QFileInfo(destDir).fileName();
    const auto sourceDir = machineDirOf(info.path);
    const auto sourceCanon = QFileInfo(sourceDir).canonicalFilePath();
    const auto sourceFrond = QFileInfo(info.path).canonicalFilePath();
    const QFileInfo toInfo(to);
    if (toInfo.exists() && !sourceFrond.isEmpty() && toInfo.canonicalFilePath() == sourceFrond)
        return QStringLiteral("That is the session itself.");
    const QFileInfo destInfo(destDir);
    if (destInfo.exists() && destInfo.canonicalFilePath() == sourceCanon) return QStringLiteral("That is the session itself.");
    if (const auto error = exportTargetError(destDir); !error.isEmpty()) return error;
    const auto destFrond = destDir + '/' + destName + ".frond";
    if (QFileInfo::exists(destFrond + ".lock") && inUse(destFrond))
        return QStringLiteral("That session is open in another Frond.");
    QLockFile targetLock(destFrond + ".lock"); targetLock.setStaleLockTime(0);
    if (isMachineFolder(destDir) && !targetLock.tryLock()) return QStringLiteral("That session is open in another Frond.");
    const auto staging = makeStaging(destDir);
    if (staging.isEmpty()) return QStringLiteral("Couldn't export the session.");
    const auto part = staging + '/' + destName;
    if (const auto error = copyPackable(sourceDir, part, destName); !error.isEmpty()) {
        removeStaging(staging);
        return error;
    }
    if (QFileInfo::exists(preferencesPath(info.path))) {
        Preferences prefs;
        if (readPreferences(info.path, &prefs)) {
            if (const auto error = writePreferences(part + '/' + destName + ".frond", prefs); !error.isEmpty()) {
                removeStaging(staging);
                return error;
            }
        }
    }
    if (const auto error = publishStaged(staging, destDir); !error.isEmpty()) {
        removeStaging(staging);
        return error;
    }
    return {};
}

}
#endif
