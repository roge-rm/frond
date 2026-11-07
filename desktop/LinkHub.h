// frond --link-service: the per-user hub. It owns the store, discovery, the TCP listener, every
// link to a paired install and the cores' link sockets. Windows talk to it over a local socket
// with one JSON object per line (LinkCommon.h). It exits 30 s after its last client goes.
#pragma once
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <memory>
#include "../link/link.h"
#include "LinkCommon.h"
#include "LinkStore.h"

namespace frondLink {

int runHub(int argc, char** argv);

class Hub : public QObject {
    Q_OBJECT
 public:
    // Bytes the hub lets Qt hold for a link socket before it stops taking more from the mux, so
    // infrared never waits long behind serial or Bluetooth bytes. The kernel buffer is capped too.
    static constexpr qint64 kWriteAhead = 4096;
    static constexpr int kSendBuffer = 8192;
    static constexpr int kIdleExitMs = 30000;

    explicit Hub(QObject* parent = nullptr);
    ~Hub() override;
    QString start();

 private:
    struct Client;
    struct Conn;
    struct Attach;
    struct Held {
        uint16_t ch;
        QString machine, device;
    };
    struct PeerRun {
        Conn* link{nullptr};
        Conn* dialing{nullptr};
        int backoff{0};
        qint64 nextTry{0};
        int target{0};
        bool failed{false};      // the last round of dials found nothing
        bool forgotten{false};   // it doesn't know us any more
        // Set by either side's failed handshake against the pinned key. Neither is authenticated
        // (a refuse frame is cleartext, a bad message 2 can be anyone's), so it's only shown,
        // never stored, the hub keeps dialling with backoff, and a good link or pairing clears it.
        bool keyMismatch{false};
        // The last address a beacon gave. Anyone on the LAN can send one, so it's only tried
        // beside the peer's goodHost and typed address, and never replaces them.
        QHostAddress seen;
        quint16 seenPort{0};
        qint64 seenAt{0};
        qint64 beaconDialAt{-100000};
        qint64 idleSince{0};
        QList<Held> held;
    };
    struct Nearby {
        QString name;
        QHostAddress address;
        quint16 port{0};
        qint64 at{0};
    };

    qint64 now() const { return clock.elapsed(); }
    frond::link::LinkConfig config() const;
    // Clients
    void newClient();
    void clientInput(Client* c);
    void handle(Client* c, const QJsonObject& op);
    void dropClient(Client* c);
    void sendTo(Client* c, const QJsonObject& o);
    void broadcastWatchers(const QJsonObject& o);
    int watchers() const;
    // TCP
    void incoming();
    Conn* makeConn(QTcpSocket* s, bool outgoing);
    void connInput(Conn* c);
    void afterLink(Conn* c);
    void linkUp(Conn* c);
    void linkDown(Conn* c);
    void pump(Conn* c);
    void closeConn(Conn* c, bool flush = true);
    void removeConn(Conn* c);
    QList<QPair<QString, quint16>> dialTargets(const QString& hex) const;
    void dial(const QString& hex);
    void dialFailed(Conn* c);
    // Pairing
    void pairFirst(Conn* c, const QByteArray& first, const QByteArray& rest);
    void pairInput(Conn* c, const QByteArray& data);
    void pairOutStart();
    void pairDone(Conn* c);
    void endPairRequest(const QString& result);
    // Channels
    void muxEvents(Conn* c);
    void irRequest(Conn* c, const frond::link::ChannelEvent& e);
    void matchPeer(const QString& hex);
    void matchLocal();
    Attach* attachOn(Conn* c, uint16_t ch);
    void unjoin(Attach* a, bool closeChannel);
    void coreConnect(Attach* a);
    void coreBytes(Attach* a, const QByteArray& data);
    void coreState(Attach* a, frond::link::PeerState state);
    bool wanted(const QString& hex) const;
    // Discovery
    bool discoveryActive() const;
    void beacon(quint8 type, const QHostAddress& to = {}, quint16 port = 0);
    void datagrams();
    // Status
    void tick();
    QJsonObject irStatus(const Attach* a) const;
    QJsonObject snapshot() const;
    QString peerChannels(const QString& hex) const;

    Store store;
    QElapsedTimer clock;
    QLocalServer server;
    QTcpServer listener;
    QUdpSocket udp;
    quint16 discoveryPort{defaultPort};
    QList<QPair<QHostAddress, quint16>> beaconTargets;  // FROND_LINK_BEACON_TO, else broadcast
    qint64 idleExitMs{kIdleExitMs};
    qint64 pairCodeMs{qint64(frond::link::PairCode::kLifetimeMs)};
    QTimer timer;
    QList<Client*> clients;
    QList<Conn*> conns;
    QList<Attach*> attaches;
    QMap<QString, PeerRun> runs;
    QMap<QString, Nearby> nearby;
    QHash<QString, QList<qint64>> recent;  // incoming connection times per address
    qint64 lastBeacon{-100000}, idleSince{0};
    bool wasActive{false};
    QJsonObject lastSnapshot;
    // B: the code shown while someone pairs with us.
    std::unique_ptr<frond::link::PairCode> code;
    frond::link::InstallId requester{};
    QString requesterName;
    qint64 pairCooldown{0}, codeAt{0};
    // A: the pairing this hub is running for one client.
    struct PairOut {
        Client* client{nullptr};
        QString host;
        quint16 port{0};
        QString typedAddress;  // kept as the peer's manual address
        QString code;          // typed; sent when the peer answers
        Conn* conn{nullptr};
        QString peerName;
        frond::link::InstallId peerId{};
    } pairOut;
};

}  // namespace frondLink
