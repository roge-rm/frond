#include "LinkStore.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSysInfo>
#include "../link/crypto.h"

namespace frondLink {
using namespace frond::link;

QString hexId(const InstallId& id) {
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(id.data()), int(id.size())).toHex());
}
bool parseId(const QString& text, InstallId& out) {
    const auto bytes = QByteArray::fromHex(text.toLatin1());
    if (bytes.size() != int(out.size()) || bytes.toHex() != text.toLatin1().toLower()) return false;
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return true;
}
template <size_t N>
static bool parseHex(const QString& text, std::array<uint8_t, N>& out) {
    const auto bytes = QByteArray::fromHex(text.toLatin1());
    if (bytes.size() != int(N)) return false;
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return true;
}
template <size_t N>
static QString toHex(const std::array<uint8_t, N>& a) {
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(a.data()), int(N)).toHex());
}

static bool writePrivate(const QString& path, const QJsonObject& object) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    const auto bytes = QJsonDocument(object).toJson();
    return file.write(bytes) == bytes.size() && file.commit() &&
           QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
}

QString Store::open(const QString& where) {
    folder = where;
    if (!QDir().mkpath(folder)) return QStringLiteral("Couldn't make %1.").arg(folder);
    QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    lock = std::make_unique<QLockFile>(folder + "/.lock");
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) return QStringLiteral("Another link service uses %1.").arg(folder);
    QFile identity(folder + "/identity.json");
    bool loaded = false;
    if (identity.open(QIODevice::ReadOnly)) {
        const auto o = QJsonDocument::fromJson(identity.readAll()).object();
        Key32 secret{};
        if (o.value("version").toInt() == 1 && parseId(o.value("id").toString(), info.id) &&
            parseHex(o.value("secret").toString(), secret) && identityFromSecret(secret, info.identity)) {
            info.name = o.value("name").toString().toStdString();
            port = quint16(o.value("port").toInt(defaultPortValue));
            loaded = true;
        }
        wipe(secret.data(), secret.size());
    }
    if (!loaded) {
        if (identity.exists() && !QFile::rename(identity.fileName(), identity.fileName() + ".bad")) QFile::remove(identity.fileName());
        info.identity = makeIdentity();
        randomBytes(info.id.data(), info.id.size());
        auto host = QSysInfo::machineHostName();
        if (host.isEmpty()) host = QStringLiteral("Frond");
        info.name = host.toStdString();
        if (!saveIdentity()) return QStringLiteral("Couldn't save %1.").arg(identity.fileName());
    }
    QFile list(folder + "/connections.json");
    if (list.open(QIODevice::ReadOnly)) {
        const auto o = QJsonDocument::fromJson(list.readAll()).object();
        for (const auto& value : o.value("entries").toArray()) {
            const auto e = value.toObject();
            Peer p;
            if (!parseId(e.value("id").toString(), p.id) || !parseHex(e.value("key").toString(), p.key)) continue;
            p.name = e.value("name").toString();
            p.peerName = e.value("peerName").toString();
            p.address = e.value("address").toString();
            p.goodHost = e.value("lastHost").toString();
            p.goodPort = quint16(e.value("lastPort").toInt());
            p.paired = e.value("paired").toBool(true);
            p.lastSeen = qint64(e.value("lastSeen").toDouble());
            p.bluetoothRadio = e.value("bluetoothRadio").toString("here") == QLatin1String("there") ? "there" : "here";
            p.bluetoothRadioAt = qint64(e.value("bluetoothRadioAt").toDouble());
            p.keyChanged = e.value("keyChanged").toBool(false);
            if (p.name.isEmpty()) p.name = p.peerName;
            peers.push_back(p);
        }
    }
    return {};
}
bool Store::saveIdentity() {
    return writePrivate(folder + "/identity.json",
                        QJsonObject{{"version", 1}, {"id", hexId(info.id)}, {"name", name()},
                                    {"secret", toHex(info.identity.secret)}, {"port", int(port)}});
}
bool Store::save() {
    QJsonArray entries;
    for (const auto& p : peers)
        entries.append(QJsonObject{{"id", hexId(p.id)}, {"name", p.name}, {"peerName", p.peerName}, {"key", toHex(p.key)},
                                   {"address", p.address}, {"manual", !p.address.isEmpty()}, {"paired", p.paired},
                                   {"lastSeen", double(p.lastSeen)}, {"bluetoothRadio", p.bluetoothRadio},
                                   {"bluetoothRadioAt", double(p.bluetoothRadioAt)},
                                   {"keyChanged", p.keyChanged}, {"lastHost", p.goodHost}, {"lastPort", int(p.goodPort)}});
    return writePrivate(folder + "/connections.json", QJsonObject{{"version", 1}, {"entries", entries}});
}
void Store::setName(const QString& name) {
    info.name = name.toStdString();
    saveIdentity();
}
Peer* Store::find(const InstallId& id) {
    for (auto& p : peers) if (p.id == id) return &p;
    return nullptr;
}
Peer* Store::find(const QString& hex) {
    InstallId id{};
    return parseId(hex, id) ? find(id) : nullptr;
}

}  // namespace frondLink
