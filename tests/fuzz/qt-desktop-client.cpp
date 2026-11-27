// The desktop launcher's end of the control socket (DesktopClient::receive): events and replies from
// a core come from the input over a socket pair. The first input byte picks the client's state
// (ready, a request or background query out) so every reply path can be reached. QtCore and
// QtNetwork only.
#include <sys/socket.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

// The harness sets the client's private state directly.
#define private public
#include "../../desktop/DesktopClient.cpp"
#undef private
#include "DesktopClient.moc.cpp"

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
    new QCoreApplication(*argc, *argv);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    if (size < 1 || size > 65536) return 0;
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 0;
    DesktopClient client;
    const uint8_t state = bytes[0];
    client.ready = state & 1;
    static const char* const commands[] = {"", "save", "quit", "sd-create"};
    client.pending = QString::fromLatin1(commands[(state >> 1) & 3]);
    client.serial = (state >> 3) & 3;
    client.backgroundId = (state & 0x20) ? 2 : 0;
    client.backgroundCommand = "sync-state";
    auto* socket = new QLocalSocket(&client);
    socket->setSocketDescriptor(pair[0]);
    client.socket = socket;
    if (send(pair[1], bytes + 1, size - 1, MSG_NOSIGNAL) != ssize_t(size - 1)) std::abort();
    close(pair[1]);
    for (int i = 0; i < 64 && (socket->bytesAvailable() || socket->waitForReadyRead(20)); ++i) client.receive();
    client.socket = nullptr;
    return 0;
}
