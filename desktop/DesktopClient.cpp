#include "DesktopClient.h"
#include <QDir>

DesktopClient::DesktopClient(QObject* parent) : QObject(parent) {
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, this, &DesktopClient::stalled);
    connect(&server, &QLocalServer::newConnection, this, [this] {
        auto* connection = server.nextPendingConnection();
        if (socket) { connection->deleteLater(); return; }
        socket = connection;
        server.close();
        connect(socket, &QLocalSocket::readyRead, this, [this, connection] { if (socket == connection) receive(); });
        connect(socket, &QLocalSocket::disconnected, this, [this, connection] {
            if (socket != connection) return;
            if (ready && !expectingExit && running()) emit error(tr("The emulator control connection closed."));
            ready = false;
        });
        receive();
    });
    connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
        emit diagnostic(QString::fromLocal8Bit(process.readAllStandardOutput()));
    });
    connect(&process, &QProcess::readyReadStandardError, this, [this] {
        emit diagnostic(QString::fromLocal8Bit(process.readAllStandardError()));
    });
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) { fail(process.errorString()); emit finished(); }
    });
    connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
        if (!expectingExit && (status == QProcess::CrashExit || code != 0))
            emit error(tr("The emulator stopped unexpectedly. See View → Emulator Log for details."));
        deadline.stop(); ready = false; pending.clear();
        if (socket) { socket->disconnect(this); socket->deleteLater(); socket = nullptr; }
        server.close(); directory.reset();
        emit finished();
    });
}
DesktopClient::~DesktopClient() {
    // Only our child process is stopped; an interrupted save keeps the previous file.
    server.disconnect(this); deadline.disconnect(this);
    if (socket) socket->disconnect(this);
    process.disconnect(this);
    if (running()) { process.kill(); process.waitForFinished(3000); }
}
bool DesktopClient::start(const QString& core, const QStringList& arguments) {
    if (running()) return false;
    if (socket) { socket->disconnect(this); socket->deleteLater(); socket = nullptr; }
    server.close(); input.clear(); pending.clear(); ready = false; expectingExit = false;
    directory = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/frond-XXXXXX");
    if (!directory->isValid()) { emit error(tr("Could not create the private control directory.")); return false; }
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(directory->path() + "/control")) { emit error(server.errorString()); return false; }
    process.setProgram(core);
    process.setArguments(arguments + QStringList{"--desktop-socket", server.fullServerName()});
    deadline.start(30000);
    process.start();
    return true;
}
bool DesktopClient::request(const QString& command, const QString& argument) {
    if (!available() || !socket) return false;
    pending = command;
    if (command == "quit") expectingExit = true;
    const auto line = QByteArray::number(++serial) + '\t' + command.toUtf8() + '\t' + argument.toUtf8().toHex() + '\n';
    if (line.size() > 16384 || socket->write(line) != line.size()) {
        pending.clear(); emit error(tr("Could not send the emulator command.")); return false;
    }
    deadline.start(120000);
    return true;
}
void DesktopClient::receive() {
    if (!socket) return;
    input += socket->readAll();
    if (input.size() > 65536) return fail(tr("The emulator sent an oversized control response."));
    while (input.contains('\n')) {
        const auto at = input.indexOf('\n');
        const auto fields = input.first(at).split('\t');
        input.remove(0, at + 1);
        if (fields.size() != 4) return fail(tr("The emulator sent an invalid control response."));
        if (fields[0] == "0" && fields[1] == "event") {
            if (fields[2] == "ready" && !ready) { ready = true; deadline.stop(); emit started(); }
            else if (fields[2] == "close" && ready) emit closeRequested();
            else if (fields[2] == "serial" && ready && fields[3].size() % 2 == 0 &&
                     QByteArray::fromHex(fields[3]).toHex() == fields[3])
                emit serialState(QString::fromUtf8(QByteArray::fromHex(fields[3])));
            else if (fields[2] == "usb" && ready && fields[3].size() % 2 == 0 &&
                     QByteArray::fromHex(fields[3]).toHex() == fields[3])
                emit usbState(QString::fromUtf8(QByteArray::fromHex(fields[3])));
            else if (fields[2] == "infrared" && ready && fields[3].size() % 2 == 0 &&
                     QByteArray::fromHex(fields[3]).toHex() == fields[3])
                emit infraredState(QString::fromUtf8(QByteArray::fromHex(fields[3])));
            else if (fields[2] == "bluetooth" && ready && fields[3].size() % 2 == 0 &&
                     QByteArray::fromHex(fields[3]).toHex() == fields[3])
                emit bluetoothState(QString::fromUtf8(QByteArray::fromHex(fields[3])));
            else return fail(tr("The emulator sent an unexpected event."));
            continue;
        }
        if (pending.isEmpty() || fields[0] != QByteArray::number(serial) ||
            (fields[1] != "ok" && fields[1] != "error") || fields[3].size() % 2)
            return fail(tr("The emulator sent an unexpected command response."));
        const auto decoded = QByteArray::fromHex(fields[3]);
        if (decoded.toHex() != fields[3]) return fail(tr("The emulator sent invalid response text."));
        const auto command = pending;
        pending.clear(); deadline.stop();
        if (command == "quit") { ready = false; deadline.start(10000); }
        emit result(command, fields[1] == "ok", QString::fromUtf8(fields[2]), QString::fromUtf8(decoded));
    }
}
void DesktopClient::fail(const QString& message) {
    ready = false; deadline.stop();
    emit error(message);
    emit stalled();
}
void DesktopClient::forceStop() { expectingExit = true; if (running()) process.kill(); }
