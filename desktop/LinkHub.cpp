#include "LinkHub.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QNetworkInterface>
#include <QNetworkDatagram>
#include <cstdio>
#include <algorithm>

namespace frondLink {
using namespace frond::link;

namespace {
constexpr qint64 kBackoff[] = {1000, 2000, 5000, 10000, 30000};
constexpr qint64 kBeaconMs = 3000;
constexpr qint64 kNearbyMs = 10000;
constexpr qint64 kFirstFrameMs = 10000;
constexpr qint64 kLinkIdleMs = 15000;
constexpr qint64 kClosingMs = 3000;   // a closing connection's last bytes get this long to go
constexpr qint64 kCoreRetryMs = 250;
constexpr qint64 kBusyRetryMs = 5000;
constexpr qint64 kPairCooldownMs = 60000;
constexpr int kMaxPending = 16;       // connections not yet authenticated
constexpr int kPerAddress = 10;       // new connections per address in 10 s
const QByteArray kBeaconMagic("FRLB");

Bytes toBytes(const QByteArray& a) { return Bytes(a.begin(), a.end()); }
QByteArray fromBytes(const Bytes& b) { return QByteArray(reinterpret_cast<const char*>(b.data()), int(b.size())); }
qint64 wall() { return QDateTime::currentMSecsSinceEpoch(); }
QString pairResultName(PairResult r) {
    switch (r) {
        case PairResult::Paired: return "paired";
        case PairResult::WrongCode: return "wrongcode";
        case PairResult::Expired: return "expired";
        case PairResult::LockedOut: return "lockedout";
        case PairResult::Version: return "version";
        case PairResult::Timeout: return "timeout";
        default: return "failed";
    }
}
}  // namespace

struct Hub::Client {
    QLocalSocket* socket{nullptr};
    QByteArray input;
    bool watch{false};
};

struct Hub::Conn {
    enum class Kind { Unknown, Link, PairIn, PairOut };
    QTcpSocket* socket{nullptr};
    Kind kind{Kind::Unknown};
    bool outgoing{false};
    QByteArray pre;
    std::unique_ptr<Link> link;
    std::unique_ptr<Pairing> pairing;
    QString peer;  // hex id once known
    qint64 created{0};
    qint64 closingAt{0};
    bool closing{false};
    bool up{false};
    bool gotAnswer{false};  // PairOut: the peer's name came back
};

struct Hub::Attach {
    Client* client{nullptr};
    QString machine, device, link, socketPath;
    QLocalSocket* core{nullptr};
    bool coreUp{false};
    qint64 nextCoreTry{0};
    enum class St { Idle, Opening, Waiting, Joined, Busy } st{St::Idle};
    uint16_t ch{0};
    Conn* chLink{nullptr};
    QString farMachine, farDevice, busyBy;
    PeerState farState{PeerState::Running};
    Attach* partner{nullptr};
    qint64 retryAt{0};
    QJsonObject lastStatus;
};

Hub::Hub(QObject* parent) : QObject(parent) { clock.start(); }
Hub::~Hub() {
    for (auto* c : conns) {
        if (c->link && c->socket->state() == QAbstractSocket::ConnectedState) {
            c->link->close();
            const auto out = c->link->takeOutput();
            c->socket->write(fromBytes(out));
            c->socket->waitForBytesWritten(200);
        }
    }
}

LinkConfig Hub::config() const {
    LinkConfig cfg;
    cfg.local = store.local();
    return cfg;
}

QString Hub::start() {
    const auto path = hubSocketPath();
    if (path.isEmpty()) return QStringLiteral("No private runtime folder for the link service.");
    if (const auto error = store.open(storeFolder()); !error.isEmpty()) return error;
    {
        QLocalSocket probe;
        probe.connectToServer(path);
        if (probe.waitForConnected(500)) return QStringLiteral("A link service already runs on %1.").arg(path);
    }
    QLocalServer::removeServer(path);
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(path)) return server.errorString();
    connect(&server, &QLocalServer::newConnection, this, &Hub::newClient);
    bool ok = false;
    const int envPort = qEnvironmentVariableIntValue("FROND_LINK_PORT", &ok);
    const quint16 wantPort = ok ? quint16(envPort) : store.port;
    if (!listener.listen(QHostAddress::Any, wantPort) && !listener.listen(QHostAddress::Any, 0))
        return listener.errorString();
    listener.setMaxPendingConnections(kMaxPending);
    connect(&listener, &QTcpServer::newConnection, this, &Hub::incoming);
    const int envDiscovery = qEnvironmentVariableIntValue("FROND_LINK_DISCOVERY_PORT", &ok);
    discoveryPort = ok ? quint16(envDiscovery) : defaultPort;
    // Test hooks: the address the beacon socket binds, and host:port targets instead of broadcast.
    const QHostAddress bindTo(qEnvironmentVariable("FROND_LINK_DISCOVERY_BIND", "0.0.0.0"));
    for (const auto& item : qEnvironmentVariable("FROND_LINK_BEACON_TO").split(',', Qt::SkipEmptyParts)) {
        const QHostAddress host(item.section(':', 0, 0));
        const auto port = quint16(item.section(':', 1).toUInt());
        if (!host.isNull() && port) beaconTargets.push_back({host, port});
    }
    if (discoveryPort && udp.bind(bindTo.isNull() ? QHostAddress(QHostAddress::AnyIPv4) : bindTo, discoveryPort,
                                  QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint))
        connect(&udp, &QUdpSocket::readyRead, this, &Hub::datagrams);
    if (const int idle = qEnvironmentVariableIntValue("FROND_LINK_IDLE_EXIT_MS", &ok); ok && idle > 0) idleExitMs = idle;
    // Test hook: a shorter life for the code shown while someone pairs with us.
    if (const int life = qEnvironmentVariableIntValue("FROND_LINK_PAIR_CODE_MS", &ok); ok && life > 0)
        pairCodeMs = std::min<qint64>(life, qint64(PairCode::kLifetimeMs));
    timer.setInterval(100);
    connect(&timer, &QTimer::timeout, this, &Hub::tick);
    timer.start();
    idleSince = now();
    return {};
}

// ---- Clients ----

void Hub::newClient() {
    while (auto* s = server.nextPendingConnection()) {
        auto* c = new Client;
        c->socket = s;
        clients.push_back(c);
        connect(s, &QLocalSocket::readyRead, this, [this, c] { clientInput(c); });
        connect(s, &QLocalSocket::disconnected, this, [this, c] { dropClient(c); });
        sendTo(c, QJsonObject{{"type", "hello"}, {"id", hexId(store.local().id)}, {"name", store.name()},
                              {"port", int(listener.serverPort())}, {"pid", double(QCoreApplication::applicationPid())}});
    }
}
void Hub::clientInput(Client* c) {
    c->input += c->socket->readAll();
    if (c->input.size() > 1 << 20) { c->socket->abort(); return; }
    for (int at; (at = c->input.indexOf('\n')) >= 0;) {
        const auto line = c->input.left(at);
        c->input.remove(0, at + 1);
        const auto doc = QJsonDocument::fromJson(line);
        if (doc.isObject()) handle(c, doc.object());
        if (!clients.contains(c)) return;
    }
}
void Hub::sendTo(Client* c, const QJsonObject& o) {
    if (c && c->socket->state() == QLocalSocket::ConnectedState) c->socket->write(encodeLine(o));
}
void Hub::broadcastWatchers(const QJsonObject& o) {
    for (auto* c : clients) if (c->watch) sendTo(c, o);
}
int Hub::watchers() const {
    return int(std::count_if(clients.begin(), clients.end(), [](const Client* c) { return c->watch; }));
}
void Hub::dropClient(Client* c) {
    if (!clients.removeOne(c)) return;
    for (auto* a : QList<Attach*>(attaches)) {
        if (a->client != c) continue;
        unjoin(a, true);
        attaches.removeOne(a);
        if (a->core) a->core->deleteLater();
        const auto hex = a->link;
        delete a;
        if (hex == "local") matchLocal(); else matchPeer(hex);
    }
    if (pairOut.client == c) {
        if (pairOut.conn) closeConn(pairOut.conn, false);
        pairOut = {};
    }
    if (c->watch && !watchers() && code) endPairRequest("cancelled");
    c->socket->deleteLater();
    delete c;
    if (clients.isEmpty()) idleSince = now();
}

void Hub::handle(Client* c, const QJsonObject& op) {
    const auto type = op.value("op").toString();
    if (type == "watch") {
        c->watch = op.value("on").toBool(true);
        if (c->watch) { sendTo(c, snapshot()); }
        else if (!watchers() && code) endPairRequest("cancelled");
    } else if (type == "state") {
        sendTo(c, snapshot());
    } else if (type == "rename") {
        const auto name = op.value("name").toString().trimmed().left(64);
        if (!name.isEmpty()) store.setName(name);
    } else if (type == "peer-rename" || type == "peer-address" || type == "forget") {
        auto* p = store.find(op.value("id").toString());
        if (!p) return;
        const auto hex = hexId(p->id);
        if (type == "peer-rename") {
            const auto name = op.value("name").toString().trimmed().left(64);
            if (!name.isEmpty()) p->name = name;
        } else if (type == "peer-address") {
            p->address = op.value("address").toString().trimmed();
            auto& r = runs[hex];
            r.nextTry = now(); r.backoff = 0; r.failed = false;
        } else {
            auto& r = runs[hex];
            for (auto* conn : {r.link, r.dialing}) if (conn) closeConn(conn);
            runs.remove(hex);
            store.peers.erase(std::remove_if(store.peers.begin(), store.peers.end(),
                                             [&](const Peer& q) { return hexId(q.id) == hex; }), store.peers.end());
        }
        store.save();
    } else if (type == "pair") {
        if (pairOut.conn) closeConn(pairOut.conn, false);
        pairOut = {};
        pairOut.client = c;
        if (op.contains("id")) {
            const auto it = nearby.find(op.value("id").toString());
            if (it == nearby.end()) { sendTo(c, {{"type", "pair-result"}, {"result", "unreachable"}}); pairOut = {}; return; }
            pairOut.host = it->address.toString();
            pairOut.port = it->port;
            pairOut.peerName = it->name;
        } else {
            auto address = op.value("address").toString().trimmed();
            pairOut.typedAddress = address;
            quint16 port = quint16(op.value("port").toInt(defaultPort));
            // host, host:port, [v6]:port
            if (address.startsWith('[') && address.contains("]:")) {
                port = quint16(address.section("]:", 1).toUInt());
                address = address.section("]:", 0, 0).mid(1);
            } else if (address.count(':') == 1) {
                port = quint16(address.section(':', 1).toUInt());
                address = address.section(':', 0, 0);
            }
            pairOut.host = address;
            pairOut.port = port ? port : defaultPort;
        }
        pairOutStart();
    } else if (type == "pair-code") {
        if (pairOut.client != c) return;
        pairOut.code = op.value("code").toString();
        if (pairOut.conn && pairOut.conn->gotAnswer && !pairOut.conn->pairing) {
            pairOut.conn->pairing = std::make_unique<Pairing>(store.local(), pairOut.code.toStdString(), uint64_t(now()));
            pairOut.conn->pairing->start();
            pump(pairOut.conn);
        } else if (!pairOut.conn) pairOutStart();
    } else if (type == "pair-cancel") {
        if (pairOut.client != c) return;
        if (pairOut.conn) closeConn(pairOut.conn, false);
        pairOut = {};
    } else if (type == "pair-dismiss") {
        if (code) endPairRequest("cancelled");
    } else if (type == "ir-attach") {
        for (auto* a : attaches) if (a->client == c) return;  // one per window
        auto* a = new Attach;
        a->client = c;
        a->machine = op.value("machine").toString();
        a->device = op.value("device").toString();
        a->link = op.value("link").toString();
        a->socketPath = op.value("socket").toString();
        attaches.push_back(a);
        coreConnect(a);
        if (a->link == "local") matchLocal();
        else {
            auto& r = runs[a->link];
            if (!r.link && !r.dialing) { r.nextTry = now(); r.backoff = 0; }
            matchPeer(a->link);
        }
    } else if (type == "ir-detach") {
        for (auto* a : QList<Attach*>(attaches)) {
            if (a->client != c) continue;
            unjoin(a, true);
            attaches.removeOne(a);
            if (a->core) { a->core->disconnect(this); a->core->abort(); a->core->deleteLater(); }
            const auto hex = a->link;
            delete a;
            if (hex == "local") matchLocal(); else matchPeer(hex);
        }
    }
}

// ---- TCP ----

Hub::Conn* Hub::makeConn(QTcpSocket* s, bool outgoing) {
    auto* c = new Conn;
    c->socket = s;
    c->outgoing = outgoing;
    c->created = now();
    conns.push_back(c);
    s->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    s->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, kSendBuffer);
    connect(s, &QTcpSocket::readyRead, this, [this, c] { if (conns.contains(c)) connInput(c); });
    connect(s, &QTcpSocket::bytesWritten, this, [this, c] { if (conns.contains(c)) pump(c); });
    connect(s, &QTcpSocket::disconnected, this, [this, c] { if (conns.contains(c)) removeConn(c); });
    connect(s, &QTcpSocket::errorOccurred, this, [this, c](QAbstractSocket::SocketError) {
        if (!conns.contains(c)) return;
        if (c->socket->state() != QAbstractSocket::ConnectedState) removeConn(c);
    });
    return c;
}

void Hub::incoming() {
    while (auto* s = listener.nextPendingConnection()) {
        const auto from = s->peerAddress();
        if (!allowedPeer(from)) { s->abort(); s->deleteLater(); continue; }
        auto& times = recent[from.toString()];
        const auto t = now();
        times.erase(std::remove_if(times.begin(), times.end(), [&](qint64 v) { return t - v > 10000; }), times.end());
        const int pending = int(std::count_if(conns.begin(), conns.end(), [](const Conn* c) {
            return !c->outgoing && !c->up && c->kind != Conn::Kind::PairIn; }));
        if (times.size() >= kPerAddress || pending >= kMaxPending) {
            s->abort(); s->deleteLater(); continue;
        }
        times.push_back(t);
        makeConn(s, false);
    }
}

void Hub::connInput(Conn* c) {
    const auto data = c->socket->readAll();
    if (c->closing) return;
    switch (c->kind) {
        case Conn::Kind::Unknown: {
            c->pre += data;
            const auto* p = reinterpret_cast<const uint8_t*>(c->pre.constData());
            switch (peekFirstFrame(p, size_t(c->pre.size()))) {
                case FirstFrame::NeedMore:
                    if (c->pre.size() > 4096) closeConn(c, false);
                    return;
                case FirstFrame::Noise: {
                    c->kind = Conn::Kind::Link;
                    c->link = std::make_unique<Link>(config(), [this](const InstallId& id, Key32& key) {
                        auto* peer = store.find(id);
                        if (!peer || !peer->paired) return false;
                        key = peer->key;
                        return true;
                    }, uint64_t(now()));
                    const auto pre = std::exchange(c->pre, {});
                    c->link->receive(reinterpret_cast<const uint8_t*>(pre.constData()), size_t(pre.size()), uint64_t(now()));
                    afterLink(c);
                    return;
                }
                case FirstFrame::Pair: {
                    if (c->pre.size() < 2) return;
                    const int len = getU16(p);
                    if (c->pre.size() < 2 + len) return;
                    const auto first = c->pre.mid(2, len), rest = c->pre.mid(2 + len);
                    c->pre.clear();
                    pairFirst(c, first, rest);
                    return;
                }
                case FirstFrame::BadVersion: {
                    const auto f = refuseFrame(RefuseReason::Version);
                    c->socket->write(fromBytes(f));
                    closeConn(c);
                    return;
                }
                case FirstFrame::Garbage:
                    closeConn(c, false);
                    return;
            }
            return;
        }
        case Conn::Kind::Link:
            c->link->receive(reinterpret_cast<const uint8_t*>(data.constData()), size_t(data.size()), uint64_t(now()));
            afterLink(c);
            return;
        case Conn::Kind::PairIn:
        case Conn::Kind::PairOut:
            pairInput(c, data);
            return;
    }
}

void Hub::afterLink(Conn* c) {
    if (!conns.contains(c) || !c->link) return;
    auto& link = *c->link;
    if (c->peer.isEmpty() && link.state() != LinkState::Handshake) c->peer = hexId(link.peerId());
    if (link.state() == LinkState::Up && !c->up) {
        c->up = true;
        linkUp(c);
        if (!conns.contains(c)) return;
    }
    if (c->up) muxEvents(c);
    if (!conns.contains(c)) return;
    pump(c);
    if (link.state() == LinkState::Down && !c->closing) {
        const auto hex = c->peer.isEmpty() ? hexId(link.peerId()) : c->peer;
        auto* p = store.find(hex);
        // A key change is only shown, on either side, until a good link or pairing, and not while
        // a link with that peer is up: a refuse frame or a bad handshake message can come from
        // anyone on the LAN, so nothing here is stored and the dialling side keeps trying.
        if (link.error() == LinkError::KeyChanged && p) {
            auto& r = runs[hex];
            if (!(r.link && r.link != c && r.link->up)) r.keyMismatch = true;
        }
        if (link.error() == LinkError::UnknownPeer && p && c->outgoing) runs[hex].forgotten = true;
        closeConn(c);
    }
}

void Hub::linkUp(Conn* c) {
    auto* p = store.find(c->peer);
    if (!p) { closeConn(c); return; }
    auto& r = runs[c->peer];
    if (r.link && r.link != c) {
        // One link per pair of installs: keep the one dialled by the lower id; from the same side, the newer.
        const auto initNew = c->outgoing ? store.local().id : p->id;
        const auto initOld = r.link->outgoing ? store.local().id : p->id;
        if (!(initNew == initOld || initNew < initOld)) { closeConn(c); return; }
        Conn* old = r.link;
        r.link = nullptr;
        linkDown(old);
        closeConn(old);
    }
    if (r.dialing == c) r.dialing = nullptr;
    r.link = c;
    r.backoff = 0; r.failed = false; r.forgotten = false; r.keyMismatch = false; r.idleSince = now();
    p->lastSeen = wall();
    p->keyChanged = false;
    if (p->peerName != QString::fromStdString(c->link->peerName())) p->peerName = QString::fromStdString(c->link->peerName());
    if (c->outgoing) { p->goodHost = c->socket->peerAddress().toString(); p->goodPort = c->socket->peerPort(); }
    store.save();
    matchPeer(c->peer);
}

// The link went: its channels are gone, the machines on it wait again.
void Hub::linkDown(Conn* c) {
    for (auto* a : attaches) {
        if (a->chLink != c) continue;
        a->st = Attach::St::Idle; a->ch = 0; a->chLink = nullptr; a->farMachine.clear(); a->farDevice.clear();
        a->farState = PeerState::Running;
    }
    if (!c->peer.isEmpty() && runs.contains(c->peer)) {
        auto& r = runs[c->peer];
        if (r.link == c) { r.link = nullptr; r.held.clear(); r.nextTry = now() + kBackoff[0]; }
        if (r.dialing == c) r.dialing = nullptr;
    }
}

void Hub::pump(Conn* c) {
    if (!conns.contains(c) || c->socket->state() != QAbstractSocket::ConnectedState) return;
    while (c->socket->bytesToWrite() < kWriteAhead) {
        Bytes out;
        if (c->link && c->link->hasOutput()) out = c->link->takeOutput(size_t(kWriteAhead));
        else if (c->pairing) out = c->pairing->takeOutput();
        if (out.empty()) break;
        c->socket->write(fromBytes(out));
    }
    if (c->closing && c->socket->bytesToWrite() == 0 && !(c->link && c->link->hasOutput())) c->socket->disconnectFromHost();
}

void Hub::closeConn(Conn* c, bool flush) {
    if (!conns.contains(c)) return;
    if (c->up) { c->up = false; linkDown(c); if (!c->peer.isEmpty()) matchPeer(c->peer); }
    if (c->link && c->link->state() != LinkState::Down) c->link->close();
    if (pairOut.conn == c) pairOut.conn = nullptr;
    if (!c->closing) c->closingAt = now();
    c->closing = true;
    if (flush && c->socket->state() == QAbstractSocket::ConnectedState) {
        pump(c);
        if (conns.contains(c) && c->socket->state() == QAbstractSocket::ConnectedState) c->socket->disconnectFromHost();
        // A peer that never reads gets dropped by the tick after kClosingMs.
    } else {
        removeConn(c);
    }
}

void Hub::removeConn(Conn* c) {
    if (!conns.removeOne(c)) return;
    if (c->up) { c->up = false; linkDown(c); }
    if (!c->peer.isEmpty() && runs.contains(c->peer)) {
        auto& r = runs[c->peer];
        if (r.dialing == c) { r.dialing = nullptr; dialFailed(c); }
        if (r.link == c) r.link = nullptr;
        matchPeer(c->peer);
    }
    if (pairOut.conn == c) {
        pairOut.conn = nullptr;
        if (c->kind == Conn::Kind::PairOut && !c->gotAnswer && c->pairing == nullptr)
            sendTo(pairOut.client, {{"type", "pair-result"}, {"result", "unreachable"}});
    }
    c->socket->disconnect(this);
    c->socket->abort();
    c->socket->deleteLater();
    delete c;
}

// Where to dial a peer, tried in turn: the address the last beacon gave, the last address a
// handshake succeeded at and the typed address, each once.
QList<QPair<QString, quint16>> Hub::dialTargets(const QString& hex) const {
    QList<QPair<QString, quint16>> targets;
    const auto* p = const_cast<Store&>(store).find(hex);
    if (!p) return targets;
    const auto add = [&](const QString& host, quint16 port) {
        if (!host.isEmpty() && port && !targets.contains(qMakePair(host, port))) targets.push_back({host, port});
    };
    if (const auto it = runs.find(hex); it != runs.end() && !it->seen.isNull()) add(it->seen.toString(), it->seenPort);
    add(p->goodHost, p->goodPort);
    if (!p->address.isEmpty()) {
        auto host = p->address;
        quint16 port = defaultPort;
        if (host.startsWith('[') && host.contains("]:")) { port = quint16(host.section("]:", 1).toUInt()); host = host.section("]:", 0, 0).mid(1); }
        else if (host.count(':') == 1) { port = quint16(host.section(':', 1).toUInt()); host = host.section(':', 0, 0); }
        add(host, port ? port : defaultPort);
    }
    return targets;
}

void Hub::dial(const QString& hex) {
    auto* p = store.find(hex);
    if (!p) return;
    auto& r = runs[hex];
    const auto targets = dialTargets(hex);
    if (targets.isEmpty()) { r.failed = true; r.nextTry = now() + kBackoff[std::min(r.backoff++, 4)]; return; }
    const auto target = targets[r.target % targets.size()];
    auto* s = new QTcpSocket(this);
    auto* c = makeConn(s, true);
    c->kind = Conn::Kind::Link;
    c->peer = hex;
    c->link = std::make_unique<Link>(config(), p->id, p->key, uint64_t(now()));
    c->link->start();
    r.dialing = c;
    connect(s, &QTcpSocket::connected, this, [this, c] {
        if (!conns.contains(c)) return;
        c->socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        c->socket->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, kSendBuffer);
        pump(c);
    });
    s->connectToHost(target.first, target.second);
}

// Tries the next address at once, and backs off after a round of them.
void Hub::dialFailed(Conn* c) {
    auto& r = runs[c->peer];
    const int count = int(dialTargets(c->peer).size());
    ++r.target;
    if (count > 1 && r.target % count != 0) { r.nextTry = now(); return; }
    r.failed = true;
    r.nextTry = now() + kBackoff[std::min(r.backoff++, 4)];
}

// ---- Pairing ----
// Before CPace, A sends a request frame (header, 0, idA, name) and B answers with its own id
// and name, or a Refuse frame if its Connections window isn't open. B shows the code then.

void Hub::pairFirst(Conn* c, const QByteArray& first, const QByteArray& rest) {
    Reader r(reinterpret_cast<const uint8_t*>(first.constData()), size_t(first.size()));
    r.take(kHeaderLen);
    const auto step = r.u8();
    const auto id = r.array<16>();
    const auto name = QString::fromStdString(r.str8()).left(64);
    const bool same = code && id == requester;
    if (step != 0 || !r.ok() || !watchers() || now() < pairCooldown || (code && !same) || id == store.local().id) {
        c->socket->write(fromBytes(refuseFrame(RefuseReason::NotPairing)));
        closeConn(c);
        return;
    }
    if (!code) {
        code = std::make_unique<PairCode>(uint64_t(now()));
        codeAt = now();
        requester = id;
        requesterName = name.isEmpty() ? QStringLiteral("Frond") : name;
        auto shown = QString::fromStdString(code->code());
        broadcastWatchers({{"type", "pair-request"}, {"name", requesterName}, {"code", shown.left(3) + " " + shown.mid(3)}});
    }
    c->kind = Conn::Kind::PairIn;
    Bytes answer;
    putHeader(answer, FrameMode::Pair);
    putU8(answer, 0);
    putBytes(answer, store.local().id);
    putStr8(answer, store.local().name);
    Bytes frame;
    writeFrame(frame, answer);
    c->socket->write(fromBytes(frame));
    c->pairing = std::make_unique<Pairing>(store.local(), *code, uint64_t(now()));
    if (!rest.isEmpty()) pairInput(c, rest);
}

void Hub::pairInput(Conn* c, const QByteArray& data) {
    if (c->kind == Conn::Kind::PairOut && !c->gotAnswer) {
        c->pre += data;
        if (c->pre.size() < 2) return;
        const int len = getU16(reinterpret_cast<const uint8_t*>(c->pre.constData()));
        if (c->pre.size() < 2 + len) return;
        const auto body = toBytes(c->pre.mid(2, len));
        const auto rest = c->pre.mid(2 + len);
        c->pre.clear();
        uint8_t version; FrameMode mode;
        if (!parseHeader(body, version, mode) || mode != FrameMode::Pair || body.size() < kHeaderLen + 1 || body[kHeaderLen] != 0) {
            const bool refused = parseHeader(body, version, mode) && mode == FrameMode::Refuse;
            const auto why = refused && body.size() > kHeaderLen ? RefuseReason(body[kHeaderLen]) : RefuseReason::Protocol;
            sendTo(pairOut.client, {{"type", "pair-result"}, {"result", why == RefuseReason::NotPairing ? "notpairing" :
                                                                        why == RefuseReason::Version ? "version" : "failed"}});
            c->gotAnswer = true;  // not "unreachable"
            closeConn(c, false);
            return;
        }
        Reader r(body.data() + kHeaderLen + 1, body.size() - kHeaderLen - 1);
        pairOut.peerId = r.array<16>();
        const auto name = QString::fromStdString(r.str8());
        if (!name.isEmpty()) pairOut.peerName = name;
        c->gotAnswer = true;
        sendTo(pairOut.client, {{"type", "pair-peer"}, {"name", pairOut.peerName}});
        if (!pairOut.code.isEmpty()) {
            c->pairing = std::make_unique<Pairing>(store.local(), pairOut.code.toStdString(), uint64_t(now()));
            c->pairing->start();
        }
        if (!rest.isEmpty() && c->pairing) c->pairing->receive(reinterpret_cast<const uint8_t*>(rest.constData()), size_t(rest.size()), uint64_t(now()));
        pump(c);
        if (c->pairing) pairDone(c);
        return;
    }
    if (!c->pairing) return;
    c->pairing->receive(reinterpret_cast<const uint8_t*>(data.constData()), size_t(data.size()), uint64_t(now()));
    pump(c);
    pairDone(c);
}

void Hub::pairOutStart() {
    auto* s = new QTcpSocket(this);
    auto* c = makeConn(s, true);
    c->kind = Conn::Kind::PairOut;
    pairOut.conn = c;
    connect(s, &QTcpSocket::connected, this, [this, c] {
        if (!conns.contains(c)) return;
        Bytes req;
        putHeader(req, FrameMode::Pair);
        putU8(req, 0);
        putBytes(req, store.local().id);
        putStr8(req, store.local().name);
        Bytes frame;
        writeFrame(frame, req);
        c->socket->write(fromBytes(frame));
    });
    s->connectToHost(pairOut.host, pairOut.port);
}

void Hub::pairDone(Conn* c) {
    if (!c->pairing || c->pairing->result() == PairResult::Pending) return;
    const auto result = c->pairing->result();
    if (result == PairResult::Paired) {
        const auto& info = c->pairing->peer();
        auto* p = store.find(info.id);
        if (!p) { store.peers.push_back({}); p = &store.peers.back(); p->id = info.id; p->name = QString::fromStdString(info.name); }
        p->key = info.key;
        p->peerName = QString::fromStdString(info.name);
        if (p->name.isEmpty()) p->name = p->peerName;
        p->paired = true;
        p->keyChanged = false;
        p->lastSeen = wall();
        const auto hex = hexId(info.id);
        runs[hex].forgotten = false;
        runs[hex].failed = false;
        runs[hex].keyMismatch = false;
        if (c->kind == Conn::Kind::PairOut) {
            if (!pairOut.typedAddress.isEmpty()) p->address = pairOut.typedAddress;
            p->goodHost = c->socket->peerAddress().toString();
            p->goodPort = pairOut.port;
        }
        nearby.remove(hex);
        store.save();
    }
    if (c->kind == Conn::Kind::PairOut) {
        sendTo(pairOut.client, {{"type", "pair-result"}, {"result", pairResultName(result)}, {"name", pairOut.peerName},
                                {"id", result == PairResult::Paired ? hexId(c->pairing->peer().id) : QString()}});
        if (result == PairResult::Paired) { pairOut.client = nullptr; pairOut.code.clear(); }
    } else if (code) {
        if (result == PairResult::Paired) { code->succeeded(); endPairRequest("paired"); }
        else if (result == PairResult::LockedOut || code->triesLeft() <= 0) endPairRequest("lockedout");
        else if (result == PairResult::Expired) endPairRequest("expired");
        else broadcastWatchers({{"type", "pair-request-update"}, {"result", pairResultName(result)}, {"tries", code->triesLeft()}});
    }
    closeConn(c);
}

// A code that ran out or was locked out gets the same cooldown, so codes can't be cycled.
void Hub::endPairRequest(const QString& result) {
    if (result == "lockedout" || result == "expired") pairCooldown = now() + kPairCooldownMs;
    broadcastWatchers({{"type", "pair-request-end"}, {"result", result}, {"name", requesterName}});
    code.reset();
    requester = {};
    requesterName.clear();
    for (auto* c : QList<Conn*>(conns)) if (c->kind == Conn::Kind::PairIn && c->pairing && c->pairing->result() == PairResult::Pending) closeConn(c, false);
}

// ---- Channels ----

bool Hub::wanted(const QString& hex) const {
    return std::any_of(attaches.begin(), attaches.end(), [&](const Attach* a) { return a->link == hex; });
}

Hub::Attach* Hub::attachOn(Conn* c, uint16_t ch) {
    for (auto* a : attaches) if (a->chLink == c && a->ch == ch) return a;
    return nullptr;
}

void Hub::muxEvents(Conn* c) {
    auto& mux = c->link->mux();
    while (!mux.events.empty() && conns.contains(c)) {
        auto e = std::move(mux.events.front());
        mux.events.pop_front();
        auto& r = runs[c->peer];
        r.idleSince = now();
        auto* a = attachOn(c, e.channel);
        using K = ChannelEvent::Kind;
        switch (e.kind) {
            case K::OpenRequest:
                if (e.channelKind != ChannelKind::Infrared) mux.close(e.channel, CloseReason::Gone);  // serial and Bluetooth come later
                else irRequest(c, e);
                break;
            case K::Accepted:
                if (a) { a->st = Attach::St::Joined; a->farMachine = QString::fromStdString(e.machine); a->farDevice = QString::fromStdString(e.device);
                         a->farState = PeerState::Running; if (!a->coreUp) mux.sendState(a->ch, PeerState::Asleep); }
                break;
            case K::Waiting:
                if (a) a->st = Attach::St::Waiting;
                break;
            case K::Busy:
                if (a) { a->st = Attach::St::Busy; a->ch = 0; a->chLink = nullptr; a->busyBy = QString::fromStdString(e.machine);
                         a->retryAt = now() + kBusyRetryMs; }
                break;
            case K::Data:
                if (a && a->st == Attach::St::Joined && a->coreUp && a->core) a->core->write(fromBytes(e.data));
                mux.consumed(e.channel, e.data.size());
                break;
            case K::Closed:
                if (a) { a->st = Attach::St::Idle; a->ch = 0; a->chLink = nullptr; a->retryAt = now() + 500; a->farMachine.clear(); }
                r.held.erase(std::remove_if(r.held.begin(), r.held.end(), [&](const Held& h) { return h.ch == e.channel; }), r.held.end());
                break;
            case K::State:
                if (a) a->farState = PeerState(e.code);
                break;
        }
    }
    if (conns.contains(c)) matchPeer(c->peer);
}

void Hub::irRequest(Conn* c, const ChannelEvent& e) {
    auto& mux = c->link->mux();
    auto& r = runs[c->peer];
    Attach *idle = nullptr, *pending = nullptr, *joined = nullptr;
    for (auto* a : attaches) {
        if (a->link != c->peer) continue;
        if (a->st == Attach::St::Idle || a->st == Attach::St::Busy) { if (!idle) idle = a; }
        else if (a->st == Attach::St::Opening || a->st == Attach::St::Waiting) { if (!pending) pending = a; }
        else if (a->st == Attach::St::Joined && !joined) joined = a;
    }
    const auto machine = QString::fromStdString(e.machine), device = QString::fromStdString(e.device);
    auto join = [&](Attach* a) {
        mux.accept(e.channel, a->machine.toStdString(), a->device.toStdString());
        a->st = Attach::St::Joined; a->ch = e.channel; a->chLink = c; a->farMachine = machine; a->farDevice = device;
        a->farState = PeerState::Running;
        if (!a->coreUp) mux.sendState(a->ch, PeerState::Asleep);
    };
    if (idle) return join(idle);
    if (pending) {
        // Both sides opened at once: the link's initiator keeps its own channel, the other yields.
        if (c->outgoing) { mux.waiting(e.channel); r.held.push_back({e.channel, machine, device}); return; }
        mux.close(pending->ch, CloseReason::Normal);
        return join(pending);
    }
    if (joined) { mux.busy(e.channel, joined->farMachine.toStdString(), joined->farDevice.toStdString()); return; }
    mux.waiting(e.channel);
    r.held.push_back({e.channel, machine, device});
}

void Hub::matchPeer(const QString& hex) {
    if (hex.isEmpty() || hex == "local" || !runs.contains(hex)) return;
    auto& r = runs[hex];
    Conn* c = r.link;
    if (!c || !c->up || !c->link || c->link->state() != LinkState::Up) return;
    auto& mux = c->link->mux();
    for (auto* a : attaches) {
        if (a->link != hex || !(a->st == Attach::St::Idle || a->st == Attach::St::Busy) || a->retryAt > now()) continue;
        if (!r.held.isEmpty()) {
            const auto h = r.held.takeFirst();
            if (mux.accept(h.ch, a->machine.toStdString(), a->device.toStdString())) {
                a->st = Attach::St::Joined; a->ch = h.ch; a->chLink = c; a->farMachine = h.machine; a->farDevice = h.device;
                a->farState = PeerState::Running;
                if (!a->coreUp) mux.sendState(a->ch, PeerState::Asleep);
                continue;
            }
        }
        const auto ch = mux.open(ChannelKind::Infrared, a->machine.toStdString(), a->device.toStdString());
        if (!ch) continue;
        a->st = Attach::St::Opening; a->ch = ch; a->chLink = c;
    }
    // Held requests while every machine here is joined are busy.
    Attach* joined = nullptr;
    bool spare = false;
    for (auto* a : attaches) {
        if (a->link != hex) continue;
        if (a->st == Attach::St::Joined && !joined) joined = a;
        if (a->st != Attach::St::Joined) spare = true;
    }
    if (joined && !spare)
        for (const auto& h : std::exchange(r.held, {})) mux.busy(h.ch, joined->farMachine.toStdString(), joined->farDevice.toStdString());
    pump(c);
}

void Hub::matchLocal() {
    QList<Attach*> locals;
    for (auto* a : attaches) if (a->link == "local") locals.push_back(a);
    for (int i = 0; i < locals.size(); ++i) {
        auto* a = locals[i];
        auto* partner = i == 0 && locals.size() > 1 ? locals[1] : i == 1 ? locals[0] : nullptr;
        a->partner = partner;
        a->st = partner ? Attach::St::Joined : i >= 2 ? Attach::St::Busy : Attach::St::Waiting;
        a->farMachine = partner ? partner->machine : QString();
        a->farDevice = partner ? partner->device : QString();
        a->busyBy = i >= 2 ? locals[0]->machine : QString();
    }
}

void Hub::unjoin(Attach* a, bool closeChannel) {
    if (a->chLink && a->ch && conns.contains(a->chLink) && a->chLink->link && closeChannel) {
        a->chLink->link->mux().close(a->ch, CloseReason::Gone);
        pump(a->chLink);
    }
    a->st = Attach::St::Idle; a->ch = 0; a->chLink = nullptr;
    if (a->partner) { a->partner->partner = nullptr; a->partner = nullptr; }
}

void Hub::coreConnect(Attach* a) {
    if (a->core) { a->core->disconnect(this); a->core->abort(); a->core->deleteLater(); a->core = nullptr; }
    a->nextCoreTry = now() + kCoreRetryMs;
    if (a->socketPath.isEmpty()) return;
    auto* s = new QLocalSocket(this);
    a->core = s;
    connect(s, &QLocalSocket::connected, this, [this, a, s] {
        if (!attaches.contains(a) || a->core != s) return;
        a->coreUp = true;
        coreState(a, PeerState::Running);
    });
    connect(s, &QLocalSocket::readyRead, this, [this, a, s] {
        if (!attaches.contains(a) || a->core != s) return;
        coreBytes(a, s->readAll());
    });
    connect(s, &QLocalSocket::disconnected, this, [this, a, s] {
        if (!attaches.contains(a) || a->core != s) return;
        // Sleep, load or reset closed the core's port; it reopens on wake.
        const bool was = a->coreUp;
        a->coreUp = false;
        if (was) coreState(a, PeerState::Asleep);
        a->core = nullptr;
        s->deleteLater();
        a->nextCoreTry = now() + kCoreRetryMs;
    });
    connect(s, &QLocalSocket::errorOccurred, this, [this, a, s](QLocalSocket::LocalSocketError) {
        if (!attaches.contains(a) || a->core != s || s->state() == QLocalSocket::ConnectedState) return;
        if (a->coreUp) { a->coreUp = false; coreState(a, PeerState::Asleep); }
        a->core = nullptr;
        s->disconnect(this);
        s->deleteLater();
        a->nextCoreTry = now() + kCoreRetryMs;
    });
    s->connectToServer(a->socketPath);
}

void Hub::coreBytes(Attach* a, const QByteArray& data) {
    if (a->st != Attach::St::Joined) return;  // nobody in range
    if (a->partner) {
        if (a->partner->coreUp && a->partner->core) a->partner->core->write(data);
        return;
    }
    if (!a->chLink || !conns.contains(a->chLink) || !a->chLink->link) return;
    auto& mux = a->chLink->link->mux();
    for (int at = 0; at < data.size(); at += 2048) {
        const auto part = data.mid(at, 2048);
        mux.send(a->ch, reinterpret_cast<const uint8_t*>(part.constData()), size_t(part.size()));
    }
    pump(a->chLink);
}

void Hub::coreState(Attach* a, PeerState state) {
    if (a->st == Attach::St::Joined && a->chLink && conns.contains(a->chLink) && a->chLink->link) {
        a->chLink->link->mux().sendState(a->ch, state);
        pump(a->chLink);
    }
}

// ---- Discovery ----
// Beacon: "FRLB", version 1, type (1 beacon, 2 query), install id, TCP port, name.

bool Hub::discoveryActive() const {
    if (watchers() || pairOut.client) return true;
    return std::any_of(attaches.begin(), attaches.end(), [](const Attach* a) { return a->link != "local"; });
}

void Hub::beacon(quint8 type, const QHostAddress& to, quint16 port) {
    if (!udp.isValid() && udp.state() != QAbstractSocket::BoundState) return;
    Bytes b(kBeaconMagic.begin(), kBeaconMagic.end());
    putU8(b, 1);
    putU8(b, type);
    putBytes(b, store.local().id);
    putU16(b, listener.serverPort());
    putStr8(b, store.local().name);
    const auto datagram = fromBytes(b);
    if (!to.isNull()) { udp.writeDatagram(datagram, to, port); return; }
    if (!beaconTargets.isEmpty()) {
        for (const auto& t : beaconTargets) udp.writeDatagram(datagram, t.first, t.second);
        return;
    }
    QList<QHostAddress> targets{QHostAddress::Broadcast};
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        if (!(iface.flags() & QNetworkInterface::IsUp) || !(iface.flags() & QNetworkInterface::CanBroadcast)) continue;
        for (const auto& entry : iface.addressEntries())
            if (!entry.broadcast().isNull() && !targets.contains(entry.broadcast())) targets.push_back(entry.broadcast());
    }
    for (const auto& t : targets) udp.writeDatagram(datagram, t, discoveryPort);
}

void Hub::datagrams() {
    while (udp.hasPendingDatagrams()) {
        const auto d = udp.receiveDatagram(512);
        const auto data = d.data();
        if (!allowedPeer(d.senderAddress()) || !data.startsWith(kBeaconMagic)) continue;
        Reader r(reinterpret_cast<const uint8_t*>(data.constData()) + 4, size_t(data.size() - 4));
        const auto version = r.u8(), type = r.u8();
        const auto id = r.array<16>();
        const auto port = r.u16();
        const auto name = QString::fromStdString(r.str8()).left(64);
        if (!r.ok() || version != 1 || id == store.local().id) continue;
        if (type == 2 && discoveryActive()) beacon(1, d.senderAddress(), quint16(d.senderPort()));
        const auto hex = hexId(id);
        if (auto* p = store.find(id); p && p->paired) {
            auto& run = runs[hex];
            const bool moved = run.seen != d.senderAddress() || run.seenPort != port || now() - run.seenAt > kNearbyMs;
            run.seen = d.senderAddress();
            run.seenPort = port;
            run.seenAt = now();
            // A peer that moved or came back is dialled at once, at most once a second, starting at the new address.
            if (moved && wanted(hex) && !run.link && !run.dialing && now() - run.beaconDialAt >= kBackoff[0]) {
                run.nextTry = now();
                run.target = 0;
                run.beaconDialAt = now();
            }
        } else {
            nearby[hex] = {name, d.senderAddress(), port, now()};
        }
    }
}

// ---- Status ----

QJsonObject Hub::irStatus(const Attach* a) const {
    QJsonObject o{{"type", "ir"}};
    const bool local = a->link == "local";
    const Peer* p = local ? nullptr : const_cast<Store&>(store).find(a->link);
    o["peer"] = local ? QCoreApplication::translate("frondLink", "This computer") : p ? p->name : QString();
    QString state;
    if (!local && !p) state = "unknown";
    else if (p && (p->keyChanged || (runs.contains(a->link) && runs[a->link].keyMismatch && !runs[a->link].link)))
        state = "keychanged";
    else if (!local && runs.contains(a->link) && runs[a->link].forgotten) state = "forgotten";
    else switch (a->st) {
        case Attach::St::Joined: state = "joined"; break;
        case Attach::St::Waiting: state = "waiting"; break;
        case Attach::St::Busy: state = "busy"; break;
        default: {
            const auto it = runs.find(a->link);
            state = !local && it != runs.end() && it->failed && !it->link ? "notfound" : "connecting";
        }
    }
    o["state"] = state;
    o["machine"] = a->farMachine;
    o["device"] = a->farDevice;
    o["busyBy"] = a->busyBy;
    o["core"] = a->coreUp;
    PeerState far = a->farState;
    if (a->partner) far = a->partner->coreUp ? PeerState::Running : PeerState::Asleep;
    o["far"] = far == PeerState::Asleep ? "asleep" : far == PeerState::Paused ? "paused" : far == PeerState::Reset ? "reset" : "running";
    return o;
}

QString Hub::peerChannels(const QString& hex) const {
    QStringList parts;
    for (const auto* a : attaches)
        if (a->link == hex && a->st == Attach::St::Joined && (hex != "local" || a->partner))
            parts << QStringLiteral("%1 – %2").arg(a->machine, a->farMachine.isEmpty() ? a->farDevice : a->farMachine);
    if (hex == "local" && parts.size() == 2) parts.removeLast();
    return parts.isEmpty() ? QString() : QCoreApplication::translate("frondLink", "Infrared: %1").arg(parts.join(", "));
}

QJsonObject Hub::snapshot() const {
    QJsonArray list;
    for (const auto& p : store.peers) {
        const auto hex = hexId(p.id);
        const auto it = runs.find(hex);
        const bool up = it != runs.end() && it->link && it->link->up;
        QJsonObject o{{"id", hex}, {"name", p.name}, {"peerName", p.peerName}, {"address", p.address},
                      {"keyChanged", p.keyChanged}, {"up", up}, {"lastSeen", double(p.lastSeen)}};
        if (it != runs.end()) {
            o["forgotten"] = it->forgotten;
            if (it->keyMismatch && !up) o["keyChanged"] = true;
            if (up) o["rtt"] = double(it->link->link->rttMs());
            if (!it->seen.isNull() && now() - it->seenAt < kNearbyMs) o["seen"] = it->seen.toString();
        }
        o["channels"] = peerChannels(hex);
        list.append(o);
    }
    QJsonArray near;
    for (auto it = nearby.begin(); it != nearby.end(); ++it)
        near.append(QJsonObject{{"id", it.key()}, {"name", it->name}, {"address", it->address.toString()}, {"port", int(it->port)}});
    return {{"type", "state"}, {"id", hexId(store.local().id)}, {"name", store.name()}, {"port", int(listener.serverPort())},
            {"connections", list}, {"nearby", near}, {"local", peerChannels("local")}};
}

void Hub::tick() {
    const auto t = now();
    for (auto* c : QList<Conn*>(conns)) {
        if (!conns.contains(c)) continue;
        if (c->closing) {
            if ((t - c->created > 2000 && c->socket->state() != QAbstractSocket::ConnectedState) || t - c->closingAt > kClosingMs)
                removeConn(c);
            else if (c->socket->bytesToWrite() == 0) c->socket->disconnectFromHost();
            continue;
        }
        if (c->kind == Conn::Kind::Unknown && t - c->created > kFirstFrameMs) { closeConn(c, false); continue; }
        if (c->outgoing && c->socket->state() != QAbstractSocket::ConnectedState && t - c->created > 3000) { removeConn(c); continue; }
        if (c->link && c->socket->state() == QAbstractSocket::ConnectedState) { c->link->tick(uint64_t(t)); afterLink(c); }
        else if (c->kind == Conn::Kind::Link && !c->outgoing && t - c->created > kFirstFrameMs && !c->up) closeConn(c, false);
        if (c->pairing && conns.contains(c)) { c->pairing->tick(uint64_t(t)); pump(c); pairDone(c); }
        else if (c->kind == Conn::Kind::PairOut && t - c->created > Pairing::kTimeoutMs * 4) closeConn(c, false);
    }
    // Links: dial what's wanted, drop what nothing uses.
    for (const auto& p : store.peers) {
        const auto hex = hexId(p.id);
        const bool want = wanted(hex);
        if (!want && !runs.contains(hex)) continue;
        auto& r = runs[hex];
        if (want && !r.link && !r.dialing && t >= r.nextTry) dial(hex);
        if (r.link && !want && r.held.isEmpty() && t - r.idleSince > kLinkIdleMs) {
            bool used = false;
            for (const auto* a : attaches) if (a->chLink == r.link) used = true;
            if (!used) closeConn(r.link);
        } else if (want || !r.held.isEmpty()) r.idleSince = t;
    }
    // Cores: reconnect after sleep, load or reset.
    for (auto* a : attaches) if (!a->coreUp && !a->core && t >= a->nextCoreTry) coreConnect(a);
    for (auto* a : attaches) if (a->st == Attach::St::Busy && a->link != "local" && a->retryAt <= t) matchPeer(a->link);
    for (auto* a : attaches) if (a->st == Attach::St::Idle && a->link != "local" && a->retryAt <= t) matchPeer(a->link);
    // Discovery.
    const bool active = discoveryActive();
    if (active && (!wasActive || t - lastBeacon >= kBeaconMs)) {
        if (!wasActive) beacon(2);
        beacon(1);
        lastBeacon = t;
    }
    wasActive = active;
    for (auto it = nearby.begin(); it != nearby.end();) it = t - it->at > kNearbyMs ? nearby.erase(it) : std::next(it);
    for (auto it = recent.begin(); it != recent.end();)
        it = it->isEmpty() || t - it->last() > 10000 ? recent.erase(it) : std::next(it);
    if (code && t - codeAt >= pairCodeMs) endPairRequest("expired");
    // Status.
    for (auto* a : attaches) {
        const auto s = irStatus(a);
        if (s != a->lastStatus) { a->lastStatus = s; sendTo(a->client, s); }
    }
    if (watchers()) {
        const auto s = snapshot();
        if (s != lastSnapshot) { lastSnapshot = s; broadcastWatchers(s); }
    } else lastSnapshot = {};
    if (clients.isEmpty() && t - idleSince > idleExitMs) QCoreApplication::quit();
}

int runHub(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setOrganizationName("Frond");
    app.setApplicationName("Frond");
    Hub hub;
    const auto error = hub.start();
    if (!error.isEmpty()) {
        std::fprintf(stderr, "frond --link-service: %s\n", qPrintable(error));
        return 1;
    }
    return app.exec();
}

}  // namespace frondLink
