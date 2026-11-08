#include "LinkClient.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QProcess>
#include "LinkCommon.h"

LinkClient::LinkClient(QObject* parent) : QObject(parent) {
    retry.setSingleShot(true);
    connect(&retry, &QTimer::timeout, this, &LinkClient::attempt);
    linger.setSingleShot(true);
    linger.setInterval(1000);
    connect(&linger, &QTimer::timeout, this, [this] {
        if (sticky.isEmpty() && queue.isEmpty() && socket.bytesToWrite() == 0) socket.disconnectFromServer();
    });
    connect(&socket, &QLocalSocket::connected, this, [this] {
        attempts = 0; startedHub = false;
        for (const auto& op : sticky) socket.write(frondLink::encodeLine(op));
        flush();
        emit connectionChanged(true);
    });
    connect(&socket, &QLocalSocket::readyRead, this, [this] {
        input += socket.readAll();
        for (int at; (at = input.indexOf('\n')) >= 0;) {
            const auto doc = QJsonDocument::fromJson(input.left(at));
            input.remove(0, at + 1);
            if (!doc.isObject()) continue;
            if (doc.object().value("type") == "hello") helloMessage = doc.object();
            emit message(doc.object());
        }
    });
    connect(&socket, &QLocalSocket::disconnected, this, [this] {
        input.clear();
        emit connectionChanged(false);
        // The hub went away: start a new one if anything here still needs it.
        if (!sticky.isEmpty() || !queue.isEmpty()) { attempts = 0; startedHub = false; retry.start(100); }
    });
    connect(&socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        if (socket.state() == QLocalSocket::ConnectedState) return;
        if (sticky.isEmpty() && queue.isEmpty()) return;
        if (!startedHub) {
            startedHub = true;
            QProcess hub;
            hub.setProgram(hubProgram());
            hub.setArguments({"--link-service"});
            hub.setStandardOutputFile(QProcess::nullDevice());
            hub.setStandardErrorFile(QProcess::nullDevice());
            if (hasEnvironment) hub.setProcessEnvironment(environment);
            hub.startDetached();
        }
        // About 5 s for a new hub, then every 2 s while still wanted.
        retry.start(++attempts < 50 ? 100 : 2000);
    });
}

// The socket's signals reach members that are already gone once destruction starts.
LinkClient::~LinkClient() {
    socket.disconnect(this);
    socket.abort();
}

QString LinkClient::hubProgram() {
    const auto custom = qEnvironmentVariable("FROND_LINK_HUB");
    return custom.isEmpty() ? QCoreApplication::applicationFilePath() : custom;
}

void LinkClient::setSticky(const QString& key, const QJsonObject& op) {
    // The hub keeps one infrared and one serial attach per client until it's detached, and a client
    // with the Connections window open stays connected, so a cleared or replaced attach is detached first.
    if ((key == "ir" || key == "serial") && sticky.contains(key) && isConnected())
        socket.write(frondLink::encodeLine({{"op", key + "-detach"}}));
    if (op.isEmpty()) {
        sticky.remove(key);
        if (sticky.isEmpty()) linger.start();
        return;
    }
    sticky[key] = op;
    if (isConnected()) socket.write(frondLink::encodeLine(op));
    else ensure();
}

void LinkClient::send(const QJsonObject& op) {
    queue.push_back(op);
    if (isConnected()) flush();
    else ensure();
}

void LinkClient::flush() {
    for (const auto& op : std::exchange(queue, {})) socket.write(frondLink::encodeLine(op));
    socket.flush();
    if (sticky.isEmpty()) linger.start();
}

void LinkClient::ensure() {
    linger.stop();
    if (socket.state() != QLocalSocket::UnconnectedState || retry.isActive()) return;
    attempt();
}

void LinkClient::attempt() {
    if (socket.state() != QLocalSocket::UnconnectedState) return;
    if (sticky.isEmpty() && queue.isEmpty()) return;
    const auto path = socketPath.isEmpty() ? frondLink::hubSocketPath() : socketPath;
    if (path.isEmpty()) return;
    socket.connectToServer(path);
}
