// A window's connection to the link hub. Starts the hub if none answers, and starts a new one
// if it goes away, then sends the sticky requests (watch, ir-attach, serial-attach) again so channels reopen.
#pragma once
#include <QJsonObject>
#include <QLocalSocket>
#include <QMap>
#include <QObject>
#include <QProcessEnvironment>
#include <QTimer>

class LinkClient : public QObject {
    Q_OBJECT
 public:
    explicit LinkClient(QObject* parent = nullptr);
    ~LinkClient() override;
    // The hub executable: FROND_LINK_HUB, else this program.
    static QString hubProgram();
    // Sent now and after every reconnect. An empty object drops it; with none left the
    // connection closes so the hub can exit.
    void setSticky(const QString& key, const QJsonObject& op);
    bool hasSticky(const QString& key) const { return sticky.contains(key); }
    // One-off request; connects first if needed.
    void send(const QJsonObject& op);
    bool isConnected() const { return socket.state() == QLocalSocket::ConnectedState; }
    QJsonObject hello() const { return helloMessage; }
    // Tests run several hubs: each client names its socket and the environment a hub it starts gets.
    void setSocketPath(const QString& path) { socketPath = path; }
    void setHubEnvironment(const QProcessEnvironment& env) { environment = env; hasEnvironment = true; }
 signals:
    void message(const QJsonObject& message);
    void connectionChanged(bool connected);
 private:
    void ensure();
    void attempt();
    void flush();
    QLocalSocket socket;
    QByteArray input;
    QMap<QString, QJsonObject> sticky;
    QList<QJsonObject> queue;
    QJsonObject helloMessage;
    QTimer retry, linger;
    QString socketPath;
    QProcessEnvironment environment;
    bool hasEnvironment{false};
    int attempts{0};
    bool startedHub{false};
};
