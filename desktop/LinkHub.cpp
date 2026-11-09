#include "LinkHub.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QNetworkInterface>
#include <QNetworkDatagram>
#include <QDir>
#include <cstdio>
#include <algorithm>
#ifdef Q_OS_UNIX
#include <sys/socket.h>
#endif
#ifdef Q_OS_LINUX
#include <csignal>
#include <sys/prctl.h>
#endif

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
constexpr qint64 kRadioRetryMs = 100;
constexpr qint64 kRadioStartMs = 5000;  // btvirt gets this long to make its socket
// Bluetooth bytes the hub reads ahead from a core or the radio; past that they wait in its socket.
constexpr qint64 kBluetoothAhead = 16384;
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
    ChannelKind kind{ChannelKind::Infrared};
    QString machine, device, link, socketPath;
    bool phone{false};  // Bluetooth on this Frond's radio with the mobile phone
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
    // Serial: bytes from the link written to the core and not yet taken by its socket (credit
    // goes back as it takes them), and bytes that came before the core socket was up.
    qint64 unacked{0};
    QByteArray pendingIn;
    // Bluetooth: the hub's socket the core connects to, once per controller.
    QLocalServer* btServer{nullptr};
    bool serial() const { return kind == ChannelKind::Serial; }
    bool bt() const { return kind == ChannelKind::Bluetooth; }
    // Joined to a machine: over a link, or to a partner on this computer.
    bool joined() const { return st == St::Joined && (link != QLatin1String("local") || partner); }
};

struct Hub::Controller {
    Attach* owner{nullptr};          // the machine here it belongs to; null for one a peer put on our radio
    QString peer;                    // a peer's controller: the connection it came over
    QString machine;                 // a peer's controller: its machine
    QLocalSocket* core{nullptr};     // ours: the core's connection
    QLocalSocket* radioSock{nullptr};  // our radio, for ours on this Frond's radio and for a peer's
    bool radioUp{false};
    bool wantsRadio{false};
    qint64 radioTry{0};
    Conn* chLink{nullptr};           // the Bluetooth channel, when the radio is on the other side
    uint16_t ch{0};
    bool open{false};                // the channel was accepted
    qint64 unacked{0};               // link bytes written to the near socket and not yet taken
    QByteArray pending;              // link bytes that came before the radio was up
    // The socket the channel's bytes go to and come from.
    QLocalSocket* nearEnd() const { return owner ? core : radioUp ? radioSock : nullptr; }
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
    for (auto* a : QList<Attach*>(attaches)) if (a->client == c) removeAttach(a);
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
    } else if (type == "peer-radio") {
        auto* p = store.find(op.value("id").toString());
        if (!p) return;
        const auto hex = hexId(p->id);
        const bool here = op.value("radio").toString() != QLatin1String("there");
        const qint64 at = std::max(wall(), p->bluetoothRadioAt + 1);
        setRadio(hex, here, at);
        if (const auto it = runs.find(hex); it != runs.end() && it->link && it->link->up && it->link->link) {
            it->link->link->sendRadio({here, uint64_t(at)});
            pump(it->link);
        }
    } else if (type == "bt-attach") {
        btAttach(c, op);
    } else if (type == "bt-detach") {
        for (auto* a : QList<Attach*>(attaches)) if (a->client == c && a->bt()) removeAttach(a);
    } else if (type == "ir-attach" || type == "serial-attach") {
        const auto kind = type == "ir-attach" ? ChannelKind::Infrared : ChannelKind::Serial;
        for (auto* a : attaches) if (a->client == c && a->kind == kind) return;  // one of each per window
        auto* a = new Attach;
        a->client = c;
        a->kind = kind;
        a->machine = op.value("machine").toString();
        a->device = op.value("device").toString();
        a->link = op.value("link").toString();
        a->socketPath = op.value("socket").toString();
        attaches.push_back(a);
        // A serial port opens only once it's joined, so the Palm's DSR and DCD mean the far port is open.
        if (!a->serial()) coreConnect(a);
        if (a->link == "local") matchLocal();
        else {
            auto& r = runs[a->link];
            if (!r.link && !r.dialing) { r.nextTry = now(); r.backoff = 0; }
            matchPeer(a->link);
        }
        syncCores();
    } else if (type == "ir-detach" || type == "serial-detach") {
        const auto kind = type == "ir-detach" ? ChannelKind::Infrared : ChannelKind::Serial;
        for (auto* a : QList<Attach*>(attaches)) if (a->client == c && a->kind == kind) removeAttach(a);
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
    if (c->up) btRadioChoices(c);
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
    btLinkUp(c);
}

// The link went: its channels are gone, the machines on it wait again.
void Hub::linkDown(Conn* c) {
    for (auto* a : attaches) if (a->chLink == c) clearChannel(a);
    btLinkDown(c);
    if (!c->peer.isEmpty() && runs.contains(c->peer)) {
        auto& r = runs[c->peer];
        if (r.link == c) { r.link = nullptr; r.held.clear(); r.nextTry = now() + kBackoff[0]; }
        if (r.dialing == c) r.dialing = nullptr;
    }
    // Serial bytes in flight are lost and the cores' serial ports close (DSR and DCD drop).
    syncCores();
}

void Hub::pump(Conn* c) {
    if (!conns.contains(c) || c->socket->state() != QAbstractSocket::ConnectedState) return;
    // Then serial bytes the cores held while the channel had no room, and what they make.
    for (int round = 0; round < 2; ++round) {
        while (c->socket->bytesToWrite() < kWriteAhead) {
            Bytes out;
            if (c->link && c->link->hasOutput()) out = c->link->takeOutput(size_t(kWriteAhead));
            else if (c->pairing) out = c->pairing->takeOutput();
            if (out.empty()) break;
            c->socket->write(fromBytes(out));
        }
        if (round == 1 || c->closing) break;
        const bool serialMoved = pullSerial(c);
        if (!(pullBluetooth(c) || serialMoved)) break;
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
        if (e.kind != K::OpenRequest) {
            if (auto* k = controllerOn(c, e.channel)) { btChannelEvent(k, e); continue; }
        }
        switch (e.kind) {
            case K::OpenRequest:
                if (e.channelKind == ChannelKind::Bluetooth) btRequest(c, e);
                else channelRequest(c, e);
                break;
            case K::Accepted:
                if (a) { a->st = Attach::St::Joined; a->farMachine = QString::fromStdString(e.machine); a->farDevice = QString::fromStdString(e.device);
                         a->farState = PeerState::Running; if (!a->coreUp && !a->serial()) mux.sendState(a->ch, PeerState::Asleep); }
                break;
            case K::Waiting:
                if (a) a->st = Attach::St::Waiting;
                break;
            case K::Busy:
                if (a) { a->st = Attach::St::Busy; a->ch = 0; a->chLink = nullptr; a->busyBy = QString::fromStdString(e.machine);
                         a->retryAt = now() + kBusyRetryMs; }
                break;
            case K::Data:
                if (a && a->serial() && a->st == Attach::St::Joined) { serialIn(a, fromBytes(e.data)); break; }
                if (a && a->st == Attach::St::Joined && a->coreUp && a->core) a->core->write(fromBytes(e.data));
                mux.consumed(e.channel, e.data.size());
                break;
            case K::Closed:
                if (a) { clearChannel(a); a->retryAt = now() + 500; }
                r.held.erase(std::remove_if(r.held.begin(), r.held.end(), [&](const Held& h) { return h.ch == e.channel; }), r.held.end());
                break;
            case K::State:
                if (a) a->farState = PeerState(e.code);
                break;
        }
    }
    if (conns.contains(c)) matchPeer(c->peer);
    syncCores();
}

void Hub::channelRequest(Conn* c, const ChannelEvent& e) {
    auto& mux = c->link->mux();
    auto& r = runs[c->peer];
    Attach *idle = nullptr, *pending = nullptr, *joined = nullptr;
    for (auto* a : attaches) {
        if (a->link != c->peer || a->kind != e.channelKind) continue;
        if (a->st == Attach::St::Idle || a->st == Attach::St::Busy) { if (!idle) idle = a; }
        else if (a->st == Attach::St::Opening || a->st == Attach::St::Waiting) { if (!pending) pending = a; }
        else if (a->st == Attach::St::Joined && !joined) joined = a;
    }
    const auto machine = QString::fromStdString(e.machine), device = QString::fromStdString(e.device);
    auto join = [&](Attach* a) {
        mux.accept(e.channel, a->machine.toStdString(), a->device.toStdString());
        a->st = Attach::St::Joined; a->ch = e.channel; a->chLink = c; a->farMachine = machine; a->farDevice = device;
        a->farState = PeerState::Running;
        if (!a->coreUp && !a->serial()) mux.sendState(a->ch, PeerState::Asleep);
    };
    if (idle) return join(idle);
    if (pending) {
        // Both sides opened at once: the link's initiator keeps its own channel, the other yields.
        if (c->outgoing) { mux.waiting(e.channel); r.held.push_back({e.channel, e.channelKind, machine, device}); return; }
        mux.close(pending->ch, CloseReason::Normal);
        return join(pending);
    }
    if (joined) { mux.busy(e.channel, joined->farMachine.toStdString(), joined->farDevice.toStdString()); return; }
    mux.waiting(e.channel);
    r.held.push_back({e.channel, e.channelKind, machine, device});
}

void Hub::matchPeer(const QString& hex) {
    if (hex.isEmpty() || hex == "local" || !runs.contains(hex)) return;
    auto& r = runs[hex];
    Conn* c = r.link;
    if (!c || !c->up || !c->link || c->link->state() != LinkState::Up) return;
    auto& mux = c->link->mux();
    for (auto* a : attaches) {
        if (a->bt() || a->link != hex || !(a->st == Attach::St::Idle || a->st == Attach::St::Busy) || a->retryAt > now()) continue;
        const auto held = std::find_if(r.held.begin(), r.held.end(), [&](const Held& h) { return h.kind == a->kind; });
        if (held != r.held.end()) {
            const auto h = *held;
            r.held.erase(held);
            if (mux.accept(h.ch, a->machine.toStdString(), a->device.toStdString())) {
                a->st = Attach::St::Joined; a->ch = h.ch; a->chLink = c; a->farMachine = h.machine; a->farDevice = h.device;
                a->farState = PeerState::Running;
                if (!a->coreUp && !a->serial()) mux.sendState(a->ch, PeerState::Asleep);
                continue;
            }
        }
        const auto ch = mux.open(a->kind, a->machine.toStdString(), a->device.toStdString());
        if (!ch) continue;
        a->st = Attach::St::Opening; a->ch = ch; a->chLink = c;
    }
    // Held requests of a kind while every machine here of that kind is joined are busy.
    for (const auto kind : {ChannelKind::Infrared, ChannelKind::Serial}) {
        Attach* joined = nullptr;
        bool spare = false;
        for (auto* a : attaches) {
            if (a->link != hex || a->kind != kind) continue;
            if (a->st == Attach::St::Joined && !joined) joined = a;
            if (a->st != Attach::St::Joined) spare = true;
        }
        if (!joined || spare) continue;
        for (auto it = r.held.begin(); it != r.held.end();) {
            if (it->kind != kind) { ++it; continue; }
            mux.busy(it->ch, joined->farMachine.toStdString(), joined->farDevice.toStdString());
            it = r.held.erase(it);
        }
    }
    pump(c);
}

void Hub::matchLocal() {
    for (const auto kind : {ChannelKind::Infrared, ChannelKind::Serial}) {
        QList<Attach*> locals;
        for (auto* a : attaches) if (a->link == "local" && a->kind == kind) locals.push_back(a);
        for (int i = 0; i < locals.size(); ++i) {
            auto* a = locals[i];
            auto* partner = i == 0 && locals.size() > 1 ? locals[1] : i == 1 ? locals[0] : nullptr;
            // A serial port joined to another machine closes first, so its DSR and DCD drop.
            if (a->serial() && a->partner != partner) dropCore(a);
            a->partner = partner;
            a->st = partner ? Attach::St::Joined : i >= 2 ? Attach::St::Busy : Attach::St::Waiting;
            a->farMachine = partner ? partner->machine : QString();
            a->farDevice = partner ? partner->device : QString();
            a->busyBy = i >= 2 ? locals[0]->machine : QString();
        }
    }
}

void Hub::removeAttach(Attach* a) {
    if (a->bt()) dropAttachBt(a);
    unjoin(a, true);
    attaches.removeOne(a);
    if (a->core) { a->core->disconnect(this); a->core->abort(); a->core->deleteLater(); }
    const auto hex = a->link;
    delete a;
    if (hex == "local") matchLocal(); else matchPeer(hex);
    syncCores();
}

void Hub::unjoin(Attach* a, bool closeChannel) {
    if (a->chLink && a->ch && conns.contains(a->chLink) && a->chLink->link && closeChannel) {
        a->chLink->link->mux().close(a->ch, CloseReason::Gone);
        pump(a->chLink);
    }
    a->st = Attach::St::Idle; a->ch = 0; a->chLink = nullptr;
    a->unacked = 0; a->pendingIn.clear();
    if (a->partner) { a->partner->partner = nullptr; a->partner = nullptr; }
}

// The channel is gone; the machine waits for the next one.
void Hub::clearChannel(Attach* a) {
    a->st = Attach::St::Idle; a->ch = 0; a->chLink = nullptr; a->farMachine.clear(); a->farDevice.clear();
    a->farState = PeerState::Running;
    a->unacked = 0; a->pendingIn.clear();
}

void Hub::coreConnect(Attach* a) {
    if (a->core) { a->core->disconnect(this); a->core->abort(); a->core->deleteLater(); a->core = nullptr; }
    a->nextCoreTry = now() + kCoreRetryMs;
    if (a->socketPath.isEmpty()) return;
    auto* s = new QLocalSocket(this);
    a->core = s;
    // Serial: hold no more than a little of the core's bytes, so its queue fills and CTS drops.
    if (a->serial()) s->setReadBufferSize(kSerialAhead);
    connect(s, &QLocalSocket::connected, this, [this, a, s] {
        if (!attaches.contains(a) || a->core != s) return;
        a->coreUp = true;
#ifdef Q_OS_UNIX
        if (a->serial()) {
            const int size = int(kSerialAhead);
            ::setsockopt(int(s->socketDescriptor()), SOL_SOCKET, SO_SNDBUF, &size, sizeof size);
        }
#endif
        coreState(a, PeerState::Running);
        if (a->serial()) {
            if (!a->pendingIn.isEmpty()) { a->unacked += a->pendingIn.size(); s->write(std::exchange(a->pendingIn, {})); }
            if (a->partner) serialPull(a->partner);
            serialPull(a);
            if (a->chLink) pump(a->chLink);
        }
    });
    connect(s, &QLocalSocket::readyRead, this, [this, a, s] {
        if (!attaches.contains(a) || a->core != s) return;
        if (!a->serial()) { coreBytes(a, s->readAll()); return; }
        serialPull(a);
        if (a->chLink) pump(a->chLink);
    });
    connect(s, &QLocalSocket::bytesWritten, this, [this, a, s](qint64 n) {
        if (attaches.contains(a) && a->core == s && a->serial()) serialWritten(a, n);
    });
    connect(s, &QLocalSocket::disconnected, this, [this, a, s] {
        if (!attaches.contains(a) || a->core != s) return;
        // Sleep, load or reset closed the core's port; it reopens on wake.
        const bool was = a->coreUp;
        a->coreUp = false;
        if (was) coreState(a, PeerState::Asleep);
        a->core = nullptr;
        if (a->unacked) serialWritten(a, a->unacked);  // lost with the socket; their credit goes back
        s->deleteLater();
        a->nextCoreTry = now() + kCoreRetryMs;
    });
    connect(s, &QLocalSocket::errorOccurred, this, [this, a, s](QLocalSocket::LocalSocketError) {
        if (!attaches.contains(a) || a->core != s || s->state() == QLocalSocket::ConnectedState) return;
        if (a->coreUp) { a->coreUp = false; coreState(a, PeerState::Asleep); }
        a->core = nullptr;
        if (a->unacked) serialWritten(a, a->unacked);
        s->disconnect(this);
        s->deleteLater();
        a->nextCoreTry = now() + kCoreRetryMs;
    });
    s->connectToServer(a->socketPath);
}

void Hub::dropCore(Attach* a) {
    if (a->core) { a->core->disconnect(this); a->core->abort(); a->core->deleteLater(); a->core = nullptr; }
    a->coreUp = false;
    a->unacked = 0;
    a->pendingIn.clear();
}

// Serial ports are open only while joined: opened as a channel joins, closed as it goes.
void Hub::syncCores() {
    for (auto* a : attaches) {
        if (!a->serial()) continue;
        if (!a->joined()) { if (a->core || a->coreUp) dropCore(a); }
        else if (!a->core && !a->coreUp && now() >= a->nextCoreTry) coreConnect(a);
    }
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

// Bytes from the link for a serial port; credit goes back once the core's socket takes them.
void Hub::serialIn(Attach* a, const QByteArray& data) {
    if (a->coreUp && a->core) { a->unacked += data.size(); a->core->write(data); }
    else a->pendingIn += data;
}

void Hub::serialWritten(Attach* a, qint64 n) {
    if (a->partner) { serialPull(a->partner); return; }
    if (!a->chLink || !conns.contains(a->chLink) || !a->chLink->link) return;
    const auto k = std::min(n, a->unacked);
    a->unacked -= k;
    if (k > 0) {
        a->chLink->link->mux().consumed(a->ch, size_t(k));
        pump(a->chLink);
    }
}

// Takes what the other side has room for from the core's socket. The rest waits there.
bool Hub::serialPull(Attach* a) {
    if (!a->core || !a->coreUp || !a->joined()) return false;
    bool moved = false;
    if (a->partner) {
        auto* to = a->partner;
        if (!to->core || !to->coreUp) return false;
        while (a->core->bytesAvailable() > 0) {
            const qint64 room = kSerialAhead - to->core->bytesToWrite();
            if (room <= 0) break;
            const auto part = a->core->read(room);
            if (part.isEmpty()) break;
            to->core->write(part);
            moved = true;
        }
        return moved;
    }
    if (!a->chLink || !conns.contains(a->chLink) || !a->chLink->link) return false;
    auto& mux = a->chLink->link->mux();
    while (a->core->bytesAvailable() > 0) {
        const auto room = mux.writable(a->ch);
        if (room == 0) break;
        const auto part = a->core->read(qint64(std::min<size_t>(room, 2048)));
        if (part.isEmpty()) break;
        mux.send(a->ch, reinterpret_cast<const uint8_t*>(part.constData()), size_t(part.size()));
        moved = true;
    }
    return moved;
}

bool Hub::pullSerial(Conn* c) {
    bool moved = false;
    for (auto* a : attaches) if (a->serial() && a->chLink == c && serialPull(a)) moved = true;
    return moved;
}

// ---- Discovery ----
// Beacon: "FRLB", version 1, type (1 beacon, 2 query), install id, TCP port, name.

bool Hub::discoveryActive() const {
    if (watchers() || pairOut.client) return true;
    return std::any_of(attaches.begin(), attaches.end(), [](const Attach* a) { return a->link != "local" && a->link != "radio"; });
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

QJsonObject Hub::status(const Attach* a) const {
    QJsonObject o{{"type", a->bt() ? "bt" : a->serial() ? "serial" : "ir"}};
    if (a->bt()) {
        o["listening"] = a->btServer && a->btServer->isListening();
        o["controllers"] = int(std::count_if(controllers.begin(), controllers.end(), [a](const Controller* k) { return k->owner == a; }));
        if (a->link == QLatin1String("radio")) {
            o["peer"] = QCoreApplication::translate("frondLink", "This Frond");
            o["state"] = radioProgram().isEmpty() ? "noradio" : a->phone && phoneProgram().isEmpty() ? "nophone" : "radio";
            return o;
        }
    }
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
    if (a->bt() && p) {
        // Bluetooth has no channel of its own to wait for: once linked, it's on one side's radio.
        const bool here = radioHere(a->link);
        o["radio"] = here ? "here" : "there";
        if (state == "connecting" || state == "joined" || state == "waiting" || state == "busy") {
            const auto it = runs.find(a->link);
            if (it != runs.end() && it->link && it->link->up) state = here ? (radioProgram().isEmpty() ? "noradio" : "here") : "there";
            else state = "connecting";
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
    QStringList kinds;
    for (const auto kind : {ChannelKind::Infrared, ChannelKind::Serial}) {
        QStringList parts;
        for (const auto* a : attaches)
            if (a->link == hex && a->kind == kind && a->joined())
                parts << QStringLiteral("%1 – %2").arg(a->machine, a->farMachine.isEmpty() ? a->farDevice : a->farMachine);
        if (hex == "local" && parts.size() == 2) parts.removeLast();
        if (parts.isEmpty()) continue;
        kinds << (kind == ChannelKind::Serial ? QCoreApplication::translate("frondLink", "Serial: %1")
                                              : QCoreApplication::translate("frondLink", "Infrared: %1")).arg(parts.join(", "));
    }
    if (const auto bt = peerBluetooth(hex); !bt.isEmpty()) kinds << bt;
    return kinds.join(QStringLiteral(" · "));
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
        o["bluetoothRadio"] = radioHere(hex) ? "here" : "there";
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
            for (const auto* k : controllers) if (k->chLink == r.link) used = true;
            if (!used) closeConn(r.link);
        } else if (want || !r.held.isEmpty()) r.idleSince = t;
    }
    // Cores: reconnect after sleep, load or reset.
    for (auto* a : attaches) if (!a->serial() && !a->bt() && !a->coreUp && !a->core && t >= a->nextCoreTry) coreConnect(a);
    radioTick();
    syncCores();
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
        const auto s = status(a);
        if (s != a->lastStatus) { a->lastStatus = s; sendTo(a->client, s); }
    }
    if (watchers()) {
        const auto s = snapshot();
        if (s != lastSnapshot) { lastSnapshot = s; broadcastWatchers(s); }
    } else lastSnapshot = {};
    if (clients.isEmpty() && t - idleSince > idleExitMs) QCoreApplication::quit();
}

// ---- Bluetooth ----

QString Hub::radioProgram() {
    if (qEnvironmentVariableIsSet("FROND_BTVIRT")) {
        const auto custom = qEnvironmentVariable("FROND_BTVIRT");
        return !custom.isEmpty() && QFileInfo(custom).isExecutable() ? custom : QString();
    }
    const auto dir = QCoreApplication::applicationDirPath();
    for (const auto& candidate : {dir + "/../lib/frond/btvirt", dir + "/btvirt",
                                  dir + "/../build/bluetooth-provider/emulator/btvirt",
                                  dir + "/../../build/bluetooth-provider/emulator/btvirt"}) {
        const QFileInfo info(candidate);
        if (info.isFile() && info.isExecutable()) return info.canonicalFilePath();
    }
    return {};
}

QString Hub::phoneProgram() {
    if (qEnvironmentVariableIsSet("FROND_BT_PHONE")) {
        const auto custom = qEnvironmentVariable("FROND_BT_PHONE");
        return !custom.isEmpty() && QFileInfo(custom).isExecutable() ? custom : QString();
    }
    const auto dir = QCoreApplication::applicationDirPath();
    for (const auto& candidate : {dir + "/../lib/frond/bluetooth-phone", dir + "/bluetooth-phone",
                                  dir + "/../build/bluetooth-opp-peer/bluetooth-opp-peer",
                                  dir + "/../../build/bluetooth-opp-peer/bluetooth-opp-peer"}) {
        const QFileInfo info(candidate);
        if (info.isFile() && info.isExecutable()) return info.canonicalFilePath();
    }
    return {};
}

QString Hub::radioSocket() const { return radioFolder + "/bt-server-bredr"; }

// No one has chosen: the side with the lower install id holds the radio, as both sides work out the same.
bool Hub::radioHere(const QString& hex) const {
    const auto* p = const_cast<Store&>(store).find(hex);
    if (!p) return true;
    if (p->bluetoothRadioAt > 0) return p->bluetoothRadio != QLatin1String("there");
    return hexId(store.local().id) < hex;
}

Hub::Controller* Hub::controllerOn(Conn* c, uint16_t ch) const {
    for (auto* k : controllers) if (k->chLink == c && k->ch == ch) return k;
    return nullptr;
}

void Hub::btAttach(Client* c, const QJsonObject& op) {
    for (auto* a : attaches) if (a->client == c && a->bt()) return;  // one per window
    auto* a = new Attach;
    a->client = c;
    a->kind = ChannelKind::Bluetooth;
    a->machine = op.value("machine").toString();
    a->device = op.value("device").toString();
    a->link = op.value("link").toString();
    a->socketPath = op.value("socket").toString();
    a->phone = a->link == QLatin1String("radio") && op.value("phone").toBool();
    attaches.push_back(a);
    a->btServer = new QLocalServer(this);
    a->btServer->setSocketOptions(QLocalServer::UserAccessOption);
    connect(a->btServer, &QLocalServer::newConnection, this, [this, a] { if (attaches.contains(a)) btAccept(a); });
    if (!a->socketPath.isEmpty()) {
        QLocalServer::removeServer(a->socketPath);
        a->btServer->listen(a->socketPath);
    }
    if (a->link == QLatin1String("radio")) {
        startRadio();
        phoneTick();
    } else if (a->link != QLatin1String("local")) {
        auto& r = runs[a->link];
        if (!r.link && !r.dialing) { r.nextTry = now(); r.backoff = 0; }
    }
}

void Hub::dropAttachBt(Attach* a) {
    for (auto* k : QList<Controller*>(controllers)) if (k->owner == a) dropController(k);
    if (a->btServer) { a->btServer->disconnect(this); a->btServer->close(); a->btServer->deleteLater(); a->btServer = nullptr; }
}

// The core made a connection: a new controller.
void Hub::btAccept(Attach* a) {
    while (auto* s = a->btServer->nextPendingConnection()) {
        auto* k = new Controller;
        k->owner = a;
        k->core = s;
        s->setReadBufferSize(kBluetoothAhead);
        controllers.push_back(k);
        connect(s, &QLocalSocket::readyRead, this, [this, k] {
            if (!controllers.contains(k)) return;
            if (k->wantsRadio) { if (k->radioUp) btPipe(k->core, k->radioSock); return; }
            if (btPull(k) && k->chLink) pump(k->chLink);
        });
        connect(s, &QLocalSocket::bytesWritten, this, [this, k](qint64 n) { if (controllers.contains(k)) btWritten(k, n); });
        connect(s, &QLocalSocket::disconnected, this, [this, k] { if (controllers.contains(k)) dropController(k); });
        btRoute(k);
    }
}

// One of ours goes to this Frond's radio, or over the link once it's up. Until then it waits, unread.
void Hub::btRoute(Controller* k) {
    auto* a = k->owner;
    if (!a || k->wantsRadio || k->chLink) return;
    if (a->link == QLatin1String("radio") || (store.find(a->link) && radioHere(a->link))) {
        k->wantsRadio = true;
        btRadioConnect(k);
        return;
    }
    const auto it = runs.find(a->link);
    Conn* c = it != runs.end() ? it->link : nullptr;
    if (!c || !c->up || !c->link || c->link->state() != LinkState::Up) return;
    const auto ch = c->link->mux().open(ChannelKind::Bluetooth, a->machine.toStdString(), a->device.toStdString());
    if (!ch) { dropController(k); return; }
    k->chLink = c; k->ch = ch; k->open = false;
    pump(c);
}

// The peer puts one of its controllers on our radio.
void Hub::btRequest(Conn* c, const ChannelEvent& e) {
    auto& mux = c->link->mux();
    if (!radioHere(c->peer) || !startRadio()) { mux.close(e.channel, CloseReason::Gone); return; }
    auto* k = new Controller;
    k->peer = c->peer;
    k->machine = QString::fromStdString(e.machine);
    k->chLink = c; k->ch = e.channel; k->open = true;
    k->wantsRadio = true;
    controllers.push_back(k);
    mux.accept(e.channel, store.name().toStdString(), std::string());
    btRadioConnect(k);
}

void Hub::btChannelEvent(Controller* k, const ChannelEvent& e) {
    auto* c = k->chLink;
    auto& mux = c->link->mux();
    using K = ChannelEvent::Kind;
    switch (e.kind) {
        case K::Accepted: k->open = true; btPull(k); break;
        case K::Data: {
            const auto data = fromBytes(e.data);
            if (auto* s = k->nearEnd()) { k->unacked += data.size(); s->write(data); }
            else k->pending += data;  // credit goes back once the radio takes them
            break;
        }
        case K::Busy:
        case K::Closed: dropController(k, false); break;
        default: break;
    }
    (void)mux;
}

void Hub::btRadioConnect(Controller* k) {
    k->radioTry = now() + kRadioRetryMs;
    if (k->radioSock) return;
    if (!startRadio()) return;  // shown as no radio; it waits
    auto* s = new QLocalSocket(this);
    k->radioSock = s;
    s->setReadBufferSize(kBluetoothAhead);
    connect(s, &QLocalSocket::connected, this, [this, k, s] {
        if (!controllers.contains(k) || k->radioSock != s) return;
        k->radioUp = true;
        if (k->owner) { btPipe(k->core, s); return; }
        if (!k->pending.isEmpty()) { k->unacked += k->pending.size(); s->write(std::exchange(k->pending, {})); }
        if (btPull(k) && k->chLink) pump(k->chLink);
    });
    connect(s, &QLocalSocket::readyRead, this, [this, k, s] {
        if (!controllers.contains(k) || k->radioSock != s) return;
        if (k->owner) { btPipe(s, k->core); return; }
        if (btPull(k) && k->chLink) pump(k->chLink);
    });
    connect(s, &QLocalSocket::bytesWritten, this, [this, k, s](qint64 n) {
        if (controllers.contains(k) && k->radioSock == s && !k->owner) btWritten(k, n);
    });
    auto gone = [this, k, s] {
        if (!controllers.contains(k) || k->radioSock != s) return;
        s->disconnect(this);
        s->deleteLater();
        k->radioSock = nullptr;
        // btvirt makes its socket a moment after it starts: try again for a while, else the controller is lost.
        const bool starting = radio && radio->state() != QProcess::NotRunning && now() - radioStarted < kRadioStartMs;
        if (k->radioUp || !starting) { dropController(k); return; }
        k->radioTry = now() + kRadioRetryMs;
    };
    connect(s, &QLocalSocket::disconnected, this, gone);
    connect(s, &QLocalSocket::errorOccurred, this, [s, gone](QLocalSocket::LocalSocketError) {
        if (s->state() != QLocalSocket::ConnectedState) gone();
    });
    s->connectToServer(radioSocket());
}

void Hub::btPipe(QLocalSocket* from, QLocalSocket* to) {
    if (from && to && from->bytesAvailable() > 0) to->write(from->readAll());
}

// Takes what the channel has room for from the near socket. The rest waits there.
bool Hub::btPull(Controller* k) {
    auto* s = k->nearEnd();
    if (!s || !k->chLink || !k->open || !conns.contains(k->chLink) || !k->chLink->link) return false;
    auto& mux = k->chLink->link->mux();
    bool moved = false;
    while (s->bytesAvailable() > 0) {
        const auto room = mux.writable(k->ch);
        if (room == 0) break;
        const auto part = s->read(qint64(std::min<size_t>(room, 8192)));
        if (part.isEmpty()) break;
        mux.send(k->ch, reinterpret_cast<const uint8_t*>(part.constData()), size_t(part.size()));
        moved = true;
    }
    return moved;
}

bool Hub::pullBluetooth(Conn* c) {
    bool moved = false;
    for (auto* k : QList<Controller*>(controllers)) if (k->chLink == c && btPull(k)) moved = true;
    return moved;
}

void Hub::btWritten(Controller* k, qint64 n) {
    if (!k->chLink || !conns.contains(k->chLink) || !k->chLink->link) return;
    const auto taken = std::min(n, k->unacked);
    k->unacked -= taken;
    if (taken > 0) {
        k->chLink->link->mux().consumed(k->ch, size_t(taken));
        pump(k->chLink);
    }
}

// The controller is gone: the core sees its controller lost, the far side's channel closes.
void Hub::dropController(Controller* k, bool closeChannel) {
    if (!controllers.removeOne(k)) return;
    if (closeChannel && k->chLink && conns.contains(k->chLink) && k->chLink->link) {
        k->chLink->link->mux().close(k->ch, CloseReason::Gone);
        pump(k->chLink);
    }
    for (auto* s : {k->radioSock, k->core}) {
        if (!s) continue;
        s->disconnect(this);
        s->abort();
        s->deleteLater();
    }
    delete k;
}

void Hub::btLinkUp(Conn* c) {
    if (!conns.contains(c) || !c->link) return;
    const auto* p = store.find(c->peer);
    c->link->sendRadio({radioHere(c->peer), uint64_t(p ? p->bluetoothRadioAt : 0)});
    for (auto* k : QList<Controller*>(controllers))
        if (controllers.contains(k) && k->owner && k->owner->link == c->peer) btRoute(k);
    pump(c);
}

void Hub::btLinkDown(Conn* c) {
    for (auto* k : QList<Controller*>(controllers)) if (k->chLink == c) dropController(k, false);
}

// The peer's view of whose radio it is. The later choice wins; at the same time the lower id's.
void Hub::btRadioChoices(Conn* c) {
    auto& queue = c->link->radio;
    while (!queue.empty() && conns.contains(c)) {
        const auto r = queue.front();
        queue.pop_front();
        auto* p = store.find(c->peer);
        if (!p) continue;
        const bool theyMeanHere = !r.senderHolds;  // "here" as we say it
        const qint64 at = qint64(r.at);
        const bool same = theyMeanHere == radioHere(c->peer);
        if (at > p->bluetoothRadioAt || (at == p->bluetoothRadioAt && at > 0 && !same && c->peer < hexId(store.local().id))) {
            if (!same || at != p->bluetoothRadioAt) setRadio(c->peer, theyMeanHere, at);
        } else if (at < p->bluetoothRadioAt) {
            c->link->sendRadio({radioHere(c->peer), uint64_t(p->bluetoothRadioAt)});
        }
    }
}

void Hub::setRadio(const QString& hex, bool here, qint64 at) {
    auto* p = store.find(hex);
    if (!p) return;
    p->bluetoothRadio = here ? "here" : "there";
    p->bluetoothRadioAt = at;
    store.save();
    btReroute(hex);
}

// Controllers on the side that no longer holds the radio go; the cores reconnect to the new one.
void Hub::btReroute(const QString& hex) {
    const bool here = radioHere(hex);
    for (auto* k : QList<Controller*>(controllers)) {
        if (!controllers.contains(k)) continue;
        if (k->owner && k->owner->link == hex && ((here && k->chLink) || (!here && k->wantsRadio))) dropController(k);
        else if (!k->owner && k->peer == hex && !here) dropController(k);
    }
}

bool Hub::radioWanted() const {
    for (const auto* k : controllers) if (k->wantsRadio) return true;
    for (const auto* a : attaches) if (a->bt() && a->link == QLatin1String("radio")) return true;
    return false;
}

// Runs this Frond's radio, a btvirt in <runtime>/frond/radio. It goes with the hub.
bool Hub::startRadio() {
    if (radio && radio->state() != QProcess::NotRunning) return true;
    if (radio) { radio->deleteLater(); radio = nullptr; }
    if (radioFailed && now() - radioStarted < kRadioStartMs) return false;
    const auto program = radioProgram();
    const auto runtime = runtimeFolder();
    if (program.isEmpty() || runtime.isEmpty()) return false;
    radioFolder = runtime + "/radio";
    QDir().mkpath(radioFolder);
    QFile::setPermissions(radioFolder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    for (const auto& name : QDir(radioFolder).entryList({"bt-server-*"}, QDir::System | QDir::Files))
        QFile::remove(radioFolder + "/" + name);
    radio = new QProcess(this);
    radio->setProgram(program);
    radio->setArguments({"-s" + radioFolder});
    radio->setStandardOutputFile(QProcess::nullDevice());
    radio->setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_LINUX
    // A hub that's killed takes its radio with it.
    radio->setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGTERM); });
#endif
    radioStarted = radioIdleSince = now();
    radio->start();
    radioFailed = !radio->waitForStarted(2000);
    if (radioFailed) { radio->deleteLater(); radio = nullptr; return false; }
    return true;
}

bool Hub::phoneWanted() const {
    return std::any_of(attaches.begin(), attaches.end(), [](const Attach* a) { return a->bt() && a->phone; });
}

// Starts the phone once the radio runs, again a while after it stops (as when the radio restarts),
// and stops it when no machine wants it.
void Hub::phoneTick() {
    const bool running = phone && phone->state() != QProcess::NotRunning;
    if (!phoneWanted() || !radio || radio->state() == QProcess::NotRunning) {
        if (!phone) return;
        if (running) {
            phone->terminate();
            if (!phone->waitForFinished(1000)) phone->kill();
        }
        phone->deleteLater();
        phone = nullptr;
        return;
    }
    if (running || now() - phoneStarted < kRadioStartMs) return;
    const auto program = phoneProgram();
    if (program.isEmpty()) return;
    if (phone) phone->deleteLater();
    phone = new QProcess(this);
    phone->setProgram(program);
    // The phone waits for the radio's socket itself.
    phone->setArguments({radioSocket(), "--dun"});
    phone->setStandardOutputFile(QProcess::nullDevice());
    phone->setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_LINUX
    phone->setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGTERM); });
#endif
    phoneStarted = now();
    phone->start();
}

void Hub::radioTick() {
    phoneTick();
    const auto t = now();
    for (auto* k : QList<Controller*>(controllers))
        if (controllers.contains(k) && k->wantsRadio && !k->radioSock && t >= k->radioTry) btRadioConnect(k);
    if (!radio) return;
    if (radioWanted()) { radioIdleSince = t; return; }
    if (t - radioIdleSince < kRadioIdleMs) return;
    radio->terminate();
    if (!radio->waitForFinished(1000)) radio->kill();
    radio->deleteLater();
    radio = nullptr;
    phoneTick();
}

QString Hub::peerBluetooth(const QString& hex) const {
    QStringList parts;
    for (const auto* a : attaches) {
        if (!a->bt() || a->link != hex) continue;
        if (std::any_of(controllers.begin(), controllers.end(), [a](const Controller* k) { return k->owner == a; }))
            parts << a->machine;
    }
    for (const auto* k : controllers) if (!k->owner && k->peer == hex && !parts.contains(k->machine)) parts << k->machine;
    if (parts.isEmpty()) return {};
    return QCoreApplication::translate("frondLink", "Bluetooth: %1").arg(parts.join(", "));
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
