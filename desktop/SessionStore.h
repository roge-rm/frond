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

// The managed session folder. A saved session is <name>.frond, written by the core (session
// file version 6 starts with the magic, the version and the 32-byte device name). One that was
// created but not saved yet is <name>.frond.new, holding the device's fixed name. The name is
// the file name, so renaming a session renames the file.
namespace frondSessions {
constexpr quint32 magic = 0x19800819, newestVersion = 6, firstNamedVersion = 6;

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
inline QString filePath(const QString& folder, const QString& name) { return folder + '/' + name + ".frond"; }
inline QString markerPath(const QString& path) { return path + ".new"; }
inline bool taken(const QString& folder, const QString& name) {
    const auto path = filePath(folder, name);
    return QFileInfo::exists(path) || QFileInfo::exists(markerPath(path)) || QFileInfo::exists(preferencesPath(path));
}

// "Palm TX", then "Palm TX 2" and so on.
inline QString freeName(const QString& folder, const QString& base) {
    if (!taken(folder, base)) return base;
    int n = 2;
    while (taken(folder, base + ' ' + QString::number(n))) n++;
    return base + ' ' + QString::number(n);
}

inline Info describe(const QString& path) {
    Info info;
    info.path = path;
    info.name = QFileInfo(path).completeBaseName();
    QFileInfo file(path);
    if (file.isFile()) {
        const auto header = readHeader(path);
        info.saved = true; info.when = file.lastModified(); info.size = file.size();
        info.old = !openable(header);
        if (!info.old) info.device = header.device;
        return info;
    }
    QFile marker(markerPath(path));
    if (marker.open(QIODevice::ReadOnly)) {
        info.device = QString::fromUtf8(marker.readAll()).trimmed();
        info.when = QFileInfo(marker).lastModified();
    }
    info.old = frondFirmware::deviceFromId(info.device) == frondFirmware::Device::automatic;
    if (info.old) info.device.clear();
    return info;
}

// Newest first.
inline QList<Info> list(const QString& folder) {
    QList<Info> sessions;
    QStringList seen;
    for (const auto& entry : QDir(folder).entryInfoList({"*.frond", "*.frond.new"}, QDir::Files)) {
        auto path = entry.absoluteFilePath();
        if (path.endsWith(".new")) path.chop(4);
        if (seen.contains(path)) continue;
        seen << path;
        sessions << describe(path);
    }
    std::stable_sort(sessions.begin(), sessions.end(), [](const Info& a, const Info& b) { return a.when > b.when; });
    return sessions;
}

// A session that has not been saved yet. Returns false if the marker couldn't be written or already exists.
inline bool create(const QString& folder, const QString& name, const QString& deviceId) {
    if (!QDir().mkpath(folder)) return false;
    QFile marker(markerPath(filePath(folder, name)));
    if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return false;  // NewOnly: never takes a name another Frond holds
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

// True if another Frond has the session open.
inline bool inUse(const QString& path) {
    QLockFile lock(path + ".lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) return true;
    lock.unlock();
    return false;
}

// Returns an error message, or empty when done. The session's lock is held meanwhile, so another
// Frond can't open it half way.
inline QString rename(const QString& folder, const Info& info, const QString& wanted) {
    const auto name = cleanName(wanted);
    if (name.isEmpty()) return QStringLiteral("Type a name.");
    if (name == info.name) return {};
    if (taken(folder, name)) return QStringLiteral("There is already a session called %1.").arg(name);
    QLockFile lock(info.path + ".lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) return QStringLiteral("This session is open in another Frond.");
    const auto target = filePath(folder, name);
    QLockFile targetLock(target + ".lock"); targetLock.setStaleLockTime(0);
    if (!targetLock.tryLock()) return QStringLiteral("This session is open in another Frond.");
    const bool hasFile = QFileInfo::exists(info.path), hasMarker = QFileInfo::exists(markerPath(info.path));
    if (hasFile && !QFile::rename(info.path, target)) return QStringLiteral("Couldn't rename the session.");
    if (hasMarker && !QFile::rename(markerPath(info.path), markerPath(target))) {
        if (hasFile) QFile::rename(target, info.path);
        return QStringLiteral("Couldn't rename the session.");
    }
    if (QFileInfo::exists(preferencesPath(info.path)) && !QFile::rename(preferencesPath(info.path), preferencesPath(target))) {
        if (hasMarker) QFile::rename(markerPath(target), markerPath(info.path));
        if (hasFile) QFile::rename(target, info.path);
        return QStringLiteral("Couldn't rename the machine settings.");
    }
    return {};
}

inline QString remove(const Info& info) {
    QLockFile lock(info.path + ".lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) return QStringLiteral("This session is open in another Frond.");
    for (const auto& path : {info.path, markerPath(info.path), preferencesPath(info.path)})
        if (QFileInfo::exists(path) && !QFile::remove(path)) return QStringLiteral("Couldn't delete the session.");
    return {};  // the lock file goes when 'lock' unlocks
}

// Copies a session file in as a new session. Returns an error message, or empty and the new name.
inline QString importFile(const QString& folder, const QString& from, QString* newName = nullptr) {
    const auto header = readHeader(from);
    if (const auto error = importError(header); !error.isEmpty()) return error;
    if (!QDir().mkpath(folder)) return QStringLiteral("Couldn't save the session.");
    auto base = cleanName(QFileInfo(from).completeBaseName());
    if (base.isEmpty()) base = frondFirmware::deviceName(frondFirmware::deviceFromId(header.device));
    // The name is held with a marker while the file lands, so no other Frond takes it meanwhile.
    const auto name = reserve(folder, base, header.device);
    if (name.isEmpty()) return QStringLiteral("Couldn't save the session.");
    const auto target = filePath(folder, name);
    const auto fail = [&] {
        QFile::remove(target + ".part");
        QFile::remove(markerPath(target));
        return QStringLiteral("Couldn't save the session.");
    };
    // Copied to .part and renamed after the check, so a stopped import leaves no session behind.
    QFile::remove(target + ".part");
    if (!QFile::copy(from, target + ".part")) return fail();
    if (!QFile::rename(target + ".part", target)) return fail();
    Preferences prefs;
    if (readPreferences(from, &prefs)) {
        if (const auto error = writePreferences(target, prefs); !error.isEmpty()) {
            QFile::remove(target); return fail();
        }
    }
    QFile::remove(markerPath(target));
    if (newName) *newName = name;
    return {};
}

// Copies a saved session out; replaces the target. Refuses the session itself and any other
// session of the folder that is open. The copy goes to a .part file first, so a failure keeps the old target.
// Returns an error message or empty.
inline QString exportFile(const Info& info, const QString& to) {
    const auto source = QFileInfo(info.path).canonicalFilePath();
    const QFileInfo target(to);
    if (target.exists() && target.canonicalFilePath() == source) return QStringLiteral("That is the session itself.");
    if (target.suffix() == "frond" && QFileInfo::exists(to + ".lock") && inUse(to))
        return QStringLiteral("That session is open in another Frond.");
    QLockFile targetLock(to + ".lock"); targetLock.setStaleLockTime(0);
    if (!targetLock.tryLock()) return QStringLiteral("That session is open in another Frond.");
    const auto part = to + ".part";
    QFile::remove(part);
    if (!QFile::copy(info.path, part)) { QFile::remove(part); return QStringLiteral("Couldn't export the session."); }
    std::error_code error;
    std::filesystem::rename(part.toStdString(), to.toStdString(), error);  // replaces the target in one step
    if (error) { QFile::remove(part); return QStringLiteral("Couldn't export the session."); }
    Preferences prefs;
    if (readPreferences(info.path, &prefs)) return writePreferences(to, prefs);
    if (QFileInfo::exists(preferencesPath(to)) && !QFile::remove(preferencesPath(to)))
        return QStringLiteral("Couldn't remove the old machine settings.");
    return {};
}
}
#endif
