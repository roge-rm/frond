// The hub's store in <data>/frond/connections: identity.json (this install's id, name, X25519
// secret and TCP port) and connections.json (paired peers with pinned keys). Both 0600, written
// only by the hub while it holds connections/.lock.
#pragma once
#include <QLockFile>
#include <QString>
#include <QVector>
#include <memory>
#include "../link/pairing.h"

namespace frondLink {

QString hexId(const frond::link::InstallId& id);
bool parseId(const QString& text, frond::link::InstallId& out);

struct Peer {
    frond::link::InstallId id{};
    QString name;      // shown here, renameable
    QString peerName;  // what the peer calls itself
    frond::link::Key32 key{};
    QString address;   // host or host:port typed in Address…, else empty
    // Where a handshake we dialled last succeeded. Beacons never change it, so a spoofed one
    // can't take every dial away from a working address.
    QString goodHost;
    quint16 goodPort{0};
    bool paired{true};
    qint64 lastSeen{0};  // ms since the epoch, from beacons and links
    QString bluetoothRadio{"here"};
    bool keyChanged{false};  // its handshake failed against the pinned key: pair again
    QString id_() const { return hexId(id); }
};

class Store {
 public:
    // Takes the lock and loads or makes the identity. Returns an error, or empty.
    QString open(const QString& folder);
    bool save();
    bool saveIdentity();
    const frond::link::LocalInfo& local() const { return info; }
    QString name() const { return QString::fromStdString(info.name); }
    void setName(const QString& name);
    quint16 port{defaultPortValue};
    QVector<Peer> peers;
    Peer* find(const frond::link::InstallId& id);
    Peer* find(const QString& hex);

 private:
    static constexpr quint16 defaultPortValue = 5035;
    QString folder;
    std::unique_ptr<QLockFile> lock;
    frond::link::LocalInfo info;
};

}  // namespace frondLink
