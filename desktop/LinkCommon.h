// Shared by the link hub, the windows and the tests: where the hub and its store live, the
// address rule for peers, and the connection choices a machine's peripheral pickers offer.
#pragma once
#include <QByteArray>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QString>
#include <QVector>
#include <algorithm>

namespace frondLink {

constexpr quint16 defaultPort = 5035;

// <runtime>/frond, mode 0700: the hub socket and the cores' link sockets. Empty if it can't be made.
inline QString runtimeFolder() {
    auto base = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (base.isEmpty()) base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (base.isEmpty()) return {};
    const auto folder = base + "/frond";
    if (!QDir().mkpath(folder)) return {};
    QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QFileInfo info(folder);
    if (!info.isDir() || info.permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
                                                QFile::ReadOther | QFile::WriteOther | QFile::ExeOther)) return {};
    return folder;
}
// FROND_LINK_SOCKET picks another hub, as tests and a second hub on one desktop do.
inline QString hubSocketPath() {
    const auto custom = qEnvironmentVariable("FROND_LINK_SOCKET");
    if (!custom.isEmpty()) return custom;
    const auto folder = runtimeFolder();
    return folder.isEmpty() ? QString() : folder + "/link.sock";
}
// The core's IR socket for one window; the hub connects to it.
inline QString coreSocketPath(const QString& kind, const QString& tag) {
    const auto folder = runtimeFolder();
    return folder.isEmpty() ? QString() : folder + "/" + kind + "-" + tag + ".sock";
}
inline QString storeFolder() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/frond/connections";
}

// Peers may only be on the local network: loopback, private, link-local and ULA addresses.
inline bool allowedPeer(const QHostAddress& address) {
    bool v4 = false;
    const quint32 a = address.toIPv4Address(&v4);
    if (v4) {
        return (a >> 24) == 127 || (a >> 24) == 10 || (a >> 20) == (172u << 4 | 1) || (a >> 16) == (192u << 8 | 168) ||
               (a >> 16) == (169u << 8 | 254);
    }
    if (address.protocol() != QAbstractSocket::IPv6Protocol) return false;
    const auto b = address.toIPv6Address();
    if (address.isLoopback()) return true;
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return true;  // fe80::/10
    return (b[0] & 0xfe) == 0xfc;                             // fc00::/7
}

struct Choice {
    QString id, name;
};
// The paired connections by name, read from the hub's store. Only the hub writes it.
inline QVector<Choice> pairedConnections() {
    QVector<Choice> result;
    QFile file(storeFolder() + "/connections.json");
    if (!file.open(QIODevice::ReadOnly)) return result;
    const auto entries = QJsonDocument::fromJson(file.readAll()).object().value("entries").toArray();
    for (const auto& value : entries) {
        const auto entry = value.toObject();
        if (!entry.value("paired").toBool(true)) continue;
        result.push_back({entry.value("id").toString(), entry.value("name").toString()});
    }
    return result;
}
inline QString connectionName(const QString& id) {
    if (id == QLatin1String("local")) return QCoreApplication::translate("frondLink", "This computer");
    for (const auto& c : pairedConnections()) if (c.id == id) return c.name;
    return {};
}

// Puts the connections into a picker after its local choices: a separator, This computer, then
// the paired ones by name, with item data link:local and link:<id>. 'current' is kept as
// "Unknown connection" if it names one that's gone. Bluetooth has no This computer: machines on
// one desktop share This Frond's radio.
inline void addConnectionChoices(QComboBox* combo, const QString& current, bool thisComputer = true) {
    for (int i = combo->count() - 1; i >= 0; --i)
        if (combo->itemData(i).toString().startsWith("link:") || combo->itemText(i).isEmpty()) combo->removeItem(i);
    const auto paired = pairedConnections();
    const bool unknown = current.startsWith("link:") &&
                         std::none_of(paired.begin(), paired.end(), [&](const Choice& c) { return "link:" + c.id == current; }) &&
                         !(thisComputer && current == QLatin1String("link:local"));
    if (!thisComputer && paired.isEmpty() && !unknown) return;
    combo->insertSeparator(combo->count());
    if (thisComputer) combo->addItem(QCoreApplication::translate("frondLink", "This computer"), "link:local");
    for (const auto& c : paired) combo->addItem(c.name, "link:" + c.id);
    if (current.startsWith("link:") && combo->findData(current) < 0)
        combo->addItem(QCoreApplication::translate("frondLink", "Unknown connection"), current);
}
// Shows the Modem choice (data "modem") after Wait for a peer on a serial picker only when the
// device has a modem.
inline void setModemChoice(QComboBox* combo, bool present) {
    const int at = combo->findData("modem");
    if (!present && at >= 0) combo->removeItem(at);
    if (present && at < 0) combo->insertItem(std::max(0, combo->findData("tcp")) + 1, QCoreApplication::translate("frondLink", "Modem"), "modem");
}
// A link: setting whose connection is gone acts as Off.
inline bool knownLink(const QString& value) {
    if (!value.startsWith("link:")) return false;
    const auto id = value.mid(5);
    return id == QLatin1String("local") || !connectionName(id).isEmpty();
}

// One JSON object per line on the hub socket.
inline QByteArray encodeLine(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

}  // namespace frondLink
