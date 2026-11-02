#ifndef FROND_SESSION_STORE_H
#define FROND_SESSION_STORE_H

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
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
    QLockFile lock(info.path + ".lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) return QStringLiteral("This session is open in another Frond.");
    lock.unlock();
    if (!QDir().rename(oldDir, newDir)) return QStringLiteral("Couldn't rename the session.");
    const auto oldFrond = QFileInfo(info.path).fileName();
    const auto newFrond = name + ".frond";
    if (oldFrond != newFrond) {
        QFile::rename(newDir + '/' + oldFrond, newDir + '/' + newFrond);
        QFile::rename(newDir + '/' + oldFrond + ".new", newDir + '/' + newFrond + ".new");
        QFile::rename(newDir + '/' + oldFrond + ".settings.json", newDir + '/' + newFrond + ".settings.json");
        QFile::rename(newDir + '/' + oldFrond + ".lock", newDir + '/' + newFrond + ".lock");
    }
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
    Preferences prefs;
    if (readPreferences(from, &prefs)) {
        if (const auto error = writePreferences(target, prefs); !error.isEmpty()) return fail();
    }
    if (newName) *newName = name;
    return {};
}

inline QString exportDestDir(const QString& to) {
    const QFileInfo target(to);
    if (target.suffix() == "frond") return target.absolutePath() + '/' + target.completeBaseName();
    return target.absoluteFilePath();
}

// Copies packable files out as a machine folder. `to` is a directory, or a .frond path whose stem is the folder.
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
    const auto destFrond = destDir + '/' + destName + ".frond";
    if (QFileInfo::exists(destFrond + ".lock") && inUse(destFrond))
        return QStringLiteral("That session is open in another Frond.");
    QLockFile targetLock(destFrond + ".lock"); targetLock.setStaleLockTime(0);
    if (QFileInfo::exists(destDir) && !targetLock.tryLock()) return QStringLiteral("That session is open in another Frond.");
    const auto part = destDir + ".part";
    QDir(part).removeRecursively();
    if (const auto error = copyPackable(sourceDir, part, destName); !error.isEmpty()) {
        QDir(part).removeRecursively();
        return error;
    }
    if (QFileInfo::exists(preferencesPath(info.path))) {
        Preferences prefs;
        if (readPreferences(info.path, &prefs)) {
            if (const auto error = writePreferences(part + '/' + destName + ".frond", prefs); !error.isEmpty()) {
                QDir(part).removeRecursively();
                return error;
            }
        }
    }
    std::error_code error;
    if (QFileInfo::exists(destDir)) {
        const auto old = destDir + ".old";
        QDir(old).removeRecursively();
        std::filesystem::rename(destDir.toStdString(), old.toStdString(), error);
        if (error) { QDir(part).removeRecursively(); return QStringLiteral("Couldn't export the session."); }
        std::filesystem::rename(part.toStdString(), destDir.toStdString(), error);
        if (error) {
            std::error_code back;
            std::filesystem::rename(old.toStdString(), destDir.toStdString(), back);
            QDir(part).removeRecursively();
            return QStringLiteral("Couldn't export the session.");
        }
        QDir(old).removeRecursively();
    } else {
        std::filesystem::rename(part.toStdString(), destDir.toStdString(), error);
        if (error) { QDir(part).removeRecursively(); return QStringLiteral("Couldn't export the session."); }
    }
    return {};
}
}
#endif
