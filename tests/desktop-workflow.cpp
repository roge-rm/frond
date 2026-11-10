#include <QtTest>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QListWidget>
#include <QStandardPaths>
#include <QToolButton>
#include <QMenu>
#include <QInputDialog>
#include <QProgressDialog>
#include <QPlainTextEdit>
#include <QStatusBar>
#include "../uarm/MemoryStickImage.h"
#include "Dialogs.h"
#include "FirmwareStore.h"
#include "SessionStore.h"
#include "RunningBar.h"
#include "PalmView.h"
#include <QShortcut>
#include "AudioQualification.h"
#include <QMessageBox>
#include <QMenuBar>
#include <QTimer>
#include <QDeadlineTimer>
#include <QAction>
#include <QDialog>
#include <QFileDialog>
#include "SdImage.h"
#include <QSettings>
#include <QSpinBox>
#include <QSlider>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QImage>
#include "MainWindow.h"
#include "LicencesDialog.h"
#include "LinkClient.h"
#include "LinkCommon.h"
#include "ConnectionsWindow.h"
#include "CardsWindow.h"
#include "CardFilesWindow.h"
#include <QTreeWidget>
#include <QRadioButton>
#include <QStandardItemModel>
#include <QDirIterator>
#include <QStorageInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTcpServer>
#include <QUdpSocket>
#include <atomic>
#include <csignal>
#include <functional>
#include <memory>
#include <QShortcut>
#include <QWidgetAction>
#include <signal.h>
#include <sys/time.h>
#include "LinkStore.h"
#include "../link/link.h"

static QByteArray digest(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return hash.result();
}
// Opens the Card panel of the window `inside` belongs to.
static void showCardPanel(QWidget* inside) { inside->window()->findChild<PanelHost*>("panelHost")->open(1); }
// ---- Connections: hubs on loopback, driven like windows through LinkClient ----
static QString fakeRadio();
struct TestHub {
    // A unix socket path is limited to 107 characters, so the hub lives in a short folder under the
    // system temp folder, not under the build folder.
    std::shared_ptr<QTemporaryDir> shortRoot;
    QString root, run, data;
    QProcessEnvironment env;
    TestHub(const QString&, const QString& tag, quint16 port) {
        shortRoot = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/fh-XXXXXX");
        root = shortRoot->path() + "/" + tag; run = root + "/run"; data = root + "/data";
        QDir().mkpath(run); QDir().mkpath(data);
        QFile::setPermissions(run, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        env = QProcessEnvironment::systemEnvironment();
        env.insert("XDG_RUNTIME_DIR", run); env.insert("XDG_DATA_HOME", data);
        env.insert("FROND_LINK_PORT", QString::number(port)); env.insert("FROND_LINK_DISCOVERY_PORT", "0");
        env.insert("FROND_BTVIRT", fakeRadio());
        env.remove("FROND_LINK_SOCKET");
    }
    QString store() const { return data + "/frond/connections"; }
    QString socket() const { return run + "/frond/link.sock"; }
};
// A window's side of the hub: the client and every message it got.
struct HubUser {
    LinkClient client;
    QList<QJsonObject> got;
    explicit HubUser(const TestHub& hub) {
        client.setSocketPath(hub.socket()); client.setHubEnvironment(hub.env);
        QObject::connect(&client, &LinkClient::message, &client, [this](const QJsonObject& m) { got.push_back(m); });
    }
    // The hub the windows use, with this process's environment.
    explicit HubUser(const QString& socket) {
        client.setSocketPath(socket);
        QObject::connect(&client, &LinkClient::message, &client, [this](const QJsonObject& m) { got.push_back(m); });
    }
    QJsonObject last(const QString& type) const {
        for (auto it = got.rbegin(); it != got.rend(); ++it) if (it->value("type").toString() == type) return *it;
        return {};
    }
    QString ir() const { return last("ir").value("state").toString(); }
    QString serial() const { return last("serial").value("state").toString(); }
    QString bt() const { return last("bt").value("state").toString(); }
};
// Stands in for a core's IR socket.
struct FakeIr {
    QLocalServer server;
    QPointer<QLocalSocket> peer;
    QByteArray got;
    QString path;
    explicit FakeIr(const QString& where) : path(where) {
        QObject::connect(&server, &QLocalServer::newConnection, &server, [this] {
            while (auto* s = server.nextPendingConnection()) {
                peer = s;
                QObject::connect(s, &QLocalSocket::readyRead, s, [this, s] { got += s->readAll(); });
            }
        });
        listen();
    }
    bool listen() {
        QLocalServer::removeServer(path);
        return server.listen(path);
    }
    void sleep() { if (peer) peer->abort(); server.close(); QFile::remove(path); }
    void send(const QByteArray& bytes) { if (peer) { peer->write(bytes); peer->flush(); } }
};
// Stands in for a core's serial socket. With RTS low it stops reading, as the core does, and only
// a little sits in its socket's buffer.
struct FakeSerial {
    QLocalServer server;
    QPointer<QLocalSocket> peer;
    QByteArray got;
    QString path;
    bool rts{true};
    int opens{0};
    explicit FakeSerial(const QString& where) : path(where) {
        QObject::connect(&server, &QLocalServer::newConnection, &server, [this] {
            while (auto* s = server.nextPendingConnection()) {
                peer = s;
                ++opens;
                s->setReadBufferSize(1024);
                QObject::connect(s, &QLocalSocket::readyRead, s, [this, s] { if (rts && s == peer) got += s->readAll(); });
            }
        });
        QLocalServer::removeServer(path);
        server.listen(path);
    }
    bool open() const { return peer && peer->state() == QLocalSocket::ConnectedState; }
    void setRts(bool on) { rts = on; if (on && peer) got += peer->readAll(); }
    void send(const QByteArray& bytes) { if (peer) peer->write(bytes); }
    qint64 unsent() const { return peer ? peer->bytesToWrite() : 0; }
};
// Stands in for btvirt (FROND_BTVIRT): each connection is a controller and bytes from one reach all
// the others, as controllers in one btvirt hear each other. Each connection adds a line to
// <folder>/opened.
static QString fakeRadio() {
    const auto path = QDir::currentPath() + "/build/fake-btvirt.py";
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(R"PY(#!/usr/bin/python3
import os,selectors,socket,sys
folder=next(a[2:] for a in sys.argv[1:] if a.startswith('-s'))
path=os.path.join(folder,'bt-server-bredr')
try: os.unlink(path)
except FileNotFoundError: pass
server=socket.socket(socket.AF_UNIX);server.bind(path);server.listen(16)
sel=selectors.DefaultSelector();sel.register(server,selectors.EVENT_READ)
peers=[]
while True:
    for key,_ in sel.select():
        if key.fileobj is server:
            c,_=server.accept();peers.append(c);sel.register(c,selectors.EVENT_READ)
            with open(os.path.join(folder,'opened'),'a') as o: o.write('x\n')
            continue
        c=key.fileobj
        try: data=c.recv(65536)
        except OSError: data=b''
        if not data:
            sel.unregister(c);peers.remove(c);c.close();continue
        for p in peers:
            if p is not c:
                try: p.sendall(data)
                except OSError: pass
)PY");
        f.close();
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    return path;
}
// Stands in for the Bluetooth phone (FROND_BT_PHONE): writes its arguments to <radio folder>/phone
// and a line to <radio folder>/phone-stopped when it's stopped. Once <radio folder>/phone-dial
// exists it reports a call: dialling, then online a second later.
static QString fakePhone() {
    const auto path = QDir::currentPath() + "/build/fake-bt-phone.py";
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(R"PY(#!/usr/bin/python3
import os,signal,sys,time
folder=os.path.dirname(sys.argv[1])
def stop(*_):
    with open(os.path.join(folder,'phone-stopped'),'a') as o: o.write('x\n')
    sys.exit(0)
signal.signal(signal.SIGTERM,stop)
with open(os.path.join(folder,'phone'),'w') as o: o.write('\n'.join(sys.argv[1:])+'\n')
while not os.path.exists(os.path.join(folder,'phone-dial')): time.sleep(0.05)
print('DUN_STATE dialling',flush=True)
time.sleep(1)
print('DUN_STATE online 33600',flush=True)
while True: time.sleep(1)
)PY");
        f.close();
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    return path;
}
static int radioOpened(const QString& runtime) {
    QFile f(runtime + "/frond/radio/opened");
    return f.open(QIODevice::ReadOnly) ? int(f.readAll().count('\n')) : 0;
}
// Stands in for a core's Bluetooth: each connection to the hub's socket is a controller.
struct FakeBt {
    QLocalSocket s;
    QByteArray got;
    QString path;
    bool closed{false};
    explicit FakeBt(const QString& where) : path(where) {
        QObject::connect(&s, &QLocalSocket::readyRead, &s, [this] { got += s.readAll(); });
        QObject::connect(&s, &QLocalSocket::disconnected, &s, [this] { closed = true; });
    }
    bool connect() {
        closed = false; got.clear();
        s.abort(); s.connectToServer(path);
        return s.waitForConnected(2000);
    }
    // Connects once the hub listens. Not inside QTRY_VERIFY, which evaluates its expression
    // more than once after it holds, and each connect() makes a new controller.
    bool connectWithin(int ms) {
        QDeadlineTimer deadline(ms);
        while (!connect()) {
            if (deadline.hasExpired()) return false;
            QTest::qWait(50);
        }
        return true;
    }
    void send(const QByteArray& bytes) { s.write(bytes); s.flush(); }
};
static QByteArray pattern(int size, int seed) {
    QByteArray b(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i) b[i] = char((i * 7 + seed) % 251);
    return b;
}
static quint16 freePort() {
    QTcpServer probe; probe.listen(QHostAddress::LocalHost, 0);
    return probe.serverPort();
}
static quint16 freeUdpPort() {
    QUdpSocket probe; probe.bind(QHostAddress::AnyIPv4, 0);
    return probe.localPort();
}
// A hub process that's gone or a zombie.
static bool alive(qint64 pid) {
    if (pid <= 0 || ::kill(pid_t(pid), 0) != 0) return false;
    QFile stat(QString("/proc/%1/stat").arg(pid));
    if (!stat.open(QIODevice::ReadOnly)) return false;
    return QString::fromLatin1(stat.readAll()).section(')', -1).trimmed().left(1) != "Z";
}
static QJsonObject listed(const HubUser& user, const QString& list, const QString& id) {
    for (const auto& e : user.last("state").value(list).toArray())
        if (e.toObject().value("id").toString() == id) return e.toObject();
    return {};
}
// The hello a hub sends on its socket, read with a plain connection.
static QJsonObject hubHello(const QString& path) {
    QLocalSocket s; s.connectToServer(path);
    if (!s.waitForConnected(2000)) return {};
    QByteArray line;
    while (!line.contains('\n') && s.waitForReadyRead(2000)) line += s.readAll();
    return QJsonDocument::fromJson(line.left(line.indexOf('\n'))).object();
}
// Runs f once on the next T that is the active modal dialog or popup, polling every 20 ms.
template <typename T>
static void whenShown(std::function<void(T*)> f) {
    auto* timer = new QTimer;
    timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, timer, [timer, f] {
        auto* w = qobject_cast<T*>(QApplication::activeModalWidget());
        if (!w) w = qobject_cast<T*>(QApplication::activePopupWidget());
        if (!w || !w->isVisible()) return;
        timer->stop(); timer->deleteLater();
        f(w);
    });
    timer->start();
}
static void typeInto(const QString& text) {
    whenShown<QInputDialog>([text](QInputDialog* d) { d->setTextValue(text); d->accept(); });
}
static void pickMenu(const QString& action) {
    whenShown<QMenu>([action](QMenu* m) {
        auto* a = m->findChild<QAction*>(action);
        if (!a) { m->close(); return; }
        m->setActiveAction(a);
        QTest::keyClick(m, Qt::Key_Return);
    });
}
static void killHub(const HubUser& user) {
    const auto pid = qint64(user.client.hello().value("pid").toDouble());
    if (pid > 0) ::kill(pid_t(pid), SIGKILL);
}
static QJsonObject storedPeer(const TestHub& hub, const QString& id) {
    QFile f(hub.store() + "/connections.json");
    if (!f.open(QIODevice::ReadOnly)) return {};
    for (const auto& e : QJsonDocument::fromJson(f.readAll()).object().value("entries").toArray())
        if (e.toObject().value("id").toString() == id) return e.toObject();
    return {};
}
// a pairs with b by address, typing a wrong code first when asked. b's user watches.
static bool pairHubs(HubUser& a, HubUser& b, bool wrongFirst) {
    b.client.setSticky("watch", {{"op", "watch"}});
    if (!QTest::qWaitFor([&] { return !b.last("hello").isEmpty(); }, 10000)) return false;
    b.got.clear(); a.got.clear();
    const auto port = b.client.hello().value("port").toInt();
    a.client.send({{"op", "pair"}, {"address", "127.0.0.1:" + QString::number(port)}});
    if (!QTest::qWaitFor([&] { return !a.last("pair-peer").isEmpty() && !b.last("pair-request").isEmpty(); }, 10000)) return false;
    const auto code = b.last("pair-request").value("code").toString().remove(' ');
    if (wrongFirst) {
        a.client.send({{"op", "pair-code"}, {"code", QString("%1").arg((code.toInt() + 1) % 1000000, 6, 10, QChar('0'))}});
        if (!QTest::qWaitFor([&] { return a.last("pair-result").value("result") == "wrongcode" &&
                                         b.last("pair-request-update").value("tries").toInt() == 2; }, 10000)) return false;
    }
    a.client.send({{"op", "pair-code"}, {"code", code}});
    return QTest::qWaitFor([&] { return a.last("pair-result").value("result") == "paired" &&
                                        b.last("pair-request-end").value("result") == "paired"; }, 10000);
}

// ---- Cards: a library beside the test's sessions, real card files from the core's card tool ----
static QString cardsData() { return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/frond"; }
static QString cardsSessions() { return cardsData() + "/sessions"; }
static QString cardsLibrary() { return cardsData() + "/cards"; }
static void cardsReset() {
    QDir(cardsSessions()).removeRecursively();
    QDir(cardsLibrary()).removeRecursively();
    QDir().mkpath(cardsSessions());
}
// The ARM core built beside the desktop: out/frond from build/desktop.
static QString cardsCore() {
    const auto env = qEnvironmentVariable("FROND_TEST_CORE");
    return env.isEmpty() ? QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../../out/frond") : env;
}
static bool cardTool(const QStringList& args) {
    QProcess p;
    p.start(cardsCore(), QStringList{"--card"} + args);
    return p.waitForFinished(120000) && p.exitCode() == 0;
}
static void putLe(QByteArray& b, int at, quint64 v, int n) {
    for (int i = 0; i < n; ++i) b[at + i] = char(v >> (8 * i));
}
struct TestMedia {
    quint32 kind{1};
    quint64 logical{32ull << 20}, physical{32ull << 20}, generation{1};
    QByteArray identity{QByteArray("c4c4c4c4frondcardstoretest0251c4")};
};
// A v8 .frond with a device header and media descriptors, enough for the store and a fake core.
static void writeFrond(const QString& path, const QString& device, const QList<TestMedia>& media) {
    QByteArray b(8 + 32 + 52, '\0');
    putLe(b, 0, frondSessions::magic, 4); putLe(b, 4, 8, 4);
    const auto id = device.toLatin1();
    for (int i = 0; i < id.size(); ++i) b[8 + i] = id[i];
    putLe(b, 88, quint64(media.size()), 4);
    for (const auto& m : media) {
        QByteArray d(100, '\0');
        putLe(d, 0, m.kind, 4);
        const QByteArray name = m.kind == 1 ? "sd.frondsd" : m.kind == 2 ? "ms.frondms" : "ms.frondmsp";
        for (int i = 0; i < name.size(); ++i) d[4 + i] = name[i];
        putLe(d, 36, 1, 4);
        for (int i = 0; i < 32; ++i) d[44 + i] = m.identity.leftJustified(32, '\0', true)[i];
        putLe(d, 76, m.logical, 8); putLe(d, 84, m.physical, 8); putLe(d, 92, m.generation, 8);
        b += d;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path); f.open(QIODevice::WriteOnly); f.write(b);
}
// A saved TX machine in the sessions folder; with a card, a 32 MB SD card at generation 1.
// Each machine's card has its own serial, from the machine's name.
static QByteArray machineIdentity(const QString& name) { return QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Md5).toHex(); }
static QString cardsMachine(const QString& name, bool card, const QString& device = "palm-tx") {
    const auto dir = cardsSessions() + '/' + name;
    TestMedia media;
    media.identity = machineIdentity(name);
    writeFrond(dir + '/' + name + ".frond", device, card ? QList<TestMedia>{media} : QList<TestMedia>{});
    if (card) cardTool({"make", "sd", "32", dir + "/sd.frondsd"});
    return dir;
}
// Rewrites a v2 card file's generation and header CRC, as a save by another machine would.
static bool setCardGeneration(const QString& path, quint64 generation) {
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite)) return false;
    auto hdr = f.read(64);
    const auto payload = frondSessions::le64(hdr, 16);
    const quint64 chunks = (payload + (1u << 20) - 1) >> 20;
    f.seek(qint64(64 + payload));
    const auto table = f.read(qint64(4 * chunks));
    putLe(hdr, 28, generation, 8); putLe(hdr, 24, 0, 4);
    uLong crc = crc32(0, reinterpret_cast<const Bytef*>(hdr.constData()), 64);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(table.constData()), uInt(table.size()));
    putLe(hdr, 24, quint32(crc), 4);
    return f.seek(0) && f.write(hdr) == 64;
}
static quint64 cardGeneration(const QString& path) {
    quint64 g = 0;
    frondCards::cardHeader(path, &g, nullptr);
    return g;
}
static frondCards::Card onlyCard() {
    const auto all = frondCards::entries(cardsLibrary());
    return all.size() == 1 ? all[0].card : frondCards::Card{};
}
static frondCards::Card cardNamed(const QString& name) {
    for (const auto& e : frondCards::entries(cardsLibrary()))
        if (e.card.name == name) return e.card;
    return {};
}
// Answers like a core and saves by keeping the session file it was started on. Writes its
// arguments to <session>.args.
static QString fakeCardCore(const QString& where) {
    QFile core(where + "/card-core");
    core.open(QIODevice::WriteOnly);
    core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,sys
pathlib.Path(sys.argv[1]+'.args').write_text('\n'.join(sys.argv[1:]))
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    message={b'keyboard-state':b'detached',b'network-state':b'disabled',b'sd-state':b'inserted'}.get(cmd,b'done')
    if cmd==b'save':
        dest=pathlib.Path(bytes.fromhex(arg.decode()).decode())
        if dest.resolve()!=pathlib.Path(sys.argv[1]).resolve(): dest.write_bytes(pathlib.Path(sys.argv[1]).read_bytes())
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit': break
)PY");
    core.close();
    core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return core.fileName();
}
// Answers like a core, logs each install argument to <where>/installs in order, and fails files
// whose name starts with "bad".
static QString fakeInstallCore(const QString& where) {
    QFile core(where + "/install-core");
    core.open(QIODevice::WriteOnly);
    core.write(R"PY(#!/usr/bin/python3
import os,pathlib,socket,sys
log=pathlib.Path(sys.argv[0]).parent/'installs'
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    if cmd==b'install':
        name=bytes.fromhex(arg.decode()).decode()
        with log.open('a') as f: f.write(name+'\n')
        if pathlib.Path(name).name.startswith('die') and not log.with_name('died').exists():
            log.with_name('died').write_text('x'); os._exit(1)
        if pathlib.Path(name).name.startswith('bad'):
            s.sendall(i+b'\terror\tinstall\t'+b'Not a Palm database.'.hex().encode()+b'\n'); continue
    message={b'keyboard-state':b'detached',b'network-state':b'disabled',b'sd-state':b'inserted'}.get(cmd,b'done')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit': break
)PY");
    core.close();
    core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return core.fileName();
}
static QString argsOf(const QString& machineDir) {
    QFile f(machineDir + '/' + QFileInfo(machineDir).fileName() + ".frond.args");
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}
// Runs a machine in `window` until it is up, then closes it (final save, quit).
static bool runAndClose(MainWindow& window, const QString& dir) {
    window.show();
    window.openSession(dir + '/' + QFileInfo(dir).fileName() + ".frond", true);
    if (!QTest::qWaitFor([&] { return window.findChild<QPushButton*>("stop")->isEnabled(); }, 60000)) return false;
    window.close();
    return QTest::qWaitFor([&] { return !window.isVisible(); }, 120000);
}

// The running card picker: New card of `type` and `size` (labels as shown), then OK, or with
// `cancel` the card tool's progress is cancelled and the picker closed. Returns the size labels the
// New card dialog offered for `type`, greyed ones followed by " (greyed)".
static QStringList pickNewCard(MainWindow& window, const QString& type, const QString& size, bool cancel, bool* sawProgress = nullptr) {
    QStringList offered;
    int stage = 0;  // 0 picker, 1 New card done, 2 picker closed
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, &window, [&] {
        if (auto* job = window.findChild<QProgressDialog*>("cardJob"); job && job->isVisible()) {
            if (sawProgress) *sawProgress = true;
            if (cancel && !job->property("pressed").toBool())
                if (auto* b = job->findChild<QPushButton*>()) {
                    job->setProperty("pressed", true);
                    QTimer::singleShot(0, b, &QPushButton::click);
                }
            return;
        }
        auto* modal = QApplication::activeModalWidget();
        if (auto* d = qobject_cast<NewCardDialog*>(modal); d && stage == 0) {
            d->type->setCurrentIndex(d->type->findText(type));
            for (int i = 0; i < d->sizes->count(); ++i) {
                auto* item = qobject_cast<QStandardItemModel*>(d->sizes->model())->item(i);
                offered << d->sizes->itemText(i) + (item->isEnabled() ? QString() : QStringLiteral(" (greyed)"));
            }
            d->sizes->setCurrentIndex(d->sizes->findText(size));
            stage = 1;
            QTimer::singleShot(0, d, &QDialog::accept);
        } else if (auto* p = qobject_cast<CardPickerDialog*>(modal); p && stage == 0 && !p->property("asked").toBool()) {
            p->setProperty("asked", true);
            QTimer::singleShot(0, p->newCard, &QPushButton::click);
        } else if (p && stage == 1) {
            stage = 2;
            if (cancel) QTimer::singleShot(0, p, &QDialog::reject);
            else QTimer::singleShot(0, p, &QDialog::accept);
        }
    });
    poll.start();
    QTimer::singleShot(0, window.findChild<QPushButton*>("cardPick"), &QPushButton::click);
    QTest::qWaitFor([&] { return stage == 2 && !QApplication::activeModalWidget(); }, 120000);
    return offered;
}

// A card tool that reports progress and runs until it is stopped, for Cancel. Set as
// FROND_CARD_TOOL for as long as the object lives.
struct SlowCardTool {
    QTemporaryDir dir;
    QByteArray before{qgetenv("FROND_CARD_TOOL")};
    SlowCardTool() {
        QFile f(dir.filePath("tool"));
        f.open(QIODevice::WriteOnly);
        f.write("#!/usr/bin/python3\nimport sys,time\nprint('progress 1/100',flush=True)\ntime.sleep(60)\nprint('ok sd 1',flush=True)\n");
        f.close();
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv("FROND_CARD_TOOL", f.fileName().toUtf8());
    }
    ~SlowCardTool() { qputenv("FROND_CARD_TOOL", before); }
};

// A core for the card controls: SD put-in, eject and take-out as the real one answers them, and
// saves that copy the session and its card file to another folder. Writes its commands to
// <where>/commands.
static QString pickerCore(const QString& where, const QString& device) {
    QFile core(where + "/picker-core");
    core.open(QIODevice::WriteOnly);
    auto script = QByteArray(R"PY(#!/usr/bin/python3
import os,pathlib,shutil,socket,sys,time
trace=pathlib.Path(__file__).with_name('commands')
pathlib.Path(__file__).with_name('pid').write_text(str(os.getpid()))
s=socket.socket(socket.AF_UNIX);s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
src=pathlib.Path(sys.argv[1])
cards=('sd.frondsd','ms.frondms','ms.frondmsp')
card='inserted' if any((src.parent/n).exists() for n in cards) else 'none'
flag=lambda n: pathlib.Path(__file__).with_name(n).exists()
if card=='inserted' and flag('start-ejected'): card='ejected'
identity='00'*32
for line in s.makefile('rb'):
    i,raw,arg=line.rstrip(b'\n').split(b'\t'); cmd=raw.decode(); arg=bytes.fromhex(arg.decode()).decode()
    with trace.open('a') as t: t.write(cmd+'\t'+arg+'\n')
    message='done'
    if flag('fail-'+cmd):
        s.sendall(i+b'\terror\tresult\t'+('Injected '+cmd+' failure').encode().hex().encode()+b'\n'); continue
    # A Memory Stick moves the same way as an SD card.
    if cmd.startswith('ms-'): cmd='sd-'+cmd[3:]
    if cmd=='device-state': message='DEVICE'
    elif cmd=='sd-protection': message='writable'
    elif cmd=='sd-state': message=card
    elif cmd=='network-state': message='disabled'
    elif cmd=='keyboard-state': message='detached'
    elif cmd=='sd-put-in': card='inserted'; identity=arg.split(' ')[0]
    elif cmd=='sd-eject': card='ejected'
    elif cmd=='sd-take-out': card='none'; message=identity+' writable'
    elif cmd=='save':
        if flag('slow-save'): time.sleep(1.5)
        dest=pathlib.Path(arg)
        if dest.resolve()!=src.resolve():
            dest.write_bytes(src.read_bytes())
            for n in cards:
                if (src.parent/n).exists(): shutil.copyfile(src.parent/n, dest.parent/n)
    s.sendall(i+b'\tok\tresult\t'+message.encode().hex().encode()+b'\n')
    if cmd=='sd-take-out' and flag('die-after-take-out'):
        s.shutdown(socket.SHUT_RDWR); time.sleep(.3); sys.exit(1)
    if cmd=='quit': break
)PY");
    script.replace("DEVICE", device.toUtf8());
    core.write(script);
    core.close();
    core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return core.fileName();
}
// A drawer card whose file is only its size, for lists that don't read it.
static QString sparseCard(const QString& name, const QString& kind, qint64 bytes) {
    frondCards::Card card{frondCards::newId(), name, kind, frondCards::now(), {}, bytes, frondCards::newIdentity(), false};
    const auto entry = cardsLibrary() + '/' + card.id;
    QDir().mkpath(entry);
    frondCards::writeCard(entry, card);
    QFile f(entry + '/' + frondCards::fileOfKind(kind));
    f.open(QIODevice::WriteOnly);
    f.resize(bytes);
    return card.id;
}
static void screenshot(QWidget* w, const QString& name) {
    const auto dir = qEnvironmentVariable("FROND_SCREENSHOTS");
    if (dir.isEmpty()) return;
    QDir().mkpath(dir);
    w->grab().save(dir + '/' + name);
}
static void answerYes() {
    whenShown<QMessageBox>([](QMessageBox* m) {
        for (auto* b : m->buttons()) {
            const auto role = m->buttonRole(b);
            if (role == QMessageBox::YesRole || role == QMessageBox::AcceptRole || role == QMessageBox::DestructiveRole) { b->click(); return; }
        }
    });
}
static QString rowLabel(QListWidget* l, int i, const char* what) {
    return l->itemWidget(l->item(i))->findChild<QLabel*>(what)->text();
}
static QAction* cardAction(QListWidget* l, int i, const char* action) {
    return l->itemWidget(l->item(i))->findChild<QPushButton*>("cardMenu")->menu()->findChild<QAction*>(action);
}

class WorkflowTest : public QObject {
    Q_OBJECT
 private slots:
    void fastAudioLaunchOption() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        QFile source(files.filePath("input.frond"));
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write("session"); source.close();
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,sys
pathlib.Path(sys.argv[1]+'.args').write_text('\n'.join(sys.argv[1:]))
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    message={b'keyboard-state':b'detached',b'network-state':b'disabled',b'sd-state':b'none'}.get(cmd,b'done')
    if cmd==b'save': pathlib.Path(bytes.fromhex(arg.decode()).decode()).write_text('saved')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit': break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QSettings().setValue("fastAudio", true);
        QSettings().setValue("windowScale", 7);
        QSettings().setValue("smoothLcd", true);
        {
            MainWindow window(core.fileName()); window.show();
            window.openSession(source.fileName(), true);
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("stop")->isEnabled(), 5000);
            QFile args(source.fileName()+".args"); QVERIFY(args.open(QIODevice::ReadOnly));
            const auto arguments = args.readAll();
            // Unqualified firmware gets the queue-only mode.
            QVERIFY(!arguments.contains("--fast-audio") && arguments.contains("--low-latency-queue"));
            QVERIFY(arguments.contains("--window-scale\n1.75"));
            QVERIFY(arguments.contains("--smooth-lcd"));
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        }
        MainWindow next(core.fileName());
        frondSessions::Preferences prefs;
        QVERIFY(frondSessions::readPreferences(QSettings().value("lastSession").toString(), &prefs));
        QCOMPARE(prefs.scale, 7); QVERIFY(prefs.smooth && prefs.fastAudio);
        QSettings().clear();
    }
    void serialSetting() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        QFile source(files.filePath("input.frond"));
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write("session"); source.close();
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,sys,threading,time
pathlib.Path(sys.argv[1]+'.args').write_text('\n'.join(sys.argv[1:]))
trace=pathlib.Path(sys.argv[1]+'.commands')
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
lock=threading.Lock()
def send(data):
    with lock: s.sendall(data)
s.sendall(b'0\tevent\tready\t\n')
state=b'tcp 127.0.0.1:5099 waiting'
def peer():
    # A client connects and later hangs up; the core pushes each change.
    for event in (b'tcp 127.0.0.1:5099 connected', b'tcp 127.0.0.1:5099 waiting'):
        time.sleep(.6)
        send(b'0\tevent\tserial\t'+event.hex().encode()+b'\n')
pushed=False
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    arg=bytes.fromhex(arg.decode())
    with trace.open('ab') as t: t.write(cmd+b' '+arg+b'\n')
    if cmd==b'serial-mode':
        if arg==b'tcp:80':
            send(i+b'\terror\tserial\t'+b'Could not listen on 127.0.0.1:80'.hex().encode()+b'\n'); continue
        state={b'pty':b'pty /dev/pts/9 connected',b'off':b'off'}.get(arg,b'tcp 127.0.0.1:'+arg[4:]+b' waiting')
    message={b'keyboard-state':b'detached',b'network-state':b'disabled',b'sd-state':b'none',
             b'serial-state':state,b'serial-mode':state}.get(cmd,b'done')
    if cmd==b'save': pathlib.Path(arg.decode()).write_text('saved')
    send(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'serial-state' and not pushed:
        pushed=True; threading.Thread(target=peer,daemon=True).start()
    if cmd==b'quit': break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto commands = [&] {
            QFile trace(source.fileName() + ".commands");
            return trace.open(QIODevice::ReadOnly) ? QString::fromUtf8(trace.readAll()) : QString();
        };
        {
            MainWindow window(core.fileName()); window.show();
            auto* mode = window.findChild<QComboBox*>("serialMode"); QVERIFY(mode);
            auto* port = window.findChild<QSpinBox*>("serialPort"); QVERIFY(port);
            auto* state = window.findChild<QLabel*>("serialState"); QVERIFY(state);
            QCOMPARE(mode->currentData().toString(), QString("off"));
            QVERIFY(!port->isEnabled());
            // Off by default: no flag and no serial-state request.
            window.openSession(source.fileName(), true);
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("stop")->isEnabled(), 5000);
            QTRY_VERIFY_WITH_TIMEOUT(commands().contains("keyboard-state"), 5000);
            QFile args(source.fileName() + ".args"); QVERIFY(args.open(QIODevice::ReadOnly));
            QVERIFY(!args.readAll().contains("--serial"));
            QVERIFY(!commands().contains("serial"));
            QVERIFY(state->text().isEmpty());
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        }
        QFile::remove(source.fileName() + ".commands");
        {
            MainWindow window(core.fileName()); window.show();
            auto* mode = window.findChild<QComboBox*>("serialMode");
            auto* port = window.findChild<QSpinBox*>("serialPort");
            auto* state = window.findChild<QLabel*>("serialState");
            // A machine from before per-machine settings takes the old global choice.
            QSettings().setValue("serialMode", "tcp"); QSettings().setValue("serialPort", 5099);
            window.openSession(source.fileName(), true);
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("stop")->isEnabled(), 5000);
            QFile args(source.fileName() + ".args"); QVERIFY(args.open(QIODevice::ReadOnly));
            QVERIFY(args.readAll().contains("--serial\ntcp:5099"));
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Serial: 127.0.0.1:5099, waiting for a connection"), 5000);
            // A later connect and hangup show up without a request.
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Serial: 127.0.0.1:5099, connected"), 5000);
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Serial: 127.0.0.1:5099, waiting for a connection"), 5000);
            // A change while running switches the live endpoint.
            QTRY_VERIFY_WITH_TIMEOUT(mode->isEnabled(), 5000);
            mode->setCurrentIndex(mode->findData("pty"));
            emit mode->activated(mode->currentIndex());
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Serial: /dev/pts/9, connected"), 5000);
            QVERIFY(commands().contains("serial-mode pty\n"));
            QVERIFY(!port->isEnabled());
            // A port that cannot be opened is shown and leaves serial off.
            QTRY_VERIFY_WITH_TIMEOUT(mode->isEnabled(), 5000);
            QString shown;
            QTimer dialogPoll;
            dialogPoll.setInterval(50);
            connect(&dialogPoll, &QTimer::timeout, &window, [&] {
                if (auto* box = window.findChild<QMessageBox*>(); box && box->isVisible()) {
                    shown = box->text();
                    box->done(QMessageBox::Ok);
                }
            });
            dialogPoll.start();
            mode->setCurrentIndex(mode->findData("tcp"));
            port->setValue(80);
            emit mode->activated(mode->currentIndex());
            QTRY_VERIFY_WITH_TIMEOUT(shown.contains("127.0.0.1:80"), 5000);
            dialogPoll.stop();
            QVERIFY(state->text().startsWith("Serial: off, Could not listen on 127.0.0.1:80"));
            // The failed choice is not saved and the mode shows what is open.
            QCOMPARE(mode->currentData().toString(), QString("off"));
            QCOMPARE(machineSetting("serial"), QString("pty"));
            QCOMPARE(QSettings().value("serialMode").toString(), QString("tcp"));
            QTRY_VERIFY_WITH_TIMEOUT(mode->isEnabled(), 5000);
            mode->setCurrentIndex(mode->findData("pty"));
            emit mode->activated(mode->currentIndex());
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Serial: /dev/pts/9, connected"), 5000);
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
            QVERIFY(state->text().isEmpty());
        }
        QCOMPARE(machineSetting("serial", frondSessions::preferencesPath(QSettings().value("lastSession").toString())), QString("pty"));
        QSettings().clear();
    }
    void usbSetting() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        QFile source(files.filePath("input.frond"));
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write("session"); source.close();
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,sys,threading,time
pathlib.Path(sys.argv[1]+'.args').write_text('\n'.join(sys.argv[1:]))
trace=pathlib.Path(sys.argv[1]+'.commands')
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
lock=threading.Lock()
def send(data):
    with lock: s.sendall(data)
s.sendall(b'0\tevent\tready\t\n')
state=b'tcp 127.0.0.1:5098 waiting connecting'
def bus():
    # The TX enumerates and a client connects; the core pushes each change.
    for event in (b'tcp 127.0.0.1:5098 waiting running', b'tcp 127.0.0.1:5098 connected running'):
        time.sleep(.6)
        send(b'0\tevent\tusb\t'+event.hex().encode()+b'\n')
pushed=False
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    arg=bytes.fromhex(arg.decode())
    with trace.open('ab') as t: t.write(cmd+b' '+arg+b'\n')
    if cmd==b'usb-mode':
        if arg==b'tcp:80':
            send(i+b'\terror\tusb\t'+b'Could not listen on 127.0.0.1:80'.hex().encode()+b'\n'); continue
        state={b'pty':b'pty /dev/pts/8 waiting detached',b'off':b'off'}.get(arg,b'tcp 127.0.0.1:'+arg[4:]+b' waiting detached')
    message={b'keyboard-state':b'detached',b'network-state':b'disabled',b'sd-state':b'none',
             b'usb-state':state,b'usb-mode':state}.get(cmd,b'done')
    if cmd==b'save': pathlib.Path(arg.decode()).write_text('saved')
    send(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'usb-state' and not pushed:
        pushed=True; threading.Thread(target=bus,daemon=True).start()
    if cmd==b'quit': break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto commands = [&] {
            QFile trace(source.fileName() + ".commands");
            return trace.open(QIODevice::ReadOnly) ? QString::fromUtf8(trace.readAll()) : QString();
        };
        {
            MainWindow window(core.fileName()); window.show();
            auto* mode = window.findChild<QComboBox*>("usbMode"); QVERIFY(mode);
            // The USB port list sizes to its longest choice in the narrow panel.
            auto* portChoice = window.findChild<QComboBox*>("usbPortChoice"); QVERIFY(portChoice);
            QCOMPARE(portChoice->sizeAdjustPolicy(), QComboBox::AdjustToContents);
            auto* port = window.findChild<QSpinBox*>("usbPort"); QVERIFY(port);
            auto* state = window.findChild<QLabel*>("usbState"); QVERIFY(state);
            QCOMPARE(mode->currentData().toString(), QString("off"));
            QVERIFY(!port->isEnabled());
            // Off by default: no flag and no usb-state request.
            window.openSession(source.fileName(), true);
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("stop")->isEnabled(), 5000);
            QTRY_VERIFY_WITH_TIMEOUT(commands().contains("keyboard-state"), 5000);
            QFile args(source.fileName() + ".args"); QVERIFY(args.open(QIODevice::ReadOnly));
            QVERIFY(!args.readAll().contains("--usb"));
            QVERIFY(!commands().contains("usb"));
            QVERIFY(state->text().isEmpty());
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        }
        QFile::remove(source.fileName() + ".commands");
        {
            MainWindow window(core.fileName()); window.show();
            auto* mode = window.findChild<QComboBox*>("usbMode");
            auto* port = window.findChild<QSpinBox*>("usbPort");
            auto* state = window.findChild<QLabel*>("usbState");
            QSettings().setValue("usbMode", "tcp"); QSettings().setValue("usbPort", 5098);
            window.openSession(source.fileName(), true);
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("stop")->isEnabled(), 5000);
            QFile args(source.fileName() + ".args"); QVERIFY(args.open(QIODevice::ReadOnly));
            const auto passed = args.readAll();
            QVERIFY(passed.contains("--usb\ntcp:5098"));
            QVERIFY(!passed.contains("--serial"));
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("USB: 127.0.0.1:5098, waiting for a connection, Palm TX connecting"), 5000);
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("USB: 127.0.0.1:5098, waiting for a connection, Palm TX enumerated"), 5000);
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("USB: 127.0.0.1:5098, connected, Palm TX enumerated"), 5000);
            // A change while running switches the live endpoint.
            QTRY_VERIFY_WITH_TIMEOUT(mode->isEnabled(), 5000);
            mode->setCurrentIndex(mode->findData("pty"));
            emit mode->activated(mode->currentIndex());
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("USB: /dev/pts/8, waiting for a connection, Palm TX detached"), 5000);
            QVERIFY(commands().contains("usb-mode pty\n"));
            QVERIFY(!port->isEnabled());
            QCOMPARE(machineSetting("usb"), QString("pty"));
            // A port that cannot be opened is shown, leaves USB off and is not saved.
            QTRY_VERIFY_WITH_TIMEOUT(mode->isEnabled(), 5000);
            QTimer dialogPoll;
            dialogPoll.setInterval(50);
            connect(&dialogPoll, &QTimer::timeout, &window, [&] {
                if (auto* box = window.findChild<QMessageBox*>(); box && box->isVisible()) box->done(QMessageBox::Ok);
            });
            dialogPoll.start();
            mode->setCurrentIndex(mode->findData("tcp"));
            port->setValue(80);
            emit mode->activated(mode->currentIndex());
            QTRY_VERIFY_WITH_TIMEOUT(state->text().startsWith("USB: off, Could not listen on 127.0.0.1:80"), 5000);
            dialogPoll.stop();
            QCOMPARE(mode->currentData().toString(), QString("off"));
            QCOMPARE(machineSetting("usb"), QString("pty"));
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
            QVERIFY(state->text().isEmpty());
        }
        QCOMPARE(machineSetting("usb", frondSessions::preferencesPath(QSettings().value("lastSession").toString())), QString("pty"));
        QSettings().clear();
    }
    void e2NewSession() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QSettings().setValue("serialMode", "tcp"); QSettings().setValue("usbMode", "pty");
        QSettings().setValue("fastAudio", true);
        QTemporaryDir files;
        const auto norPath = files.filePath("e2-nor"), nandPath = files.filePath("e2-nand"), txNor = files.filePath("tx-nor");
        {
            // Sparse synthetic images: the E2 card header at 0x20000.
            QFile nor(norPath); QVERIFY(nor.open(QIODevice::WriteOnly)); QVERIFY(nor.resize(8 << 20));
            const char header[] = {char(0xef), char(0xbe), char(0xed), char(0xfe)};
            nor.seek(0x20008); nor.write(header, 4);
            const char palm[] = {'m', 'l', 'a', 'P'}, hal[] = {'r', 'p', 's', 'h'};
            nor.seek(0x20080); nor.write(palm, 4); nor.write(hal, 4);
            QFile tx(txNor); QVERIFY(tx.open(QIODevice::WriteOnly)); QVERIFY(tx.resize(16 << 20));
            QFile nand(nandPath); QVERIFY(nand.open(QIODevice::WriteOnly)); QVERIFY(nand.resize(34603008));
        }
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,sys
pathlib.Path(sys.argv[0]+'.args').write_text('\n'.join(sys.argv[1:]))
trace=pathlib.Path(sys.argv[0]+'.commands')
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    with trace.open('ab') as t: t.write(cmd+b'\n')
    message={b'device-state':b'palm-e2',b'keyboard-state':b'detached',b'network-state':b'disabled',b'sd-state':b'none',b'usb-state':b'pty /dev/pts/9 connected running'}.get(cmd,b'done')
    if cmd==b'keyboard-attach': message=b'attached' if bytes.fromhex(arg.decode())==b'on' else b'detached'
    if cmd==b'save': pathlib.Path(bytes.fromhex(arg.decode()).decode()).write_text('saved')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit': break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto read = [&](const QString& suffix) {
            QFile file(core.fileName() + suffix);
            return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
        };
        const auto firmwareRoot = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/frond/firmware";
        {
            // A TX choice with an E2 NOR is refused and nothing is stored.
            FirmwareDialog dialog(firmwareRoot, frondFirmware::Device::tx);
            dialog.findChild<QLineEdit*>("nor")->setText(norPath);
            QVERIFY(dialog.importImages().contains("Tungsten E2"));
            QVERIFY(!QFileInfo::exists(firmwareRoot + "/palm-tx"));
            QVERIFY(!QFileInfo::exists(firmwareRoot + "/palm-e2"));
        }
        MainWindow window(core.fileName()); window.show();
        auto* stop = window.findChild<QPushButton*>("stop");
        QVERIFY(window.findChild<QLabel*>("emptyNote")->isVisible());
        // Add device opens firmware import directly; NOR detection chooses and starts the E2.
        QString firmwareDevice;
        int firmwareDialogs = 0;
        QTimer dialogPoll;
        dialogPoll.setInterval(50);
        connect(&dialogPoll, &QTimer::timeout, &window, [&] {
            if (auto* firmware = window.findChild<FirmwareDialog*>(); firmware && firmware->isVisible()) {
                QCOMPARE(QApplication::activeModalWidget(), firmware);
                ++firmwareDialogs;
                firmwareDevice = firmware->findChild<QComboBox*>("device")->currentData().toString();
                firmware->findChild<QLineEdit*>("nor")->setText(norPath);
                QVERIFY(firmware->machineSettings);
                firmware->machineSettings->windowScale->setValue(7);
                firmware->machineSettings->smoothLcd->setChecked(true);
                firmware->machineSettings->audio->setChecked(false);
                firmware->findChild<QPushButton*>("import")->click();
            }
        });
        dialogPoll.start();
        window.findChild<QPushButton*>("addDevice")->click();
        QTRY_VERIFY_WITH_TIMEOUT(stop->isEnabled(), 5000);
        dialogPoll.stop();
        QCOMPARE(firmwareDialogs, 1);
        QCOMPARE(firmwareDevice, QString());
        QVERIFY(QFileInfo::exists(firmwareRoot + "/palm-e2/nor.bin"));
        QVERIFY(!QFileInfo::exists(firmwareRoot + "/palm-e2/nand.bin"));
        const auto args = read(".args").split('\n');
        QCOMPARE(args.value(0), QString("--device"));
        QCOMPARE(args.value(1), QString("palm-e2"));
        QCOMPARE(args.value(2), firmwareRoot + "/palm-e2/nor.bin");
        QVERIFY(!args.contains("--nand"));
        // Unqualified E2 firmware gets the queue-only low-latency mode.
        QVERIFY(!args.contains("--fast-audio") && args.contains("--low-latency-queue"));
        QVERIFY(read(".args").contains("--window-scale\n1.75"));
        QVERIFY(args.contains("--smooth-lcd") && args.contains("--no-sound"));
        // The core saves the working session if Frond goes without stopping it.
        const QFileInfo working(managedSession("Tungsten E2"));
        QCOMPARE(args.value(args.indexOf("--save-on-disconnect") + 1),
                 QFileInfo(working.absolutePath()).canonicalFilePath() + '/' + working.fileName());
        frondSessions::Preferences added;
        QVERIFY(frondSessions::readPreferences(managedSession("Tungsten E2"), &added));
        QCOMPARE(added.scale, 7); QVERIFY(added.smooth && !added.audio && added.fastAudio);
        QVERIFY(!QSettings().contains("windowScale"));
        QCOMPARE(window.windowTitle(), QString("Tungsten E2"));
        QTRY_VERIFY_WITH_TIMEOUT(read(".commands").contains("sd-state"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(read(".commands").contains("network-state"), 5000);
        // TX-only network/serial controls stay off; the E2 keyboard and USB are usable.
        QVERIFY(!window.findChild<QComboBox*>("networkMode")->isEnabled());
        QVERIFY(window.findChild<QCheckBox*>("keyboardAttached")->isEnabled());
        window.findChild<QCheckBox*>("keyboardAttached")->click();
        QTRY_VERIFY_WITH_TIMEOUT(read(".commands").contains("keyboard-attach"), 5000);
        QTRY_VERIFY(window.findChild<QCheckBox*>("keyboardAttached")->isChecked());
        QTRY_VERIFY(window.findChild<QComboBox*>("serialMode")->isEnabled());
        QVERIFY(!window.findChild<QComboBox*>("serialMode")->toolTip().contains("Not available"));
        QVERIFY(!window.findChild<QSpinBox*>("serialPort")->isEnabled());
        QTRY_VERIFY(window.findChild<QComboBox*>("usbMode")->isEnabled());
        // A new machine starts with its peripherals off, whatever the old global choices were.
        QVERIFY(!read(".args").contains("--usb") && !read(".args").contains("--serial"));
        QCOMPARE(machineSetting("usb", frondSessions::preferencesPath(managedSession("Tungsten E2"))), QString("off"));
        QCOMPARE(machineSetting("serial", frondSessions::preferencesPath(managedSession("Tungsten E2"))), QString("off"));
        QVERIFY(!window.findChild<QComboBox*>("usbMode")->toolTip().contains("Not available"));
        // A NAND file is passed on when given.
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        // A NAND file is stored and passed on when given.
        {
            FirmwareDialog dialog(firmwareRoot, frondFirmware::Device::e2);
            dialog.findChild<QLineEdit*>("nor")->setText(norPath);
            dialog.findChild<QLineEdit*>("nand")->setText(nandPath);
            QVERIFY(dialog.importImages().isEmpty());
            QCOMPARE(dialog.imported(), frondFirmware::Device::e2);
        }
        MainWindow second(core.fileName()); second.show();
        QCOMPARE(second.findChild<QListWidget*>("sessionList")->count(), 1);
        QCOMPARE(second.findChild<QListWidget*>("sessionList")->currentItem()->data(Qt::UserRole + 1).toBool(), true);
        QVERIFY(!second.findChild<QPushButton*>("start")->isEnabled());
        QString secondDevice;
        QTimer secondPoll;
        secondPoll.setInterval(50);
        connect(&secondPoll, &QTimer::timeout, &second, [&] {
            if (auto* firmware = second.findChild<FirmwareDialog*>(); firmware && firmware->isVisible()) {
                secondDevice = firmware->findChild<QComboBox*>("device")->currentData().toString();
                firmware->findChild<QLineEdit*>("nor")->setText(norPath);
                firmware->findChild<QLineEdit*>("nand")->setText(nandPath);
                firmware->findChild<QPushButton*>("import")->click();
            }
        });
        secondPoll.start();
        second.findChild<QPushButton*>("addDevice")->click();
        QTRY_VERIFY_WITH_TIMEOUT(second.findChild<QPushButton*>("stop")->isEnabled(), 5000);
        secondPoll.stop();
        // The fake saved file has no readable identity, so import starts with Detect.
        QCOMPARE(secondDevice, QString());
        QVERIFY(read(".args").contains("--nand\n" + firmwareRoot + "/palm-e2/nand.bin"));
        QVERIFY(read(".args").contains("--device\npalm-e2\n"));
        QCOMPARE(second.windowTitle(), QString("Tungsten E2 2"));
        second.close(); QTRY_VERIFY_WITH_TIMEOUT(!second.isVisible(), 5000);
        // The serial, USB and fast audio settings made before the E2 session are still saved.
        QCOMPARE(QSettings().value("serialMode").toString(), QString("tcp"));
        QCOMPARE(QSettings().value("usbMode").toString(), QString("pty"));
        QVERIFY(QSettings().value("fastAudio").toBool());
        QVERIFY(QSettings().value("fastAudio").toBool());
        QSettings().clear();
    }
    void zire72NewSession() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QSettings().setValue("serialMode", "tcp"); QSettings().setValue("usbMode", "pty");
        QSettings().setValue("fastAudio", true); QSettings().setValue("irdaMode", "pty");
        QTemporaryDir files;
        const auto norPath = files.filePath("zire-nor"), e2Nor = files.filePath("e2-nor");
        {
            // Sparse synthetic Zire 72 NOR: PalmCard aZ72 small card at 0x0, big card at 0x20000 to the end.
            QFile nor(norPath); QVERIFY(nor.open(QIODevice::WriteOnly)); QVERIFY(nor.resize(14942208));
            auto card = [&](qint64 at, quint32 length) {
                const char signature[] = {char(0xef), char(0xbe), char(0xed), char(0xfe)};
                nor.seek(at + 8); nor.write(signature, 4);
                nor.seek(at + 0x10); nor.write("PalmCard", 9);
                const char bytes[] = {char(length), char(length >> 8), char(length >> 16), char(length >> 24)};
                nor.seek(at + 0x6c); nor.write(bytes, 4);
                const char palm[] = {'m', 'l', 'a', 'P'}, hal[] = {'2', '7', 'Z', 'a'};
                nor.seek(at + 0x80); nor.write(palm, 4); nor.write(hal, 4);
            };
            card(0, 0x10000); card(0x20000, 14942208 - 0x20000);
            QFile e2(e2Nor); QVERIFY(e2.open(QIODevice::WriteOnly)); QVERIFY(e2.resize(8 << 20));
            const char signature[] = {char(0xef), char(0xbe), char(0xed), char(0xfe)};
            e2.seek(0x20008); e2.write(signature, 4);
            const char palm[] = {'m', 'l', 'a', 'P'}, hal[] = {'r', 'p', 's', 'h'};
            e2.seek(0x20080); e2.write(palm, 4); e2.write(hal, 4);
        }
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,sys
pathlib.Path(sys.argv[0]+'.args').write_text('\n'.join(sys.argv[1:]))
trace=pathlib.Path(sys.argv[0]+'.commands')
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    with trace.open('ab') as t: t.write(cmd+b'\n')
    message={b'device-state':b'palm-zire-72',b'keyboard-state':b'detached',b'network-state':b'disabled',b'sd-state':b'none',b'usb-state':b'pty /dev/pts/9 connected running'}.get(cmd,b'done')
    if cmd==b'save': pathlib.Path(bytes.fromhex(arg.decode()).decode()).write_text('saved')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit': break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto read = [&](const QString& suffix) {
            QFile file(core.fileName() + suffix);
            return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
        };
        const auto firmwareRoot = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/frond/firmware";
        {
            // The E2 and TX refuse a Zire NOR, and the Zire an E2 NOR; nothing is stored.
            for (auto device : {frondFirmware::Device::tx, frondFirmware::Device::e2}) {
                FirmwareDialog dialog(firmwareRoot, device);
                dialog.findChild<QLineEdit*>("nor")->setText(norPath);
                QVERIFY(dialog.importImages().contains("Choose Zire 72"));
            }
            FirmwareDialog dialog(firmwareRoot, frondFirmware::Device::zire72);
            QVERIFY(!dialog.findChild<QLineEdit*>("nand")->isEnabled());
            dialog.findChild<QLineEdit*>("nor")->setText(e2Nor);
            QVERIFY(dialog.importImages().contains("Zire 72"));
            QVERIFY(!QFileInfo::exists(firmwareRoot + "/palm-zire-72"));
            // Detecting picks the Zire from its cards.
            dialog.findChild<QComboBox*>("device")->setCurrentIndex(0);
            QVERIFY(dialog.findChild<QLineEdit*>("nand")->isEnabled());
            QCOMPARE(frondFirmware::detectDevice(norPath), frondFirmware::Device::zire72);
        }
        MainWindow window(core.fileName()); window.show();
        auto* stop = window.findChild<QPushButton*>("stop");
        // The import dialog is the first modal window, with the device choice kept there.
        QString firmwareDevice;
        int firmwareDialogs = 0;
        QTimer dialogPoll;
        dialogPoll.setInterval(50);
        connect(&dialogPoll, &QTimer::timeout, &window, [&] {
            if (auto* firmware = window.findChild<FirmwareDialog*>(); firmware && firmware->isVisible()) {
                QCOMPARE(QApplication::activeModalWidget(), firmware);
                ++firmwareDialogs;
                auto* devices = firmware->findChild<QComboBox*>("device");
                QCOMPARE(devices->count(), int(frondFirmware::devices.size()) + 1);
                devices->setCurrentIndex(devices->findData("palm-zire-72"));
                firmwareDevice = firmware->findChild<QComboBox*>("device")->currentData().toString();
                firmware->findChild<QLineEdit*>("nor")->setText(norPath);
                QVERIFY(firmware->machineSettings);
                QVERIFY(firmware->machineSettings->fastAudio->isEnabled());
                QVERIFY(firmware->machineSettings->fastAudio->toolTip().isEmpty());
                firmware->findChild<QPushButton*>("import")->click();
            }
        });
        dialogPoll.start();
        window.findChild<QPushButton*>("addDevice")->click();
        QTRY_VERIFY_WITH_TIMEOUT(stop->isEnabled(), 5000);
        dialogPoll.stop();
        QCOMPARE(firmwareDialogs, 1);
        QCOMPARE(firmwareDevice, QString("palm-zire-72"));
        QVERIFY(QFileInfo::exists(firmwareRoot + "/palm-zire-72/nor.bin"));
        QVERIFY(!QFileInfo::exists(firmwareRoot + "/palm-zire-72/nand.bin"));
        const auto args = read(".args").split('\n');
        QCOMPARE(args.mid(0, 3), QStringList({"--device", "palm-zire-72", firmwareRoot + "/palm-zire-72/nor.bin"}));
        // No NAND, preview audio or serial port; the queue-only low-latency mode is on; a new machine's USB and infrared are off.
        QVERIFY(!args.contains("--nand") && !args.contains("--fast-audio") && !args.contains("--serial"));
        QVERIFY(args.contains("--low-latency-queue"));
        QVERIFY(!read(".args").contains("--usb") && !read(".args").contains("--irda"));
        QCOMPARE(window.windowTitle(), QString("Zire 72"));
        QTRY_VERIFY_WITH_TIMEOUT(read(".commands").contains("keyboard-state"), 5000);
        QVERIFY(!read(".commands").contains("serial-state") && !read(".commands").contains("usb-state"));
        // Network greys out with the shared wording; the Zire has no serial port, so that row is left out.
        {
            auto* box = window.findChild<QComboBox*>("networkMode");
            QVERIFY(!box->isEnabled());
            QCOMPARE(box->toolTip(), QString("Not available on the Zire 72 yet"));
        }
        QVERIFY(!window.findChild<QComboBox*>("serialMode")->isVisibleTo(&window));
        QVERIFY(!window.findChild<QSpinBox*>("serialPort")->isVisibleTo(&window));
        QVERIFY(!window.findChild<QPushButton*>("hotsync")->isVisibleTo(&window));

        QTRY_VERIFY(window.findChild<QComboBox*>("usbMode")->isEnabled());
        QVERIFY(window.findChild<QComboBox*>("irdaMode")->isEnabled());
        QVERIFY(window.findChild<QComboBox*>("bluetoothMode")->isEnabled());
        QVERIFY(window.findChild<QCheckBox*>("keyboardAttached")->isEnabled());  // infrared beaming is off on a new machine
        // Hard reset names the Zire and sends hard-reset.
        QString text;
        QTimer poll; poll.setInterval(30);
        connect(&poll, &QTimer::timeout, &window, [&] {
            auto* box = window.findChild<QMessageBox*>();
            if (!box || !box->isVisible()) return;
            text = box->text();
            for (auto* b : box->buttons()) if (b->text() == "Erase") { b->click(); return; }
        });
        poll.start();
        auto* hard = window.findChild<QPushButton*>("hardReset");
        QTRY_VERIFY(hard->isEnabled());
        hard->click();
        QCOMPARE(text, QString("Erase everything on the Zire 72?"));
        QTRY_VERIFY_WITH_TIMEOUT(read(".commands").contains("hard-reset"), 3000);
        poll.stop();
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        // The saved session is a Zire 72 row, and the settings made before it are kept.
        MainWindow second(core.fileName()); second.show();
        auto* list = second.findChild<QListWidget*>("sessionList");
        QCOMPARE(list->count(), 1);
        QCOMPARE(QSettings().value("serialMode").toString(), QString("tcp"));
        QVERIFY(QSettings().value("fastAudio").toBool());
        QSettings().clear();
    }
    void infraredSetting_data() {
        QTest::addColumn<QByteArray>("deviceId");
        QTest::newRow("tx") << QByteArray("palm-tx");
        QTest::newRow("e2") << QByteArray("palm-e2");
        QTest::newRow("zire72") << QByteArray("palm-zire-72");
    }
    void infraredSetting() {
        QFETCH(QByteArray, deviceId);
        QSettings().clear(); QSettings().setValue("resume", false);
        QSettings().setValue("irdaMode", "tcp"); QSettings().setValue("irdaPort", 5097);
        QTemporaryDir files;
        QFile source(files.filePath("input.frond")); QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("session"); source.close();
        QFile identity(source.fileName()+".device"); QVERIFY(identity.open(QIODevice::WriteOnly)); identity.write(deviceId); identity.close();
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,sys
base=pathlib.Path(sys.argv[1]);pathlib.Path(str(base)+'.args').write_text('\n'.join(sys.argv[1:]))
device=pathlib.Path(str(base)+'.device').read_bytes()
s=socket.socket(socket.AF_UNIX);s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
state=b'tcp:5097 127.0.0.1:5097 connected'
s.sendall(b'0\tevent\tready\t\n0\tevent\tinfrared\t'+state.hex().encode()+b'\n')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t');arg=bytes.fromhex(arg.decode())
    message={b'device-state':device,b'sd-state':b'none',b'network-state':b'disabled',b'keyboard-state':b'detached'}.get(cmd,b'done')
    if cmd==b'irda-mode':
        state=b'off' if arg==b'off' else arg+b' 127.0.0.1:'+arg.split(b':')[-1]+b' waiting'
        s.sendall(b'0\tevent\tinfrared\t'+state.hex().encode()+b'\n');message=state
    if cmd==b'keyboard-attach':message=b'attached' if arg==b'on' else b'detached'
    if cmd==b'save':pathlib.Path(arg.decode()).write_text('saved')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit':break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        {
            MainWindow window(core.fileName()); window.show(); window.openSession(source.fileName(),true);
            auto* mode=window.findChild<QComboBox*>("irdaMode"); auto* port=window.findChild<QSpinBox*>("irdaPort");
            auto* keyboard=window.findChild<QCheckBox*>("keyboardAttached"); auto* state=window.findChild<QLabel*>("irdaState");
            QVERIFY(mode && port && keyboard && state);
            QTRY_VERIFY(mode->isEnabled() && state->text().contains("connected"));
            QVERIFY(!keyboard->isEnabled()); QCOMPARE(port->value(),5097);
            QFile args(source.fileName()+".args"); QVERIFY(args.open(QIODevice::ReadOnly)); QVERIFY(args.readAll().contains("--irda\ntcp:5097"));
            mode->setCurrentIndex(mode->findData("off")); QMetaObject::invokeMethod(mode,"activated",Q_ARG(int,mode->currentIndex()));
            QTRY_VERIFY(keyboard->isEnabled()); QTRY_COMPARE(state->text(),QString());
            keyboard->click(); QTRY_VERIFY(keyboard->isChecked() && !mode->isEnabled());
            QTRY_VERIFY(keyboard->isEnabled()); keyboard->click(); QTRY_VERIFY(mode->isEnabled());
            mode->setCurrentIndex(mode->findData("tcp")); QMetaObject::invokeMethod(mode,"activated",Q_ARG(int,mode->currentIndex()));
            QTRY_VERIFY(state->text().contains("waiting")); QTRY_VERIFY(!keyboard->isEnabled());
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(),5000);
        }
        QCOMPARE(machineSetting("infrared", frondSessions::preferencesPath(QSettings().value("lastSession").toString())), QString("tcp:5097"));
        QSettings().clear();
    }
    // Leaving IR printer mode with a job arriving asks first. A Discard while a printer poll is still out
    // waits for its reply and then switches, so the core follows the choice.
    void infraredPrinterDiscard() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        QFile source(files.filePath("input.frond")); QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("session"); source.close();
        QFile identity(source.fileName()+".device"); QVERIFY(identity.open(QIODevice::WriteOnly)); identity.write("alphasmart-dana"); identity.close();
        const auto polled = files.filePath("polled"), modes = files.filePath("modes");
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(QString(R"PY(#!/usr/bin/python3
import pathlib,socket,sys,time
base=pathlib.Path(sys.argv[1]);device=pathlib.Path(str(base)+'.device').read_bytes()
s=socket.socket(socket.AF_UNIX);s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t');arg=bytes.fromhex(arg.decode())
    message={b'device-state':device,b'sd-state':b'none',b'network-state':b'disabled',b'keyboard-state':b'detached'}.get(cmd,b'done')
    if cmd==b'irprinter' and arg==b'status':
        with open('%1','a') as f: f.write('poll\n')
        time.sleep(0.8);message=b'attached receiving bytes=752 receiving=752 last=none idle=0'
    if cmd==b'irda-mode':
        with open('%2','a') as f: f.write(arg.decode()+'\n')
        message=b'off' if arg==b'off' else arg+b' ready'
    if cmd==b'save':pathlib.Path(arg.decode()).write_text('saved')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit':break
)PY").arg(polled, modes).toUtf8());
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        {
            MainWindow window(core.fileName()); window.show(); window.openSession(source.fileName(),true);
            auto* mode=window.findChild<QComboBox*>("irdaMode"); QVERIFY(mode);
            auto polls=[&]{ QFile f(polled); return f.open(QIODevice::ReadOnly) ? f.readAll().count('\n') : 0; };
            auto sent=[&]{ QFile f(modes); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); };
            QTRY_VERIFY_WITH_TIMEOUT(mode->isEnabled(), 10000);
            mode->setCurrentIndex(mode->findData("printer")); QMetaObject::invokeMethod(mode,"activated",Q_ARG(int,mode->currentIndex()));
            // Wait for a poll reply that shows output arriving.
            QTRY_VERIFY_WITH_TIMEOUT(sent() == "printer\n" && polls() >= 2 && mode->isEnabled(), 10000);
            QCOMPARE(mode->currentData().toString(), QString("printer"));
            int seen = -1; bool clicked = false; QTimer confirm; confirm.setInterval(10);
            connect(&confirm, &QTimer::timeout, &window, [&] {
                auto* box = window.findChild<QMessageBox*>();
                if (!box || !box->isVisible() || clicked) return;
                // Discard only once another poll has gone out and is still waiting for its reply.
                if (seen < 0) seen = polls();
                else if (polls() > seen) { clicked = true; box->button(QMessageBox::Discard)->click(); }
            });
            confirm.start();
            mode->setCurrentIndex(mode->findData("off")); QMetaObject::invokeMethod(mode,"activated",Q_ARG(int,mode->currentIndex()));
            QVERIFY(clicked);
            QTRY_VERIFY_WITH_TIMEOUT(sent() == "printer\noff\n", 5000);
            confirm.stop();
            QCOMPARE(mode->currentData().toString(), QString("off"));
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(),5000);
        }
        QCOMPARE(machineSetting("infrared", frondSessions::preferencesPath(QSettings().value("lastSession").toString())), QString("off"));
        QSettings().clear();
    }
    void bluetoothSetting_data() {
        QTest::addColumn<QByteArray>("deviceId");
        QTest::newRow("tx") << QByteArray("palm-tx");
        QTest::newRow("e2") << QByteArray("palm-e2");
        QTest::newRow("zire72") << QByteArray("palm-zire-72");
        QTest::newRow("tg50") << QByteArray("sony-clie-tg50");
    }
    void bluetoothSetting() {
        QFETCH(QByteArray, deviceId);
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        const auto socketPath = files.filePath("private controller socket");
        QFile source(files.filePath("input.frond")); QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("session"); source.close();
        QFile identity(source.fileName()+".device"); QVERIFY(identity.open(QIODevice::WriteOnly)); identity.write(deviceId); identity.close();
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,sys
base=pathlib.Path(sys.argv[1]);pathlib.Path(str(base)+'.args').write_text('\n'.join(sys.argv[1:]))
device=pathlib.Path(str(base)+'.device').read_bytes()
s=socket.socket(socket.AF_UNIX);s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
state=(sys.argv[sys.argv.index('--bluetooth')+1].encode()+b' connected') if '--bluetooth' in sys.argv else b'off'
s.sendall(b'0\tevent\tready\t\n0\tevent\tbluetooth\t'+state.hex().encode()+b'\n')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t');arg=bytes.fromhex(arg.decode())
    message={b'device-state':device,b'sd-state':b'none',b'network-state':b'disabled',b'keyboard-state':b'detached'}.get(cmd,b'done')
    if cmd==b'bluetooth-mode':
        if arg.endswith(b'missing'):
            s.sendall(i+b'\terror\tbluetooth\t'+b'Controller socket unavailable'.hex().encode()+b'\n');continue
        state=b'off' if arg==b'off' else arg+b' connected';message=state
        s.sendall(b'0\tevent\tbluetooth\t'+state.hex().encode()+b'\n')
    if cmd==b'save':pathlib.Path(arg.decode()).write_text('saved')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit':break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        {
            MainWindow window(core.fileName()); window.show();
            auto* mode=window.findChild<QComboBox*>("bluetoothMode");
            auto* path=window.findChild<QLineEdit*>("bluetoothSocket");
            auto* state=window.findChild<QLabel*>("bluetoothState");
            const auto pick=[mode](const char* value){
                mode->setCurrentIndex(mode->findData(value));
                QMetaObject::invokeMethod(mode,"activated",Q_ARG(int,mode->currentIndex()));
            };
            QVERIFY(mode && path && state); QCOMPARE(mode->currentData().toString(), QString("off")); QVERIFY(!path->isEnabled());
            QVERIFY(mode->findData("radio") > 0 && mode->findData("socket") > 0 && mode->findData("link:local") < 0);
            QSettings().setValue("bluetoothEnabled", true); QSettings().setValue("bluetoothSocket", socketPath);
            window.openSession(source.fileName(),true);
            QTRY_VERIFY(mode->isEnabled() && state->text().contains("connected"));
            QFile args(source.fileName()+".args"); QVERIFY(args.open(QIODevice::ReadOnly));
            QVERIFY(args.readAll().contains("--bluetooth\n"+socketPath.toUtf8()));
            QCOMPARE(mode->currentData().toString(), QString("socket"));
            pick("off"); QTRY_VERIFY(mode->isEnabled() && state->text().isEmpty());
            QVERIFY(!path->isEnabled()); pick("socket");
            QTRY_VERIFY(mode->isEnabled() && state->text().contains("connected"));
            QString shown; QTimer dismiss; dismiss.setInterval(20);
            connect(&dismiss,&QTimer::timeout,&window,[&]{
                if(auto* box=window.findChild<QMessageBox*>();box && box->isVisible()) {shown=box->text();box->done(QMessageBox::Ok);}
            }); dismiss.start();
            // A failed switch keeps the core's socket, and the field goes back to it.
            path->setText(socketPath+" missing"); emit path->editingFinished();
            QTRY_VERIFY(shown.contains("Controller socket unavailable"));
            QCOMPARE(machineSetting("bluetooth"), "socket:" + socketPath);
            QTRY_COMPARE(path->text(), socketPath);
            QCOMPARE(mode->currentData().toString(), QString("socket"));
            QTRY_VERIFY(mode->isEnabled()); pick("off");
            QTRY_VERIFY(mode->isEnabled() && state->text().isEmpty());
            QCOMPARE(machineSetting("bluetooth"), QString("off"));
            path->setText(socketPath); pick("socket");
            QTRY_VERIFY(mode->isEnabled() && state->text().contains("connected"));
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(),5000);
        }
        QCOMPARE(machineSetting("bluetooth", frondSessions::preferencesPath(QSettings().value("lastSession").toString())), "socket:" + socketPath);
        QSettings().clear();
    }
    void deviceWindowCloseReturnsHome_data() {
        QTest::addColumn<bool>("failFirstSave");
        QTest::newRow("saved") << false;
        QTest::newRow("cancel-failed-save") << true;
    }
    void deviceWindowCloseReturnsHome() {
        QFETCH(bool, failFirstSave);
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        QFile source(files.filePath("input.frond"));
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write("unchanged import"); source.close();
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        auto script = QByteArray(R"PY(#!/usr/bin/python3
import pathlib,socket,sys
trace=pathlib.Path(sys.argv[0]+'.commands')
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
saves=0
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    with trace.open('ab') as f: f.write(cmd+b'\n')
    message={b'device-state':b'palm-tx',b'keyboard-state':b'detached',b'network-state':b'disabled',b'sd-state':b'none'}.get(cmd,b'done')
    if cmd==b'save':
        saves+=1
        if FAIL_FIRST and saves==1:
            s.sendall(i+b'\terror\twrite\t'+b'Disk full'.hex().encode()+b'\n'); continue
        pathlib.Path(bytes.fromhex(arg.decode()).decode()).write_bytes(b'saved guest changes')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'keyboard-state': s.sendall(b'0\tevent\tclose\t\n')
    if cmd==b'quit': break
)PY");
        script.replace("FAIL_FIRST", failFirstSave ? "True" : "False");
        core.write(script); core.close();
        QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto commands = [&] {
            QFile trace(core.fileName() + ".commands");
            return trace.open(QIODevice::ReadOnly) ? trace.readAll().split('\n') : QList<QByteArray>();
        };
        MainWindow window(core.fileName()); window.show();
        auto* stop = window.findChild<QPushButton*>("stop");
        auto* start = window.findChild<QPushButton*>("start");
        auto* sessions = window.findChild<QListWidget*>("sessionList");
        QVERIFY(stop && start && sessions);
        bool prompted = false;
        QTimer dialogs; dialogs.setInterval(20);
        connect(&dialogs, &QTimer::timeout, &window, [&] {
            if (auto* box = window.findChild<QMessageBox*>(); box && box->isVisible()) {
                prompted = box->text().contains("Disk full");
                box->button(QMessageBox::Cancel)->click();
            }
        });
        dialogs.start(); window.openSession(source.fileName(), true);
        if (failFirstSave) {
            QTRY_VERIFY_WITH_TIMEOUT(prompted && stop->isEnabled(), 5000);
            QVERIFY(window.isVisible()); QVERIFY(!sessions->isVisible());
            QCOMPARE(commands().count("save"), 1); QCOMPARE(commands().count("quit"), 0);
            stop->click();
        }
        QTRY_VERIFY_WITH_TIMEOUT(!stop->isEnabled() && sessions->isVisible(), 5000);
        dialogs.stop();
        QVERIFY(window.isVisible()); QVERIFY(!stop->isEnabled());
        QCOMPARE(prompted, failFirstSave);
        QCOMPARE(commands().count("save"), failFirstSave ? 2 : 1);
        QCOMPARE(commands().count("quit"), 1);
        const auto savedPath = QSettings().value("lastSession").toString();
        QVERIFY(savedPath != source.fileName());
        QFile saved(savedPath); QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), QByteArray("saved guest changes"));
        QCOMPARE(digest(source.fileName()), QCryptographicHash::hash("unchanged import", QCryptographicHash::Sha256));
        QVERIFY(window.close()); QVERIFY(!window.isVisible());
        QSettings().clear();
    }
    void failedSaveStaysOpen() {
        QTemporaryDir files;
        QFile source(files.path() + "/import.frond");
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write("unchanged import"); source.close();
        QFile core(files.path() + "/core");
        QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import socket,sys
s=socket.socket(socket.AF_UNIX)
s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    if cmd == b'device-state':
        s.sendall(i + b'\tok\tdevice\t' + b'palm-tx'.hex().encode() + b'\n')
        continue
    if cmd == b'keyboard-state':
        s.sendall(i + b'\tok\tkeyboard\t6465746163686564\n')
        continue
    if cmd == b'network-state':
        s.sendall(i + b'\tok\tnetwork\t64697361626c6564\n')
        continue
    if cmd == b'sd-state':
        s.sendall(i+b'\tok\tcard\t'+b'none'.hex().encode()+b'\n');continue
    assert cmd == b'save', 'Closing after a failed save must not send quit'
    s.sendall(i+b'\terror\twrite\t'+b'Disk full'.hex().encode()+b'\n')
)PY");
        core.close();
        QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        MainWindow window(core.fileName()); window.show(); window.openSession(source.fileName(), true);
        auto* stop = window.findChild<QPushButton*>("stop");
        QTRY_VERIFY_WITH_TIMEOUT(stop->isEnabled(), 5000);
        bool prompted = false;
        QTimer dialogPoll;
        dialogPoll.setInterval(50);
        connect(&dialogPoll, &QTimer::timeout, &window, [&] {
            if (auto* dialog = window.findChild<QMessageBox*>()) {
                prompted = dialog->text().contains("Disk full");
                dialogPoll.stop();
                dialog->done(QMessageBox::Cancel);
            }
        });
        dialogPoll.start();
        QVERIFY(!window.close());
        QTRY_VERIFY_WITH_TIMEOUT(prompted, 3000);
        QTRY_VERIFY(stop->isEnabled());
        QVERIFY(window.isVisible());
        QCOMPARE(digest(source.fileName()), QCryptographicHash::hash("unchanged import", QCryptographicHash::Sha256));
    }
    // Files… on Internal: one flat list of databases by file name; install, export and delete go
    // to the callbacks with the database names, and each answer reads the list again.
    void internalFilesWindow() {
        int lists = 0;
        QStringList installed, exported, removed;
        QString exportFolder;
        CardFilesWindow::Databases d;
        d.list = [&] { lists++; };
        d.install = [&](const QStringList& files) { installed = files; };
        d.exportTo = [&](const QStringList& names, const QString& folder) { exported = names; exportFolder = folder; };
        d.remove = [&](const QStringList& names) { removed = names; };
        CardFilesWindow w(d, "Internal");
        QVERIFY(w.internal());
        QCOMPARE(lists, 1);
        QCOMPARE(w.windowTitle(), QString("Internal files"));
        QVERIFY(w.working());
        for (QWidget* hidden : std::initializer_list<QWidget*>{w.upButton, w.pathBar, w.cutButton, w.copyButton, w.pasteButton, w.renameButton,
                                                               w.newFolderButton, w.addFolderButton})
            QVERIFY(hidden->isHidden());
        w.setDatabases({{"Memo/DB", "DATA", "memo", false}, {"Notes", "appl", "note", true}});
        QVERIFY(!w.working());
        QCOMPARE(w.header->text(), QString("2 databases"));
        QCOMPARE(w.list->topLevelItemCount(), 2);
        w.selectNames({"Memo_DB.pdb", "Notes.prc"});
        QCOMPARE(w.selectedNames().size(), 2);
        w.exportSelected("/somewhere");
        QCOMPARE(exported, QStringList({"Memo/DB", "Notes"}));
        QCOMPARE(exportFolder, QString("/somewhere"));
        QVERIFY(w.working());
        // A second request waits for the answer.
        w.deleteSelected(false);
        QVERIFY(removed.isEmpty());
        w.databasesDone("2 files copied");
        QCOMPARE(lists, 2);
        QCOMPARE(w.note->text(), QString("2 files copied"));
        w.setDatabases({{"Memo/DB", "DATA", "memo", false}, {"Notes", "appl", "note", true}});
        w.selectNames({"Notes.prc"});
        w.deleteSelected(false);
        QCOMPARE(removed, QStringList({"Notes"}));
        w.databasesDone("Notes deleted");
        w.setDatabases({{"Memo/DB", "DATA", "memo", false}});
        QCOMPARE(w.header->text(), QString("1 database"));
        // A folder adds the files in it.
        QTemporaryDir host;
        QDir(host.path()).mkpath("in");
        for (const char* name : {"in/A.prc", "in/B.pdb"}) {
            QFile f(host.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        w.addPaths({host.filePath("in")});
        QCOMPARE(installed, QStringList({host.filePath("in/A.prc"), host.filePath("in/B.pdb")}));
    }
    void sdFiles_data() {
        QTest::addColumn<QString>("mode");
        for (const auto* mode : {"apply", "cancel-start", "already-ejected", "save-fail", "take-out-fail", "put-in-fail", "eject-fail"})
            QTest::newRow(mode) << QString(mode);
    }
    // Files… on the running machine's card: eject, save, take the card out, edit it in place in
    // Files, then put it back in (and eject it again if it was out). Swap stays set from the save on.
    void sdFiles() {
        QFETCH(QString, mode);
        cardsReset();
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        const auto core = pickerCore(files.path(), "palm-tx");
        auto flag = [&](const QString& name) { QFile f(files.filePath(name)); QVERIFY(f.open(QIODevice::WriteOnly)); };
        if (mode == "already-ejected") flag("start-ejected");
        if (mode == "save-fail") flag("fail-save");
        if (mode == "take-out-fail") flag("fail-sd-take-out");
        if (mode == "put-in-fail") flag("fail-sd-put-in");
        if (mode == "eject-fail") flag("fail-sd-eject");
        const auto a = cardsMachine("Palm TX", true);
        frondCards::reconcile(cardsSessions());
        const auto cardPath = a + "/sd.frondsd";
        const auto generation = cardGeneration(cardPath);
        QFile host(files.filePath("hello.txt")); QVERIFY(host.open(QIODevice::WriteOnly)); host.write("hello"); host.close();
        MainWindow window(core);
        window.show();
        window.openSession(a + "/Palm TX.frond", true);
        auto* action = window.findChild<QPushButton*>("sdExchange");
        QTRY_VERIFY_WITH_TIMEOUT(action->isEnabled(), 10000);
        QCOMPARE(action->text(), QString("Files…"));
        bool sawWindow = false;
        QStringList errors;
        QTimer poll; poll.setInterval(10);
        connect(&poll, &QTimer::timeout, &window, [&] {
            for (auto* box : window.findChildren<QMessageBox*>()) {
                if (!box->isVisible()) continue;
                if (box->windowTitle().contains("Open the card")) box->button(mode == "cancel-start" ? QMessageBox::Cancel : QMessageBox::Yes)->click();
                else { errors << box->text(); box->done(QMessageBox::Ok); }
            }
            if (auto* w = window.findChild<CardFilesWindow*>("cardFilesWindow"); w && w->isVisible() && !sawWindow) {
                sawWindow = true;
                QVERIFY(!action->isEnabled());
                for (const auto* name : {"sdInsert", "sdEject", "cardPick"}) QVERIFY(!window.findChild<QPushButton*>(name)->isEnabled());
                QVERIFY(frondCards::readSlot(a).swap);
                w->addPaths({host.fileName()});
                QVERIFY(w->list->findItems("hello.txt", Qt::MatchExactly).size() == 1);
                w->doneButton->click();
            }
        });
        poll.start(); action->click();
        QTRY_VERIFY_WITH_TIMEOUT(mode == "cancel-start" || sawWindow || !errors.isEmpty(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(action->isEnabled(), 10000);
        QTest::qWait(100);
        poll.stop();
        const bool opened = mode == "apply" || mode == "already-ejected" || mode == "put-in-fail";
        QCOMPARE(sawWindow, opened);
        QCOMPARE(!errors.isEmpty(), mode.endsWith("fail"));
        if (mode == "put-in-fail") QVERIFY2(errors.join(' ').contains("It's kept in the machine and goes in at the next start."), qPrintable(errors.join(' ')));
        QFile trace(files.filePath("commands")); QVERIFY(trace.open(QIODevice::ReadOnly));
        QStringList got;
        for (const auto& line : QString::fromUtf8(trace.readAll()).split('\n', Qt::SkipEmptyParts)) {
            const auto command = line.section('\t', 0, 0);
            if (command == "save" || command.startsWith("sd-") && command != "sd-state") got << command;
        }
        QStringList expected;
        if (mode == "apply" || mode == "put-in-fail") expected = {"sd-eject", "save", "sd-take-out", "sd-put-in"};
        else if (mode == "already-ejected") expected = {"save", "sd-take-out", "sd-put-in", "sd-eject"};
        else if (mode == "save-fail") expected = {"sd-eject", "save", "sd-reinsert"};
        else if (mode == "take-out-fail") expected = {"sd-eject", "save", "sd-take-out", "sd-reinsert"};
        else if (mode == "eject-fail") expected = {"sd-eject"};
        QCOMPARE(got, expected);
        // The card file changed in place only when Files had it, one generation on, and it loads.
        QVERIFY(frondCardFile::verify(cardPath).has_value());
        QCOMPARE(*frondCardFile::verify(cardPath), opened ? generation + 1 : generation);
                // Named while the card is out of the Palm.
                QVERIFY2(!w->windowTitle().startsWith("No card"), qPrintable(w->windowTitle()));
        {
            auto c = frondCardFile::CardFile::open(cardPath, false);
            frondCardFile::FatImage fat(*c);
            QCOMPARE(bool(fat.find("/hello.txt")), opened);
        }
        if (opened || mode == "take-out-fail") QVERIFY(frondCards::readSlot(a).swap);
        for (const auto& f : QDir(files.path()).entryList({"fail-*"})) QFile::remove(files.filePath(f));
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 10000);
        QSettings().clear();
    }
    // Files… on a running TG50's blank classic stick: eject, save, take it out, Files formats it the
    // TG50's way and adds a file, then it goes back in.
    void tg50StickFiles() {
        const auto device = frondFirmware::deviceFromId("sony-clie-tg50");
        if (!frondFirmware::profile(device).frontend.memoryStick) QSKIP("Memory Stick is not qualified yet");
        cardsReset();
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        const auto core = pickerCore(files.path(), "sony-clie-tg50");
        const auto dir = cardsSessions() + "/Stick";
        TestMedia media;
        media.kind = 2;
        media.logical = 4 << 20;
        media.physical = 4 * 2048 * 521;
        media.identity = machineIdentity("Stick");
        writeFrond(dir + "/Stick.frond", "sony-clie-tg50", {media});
        cardTool({"make", "ms", "4", dir + "/ms.frondms"});
        frondCards::reconcile(cardsSessions());
        const auto cardPath = dir + "/ms.frondms";
        const auto generation = cardGeneration(cardPath);
        QFile host(files.filePath("hello.txt")); QVERIFY(host.open(QIODevice::WriteOnly)); host.write("hello"); host.close();
        MainWindow window(core);
        window.show();
        window.openSession(dir + "/Stick.frond", true);
        auto* action = window.findChild<QPushButton*>("sdExchange");
        QTRY_VERIFY_WITH_TIMEOUT(action->isVisible() && action->isEnabled(), 10000);
        bool sawWindow = false;
        QStringList errors;
        QTimer poll; poll.setInterval(10);
        connect(&poll, &QTimer::timeout, &window, [&] {
            for (auto* box : window.findChildren<QMessageBox*>()) {
                if (!box->isVisible()) continue;
                if (box->windowTitle().contains("Open the card")) box->button(QMessageBox::Yes)->click();
                else { errors << box->text(); box->done(QMessageBox::Ok); }
            }
            if (auto* w = window.findChild<CardFilesWindow*>("cardFilesWindow"); w && w->isVisible() && !sawWindow) {
                sawWindow = true;
                QVERIFY(frondCards::readSlot(dir).swap);
                w->addPaths({host.fileName()});
                QVERIFY(w->list->findItems("hello.txt", Qt::MatchExactly).size() == 1);
                w->doneButton->click();
            }
        });
        poll.start(); action->click();
        QTRY_VERIFY_WITH_TIMEOUT(sawWindow || !errors.isEmpty(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(action->isEnabled(), 10000);
        QTest::qWait(100);
        poll.stop();
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(' ')));
        QVERIFY(sawWindow);
        QFile trace(files.filePath("commands")); QVERIFY(trace.open(QIODevice::ReadOnly));
        QStringList got;
        for (const auto& line : QString::fromUtf8(trace.readAll()).split('\n', Qt::SkipEmptyParts)) {
            const auto command = line.section('\t', 0, 0);
            if (command == "save" || (command.startsWith("ms-") && command != "ms-state" && command != "ms-protection")) got << command;
            QVERIFY(!command.startsWith("sd-") && command != "ms-files" && command != "ms-put");
        }
        QCOMPARE(got, QStringList({"ms-eject", "save", "ms-take-out", "ms-put-in"}));
        QCOMPARE(cardGeneration(cardPath), generation + 1);
        {
            auto c = frondCardFile::CardFile::open(cardPath, false);
            frondCardFile::FatImage fat(*c);
            QVERIFY(fat.find("/hello.txt") && fat.find("/MEMSTICK.IND"));
        }
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 10000);
        QSettings().clear();
    }
    // A Palm IIIx ROM imports as a IIIx and runs in frond-68k beside the ARM core, with only the
    // arguments and controls a IIIx has.
    void iiixNewSession() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QSettings().setValue("serialMode", "pty"); QSettings().setValue("irdaMode", "tcp"); QSettings().setValue("irdaPort", 5031);
        QSettings().setValue("usbMode", "pty"); QSettings().setValue("bluetoothEnabled", true);
        QSettings().setValue("bluetoothSocket", "/nonexistent/bt"); QSettings().setValue("fastAudio", true);
        qputenv("FROND_ENGINE_SETTING", "1");
        QDir(managedFolder()).removeRecursively();
        QTemporaryDir files;
        const auto romPath = files.filePath("Palm-OS-3.5-en-ez.rom");
        {
            // Sparse synthetic OS 3.5 EZ ROM: the big-endian card header only.
            QFile rom(romPath); QVERIFY(rom.open(QIODevice::WriteOnly)); QVERIFY(rom.resize(0x178000));
            auto be = [&](qint64 at, quint32 value, int bytes) {
                char out[4];
                for (int i = 0; i < bytes; ++i) out[i] = char(value >> (8 * (bytes - 1 - i)));
                rom.seek(at); rom.write(out, bytes);
            };
            be(0, 0x3000, 4); be(4, 0x10c090e0, 4); be(8, 0xfeedbeef, 4); be(0x0c, 4, 2); be(0x0e, 0x0020, 2);
            rom.seek(0x10); rom.write("PalmCard", 9); be(0x6c, 0x178000, 4);
        }
        QFile arm(files.filePath("core")); QVERIFY(arm.open(QIODevice::WriteOnly));
        arm.write("#!/bin/sh\ntouch \"$0.started\"\n"); arm.close();
        QVERIFY(arm.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        // frond-68k answers the state queries for absent hardware as absent and the rest as unsupported.
        QFile core(files.filePath("core-68k")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,struct,sys
pathlib.Path(sys.argv[0]+'.args').write_text('\n'.join(sys.argv[1:]))
trace=pathlib.Path(sys.argv[0]+'.commands')
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    a=bytes.fromhex(arg.decode())
    with trace.open('ab') as t: t.write(cmd+b' '+a+b'\n')
    status,code=b'ok',b'result'
    message={b'device-state':b'palm-iiix',b'keyboard-state':b'detached',b'network-state':b'disabled',
             b'serial-state':b'pty /dev/pts/7 waiting',b'usb-state':b'off',b'sd-state':b'none'}.get(cmd,b'done')
    if cmd==b'display': message=b'contrast '+a.split(b'=')[1]+b', smooth off'
    if cmd==b'serial-state' and 'modem' in sys.argv: message=b'modem online 33600; hotsync pty /dev/pts/9 connected'
    if cmd==b'save':
        pathlib.Path(a.decode()).write_bytes(struct.pack('<II',0x19800819,8)+b'palm-iiix'.ljust(32,b'\0')+bytes(8)+
            struct.pack('<5Q',0x178000,0,4<<20,0,0)+bytes(4)+b'state')
    if cmd in (b'rotate',b'usb-mode',b'hold',b'hold-state',b'microphone-mode',b'keyboard-key') or cmd.split(b'-')[0] in (b'sd',b'ms',b'bluetooth'):
        status,code,message=b'error',b'unsupported',b'unsupported'
    s.sendall(i+b'\t'+status+b'\t'+code+b'\t'+message.hex().encode()+b'\n')
    if cmd==b'quit': break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto read = [&](const QString& suffix) {
            QFile file(core.fileName() + suffix);
            return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
        };
        const auto firmwareRoot = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/frond/firmware";
        QDir(firmwareRoot + "/palm-iiix").removeRecursively();
        QStringList errors;
        {
            MainWindow window(arm.fileName()); window.show();
            QTimer poll; poll.setInterval(30);
            int dialogs = 0;
            connect(&poll, &QTimer::timeout, &window, [&] {
                if (auto* box = window.findChild<QMessageBox*>(); box && box->isVisible()) { errors << box->text(); box->accept(); }
                auto* firmware = window.findChild<FirmwareDialog*>();
                if (!firmware || !firmware->isVisible()) return;
                ++dialogs;
                firmware->findChild<QLineEdit*>("nor")->setText(romPath);
                auto* settings = firmware->machineSettings;
                // Detected as a IIIx: contrast and speed, no Engine.
                QVERIFY(settings->contrast->isVisibleTo(firmware) && settings->speed->isVisibleTo(firmware));
                QVERIFY(!settings->engine->isVisibleTo(firmware) && !settings->fastAudio->isVisibleTo(firmware));
                QCOMPARE(settings->contrast->value(), 50);
                settings->windowScale->setValue(6);
                settings->contrast->setValue(30);
                settings->speed->setCurrentIndex(settings->speed->findData(true));
                firmware->findChild<QPushButton*>("import")->click();
            });
            poll.start();
            window.findChild<QPushButton*>("addDevice")->click();
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("stop")->isEnabled(), 5000);
            QCOMPARE(dialogs, 1);
            QVERIFY(QFileInfo::exists(firmwareRoot + "/palm-iiix/nor.bin"));
            QCOMPARE(digest(firmwareRoot + "/palm-iiix/nor.bin"), digest(romPath));
            QCOMPARE(window.windowTitle(), QString("Palm IIIx"));
            const auto args = read(".args").split('\n');
            QCOMPARE(args.mid(0, 3), QStringList({"--device", "palm-iiix", firmwareRoot + "/palm-iiix/nor.bin"}));
            for (const char* option : {"--nand", "--work-folder", "--usb", "--bluetooth", "--engine", "--fast-audio", "--low-latency-queue",
                                       "--jog-controls", "--hide-keyboard", "--no-sound"})
                QVERIFY2(!args.contains(option), option);
            const auto text = read(".args");
            QVERIFY(text.contains("--window-scale\n3\n") && text.contains("--contrast\n30\n") && text.contains("--speed\nuncapped\n"));
            QVERIFY(!text.contains("--serial") && !text.contains("--irda"));
            QVERIFY(!QFileInfo::exists(arm.fileName() + ".started"));
            auto* hotSync = window.findChild<QPushButton*>("hotsync");
            QTRY_VERIFY_WITH_TIMEOUT(hotSync->isEnabled(), 5000);
            // Only what a IIIx has is in the panels and the Machine menu.
            const auto shown = [&](const char* name) { return window.findChild<QWidget*>(name)->isVisibleTo(&window); };
            auto* panelHost = window.findChild<PanelHost*>("panelHost");
            const auto inPanel = [&](int page, std::initializer_list<const char*> yes, std::initializer_list<const char*> no) {
                panelHost->open(page);
                for (const char* name : yes) QVERIFY2(shown(name), name);
                for (const char* name : no) QVERIFY2(!shown(name), name);
            };
            inPanel(0, {"installApp", "otherApp", "exportRam"}, {});
            inPanel(1, {}, {"storageHeading", "sdState", "cardPick", "sdRetry"});
            inPanel(2, {"runningContrast", "sound"}, {"rotate", "microphone"});
            inPanel(3, {"hotsync", "serialMode", "irdaMode"}, {"usbMode", "usbPort", "bluetoothMode", "bluetoothSocket"});
            if (QTest::currentTestFailed()) return;
            for (const char* name : {"machineReset", "machineSave", "machineSaveCopy", "machineHardReset"})
                QVERIFY2(window.findChild<QAction*>(name)->isVisible(), name);
            for (const char* name : {"machineNetwork", "machineKeyboard", "machineHold"})
                QVERIFY2(!window.findChild<QAction*>(name)->isVisible(), name);
            QTRY_VERIFY(window.findChild<QComboBox*>("serialMode")->isEnabled());
            QCOMPARE(window.findChild<QSlider*>("runningContrast")->value(), 30);
            hotSync->click();
            QTRY_VERIFY_WITH_TIMEOUT(read(".commands").contains("hotsync \n"), 5000);
            QTRY_VERIFY(hotSync->isEnabled());
            window.findChild<QSlider*>("runningContrast")->setValue(70);
            QTRY_VERIFY_WITH_TIMEOUT(read(".commands").contains("display contrast=70\n"), 5000);
            frondSessions::Preferences prefs;
            QTRY_VERIFY(frondSessions::readPreferences(managedSession("Palm IIIx"), &prefs) && prefs.contrast == 70);
            QVERIFY(prefs.uncapped && prefs.scale == 6);
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
            poll.stop();
        }
        // Nothing frond-68k answers unsupported was asked.
        const auto commands = read(".commands");
        for (const char* command : {"rotate", "usb-", "hold", "microphone-mode", "sd-", "ms-", "bluetooth-"})
            QVERIFY2(!commands.contains(QString("\n") + command) && !commands.startsWith(command), command);
        QCOMPARE(errors, QStringList());
        QVERIFY(commands.contains("save ") && commands.contains("quit "));
        const auto saved = frondSessions::describe(managedSession("Palm IIIx"));
        QVERIFY(saved.saved && !saved.old); QCOMPARE(saved.device, QString("palm-iiix"));
        {
            // It starts again from its session with the saved settings, and duplicates.
            MainWindow window(arm.fileName()); window.show();
            auto* list = window.findChild<QListWidget*>("sessionList");
            QCOMPARE(list->count(), 1);
            // The IIIx has a modem: a saved serial choice of modem is passed on as --serial modem.
            {
                frondSessions::Preferences saved;
                QVERIFY(frondSessions::readPreferences(managedSession("Palm IIIx"), &saved));
                saved.serial = "modem";
                saved.infrared = "phone";
                QCOMPARE(frondSessions::writePreferences(managedSession("Palm IIIx"), saved), QString());
            }
            emit list->itemActivated(list->item(0));
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("stop")->isEnabled(), 5000);
            const auto args = read(".args").split('\n');
            QVERIFY(read(".args").contains("--serial\nmodem"));
            QVERIFY(read(".args").contains("--modem-sync\npty"));
            QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLabel*>("serialState")->text(),
                                      QString("Modem: online at 33600, HotSync on /dev/pts/9"), 5000);
            QVERIFY(read(".args").contains("--irda\nphone"));
            QCOMPARE(window.findChild<QComboBox*>("irdaMode")->currentData().toString(), QString("phone"));
            QCOMPARE(args.value(0), QFileInfo(managedSession("Palm IIIx")).canonicalFilePath());
            QVERIFY(!args.contains("--device") && !args.contains("--work-folder"));
            QVERIFY(read(".args").contains("--contrast\n70\n") && read(".args").contains("--speed\nuncapped\n"));
            QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QSlider*>("runningContrast")->value(), 70, 5000);
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
            QTRY_VERIFY(window.findChild<QPushButton*>("addDevice")->isEnabled());
            rowAction(window, 0, "duplicateSession");
            QTRY_COMPARE_WITH_TIMEOUT(list->count(), 2, 5000);
            frondSessions::Preferences prefs;
            QVERIFY(frondSessions::readPreferences(managedSession("Palm IIIx 2"), &prefs));
            QCOMPARE(prefs.contrast, 70); QVERIFY(prefs.uncapped);
            QCOMPARE(frondSessions::describe(managedSession("Palm IIIx 2")).device, QString("palm-iiix"));
            QCOMPARE(digest(managedSession("Palm IIIx 2")), digest(managedSession("Palm IIIx")));
        }
        QVERIFY(!QFileInfo::exists(arm.fileName() + ".started"));
        qunsetenv("FROND_ENGINE_SETTING");
        QDir(managedFolder()).removeRecursively();
        QDir(firmwareRoot + "/palm-iiix").removeRecursively();
        QSettings().clear();
    }
    // Opt-in run with a real ROM and frond-68k, not in ctest: FROND_IIIX_ROM and FROND_IIIX_CORE, and
    // FROND_IIIX_SHOTS for screenshots (the Palm window is taken from DISPLAY with ImageMagick's import).
    void iiixRealRom() {
        const auto rom = qEnvironmentVariable("FROND_IIIX_ROM"), core68k = qEnvironmentVariable("FROND_IIIX_CORE");
        if (rom.isEmpty() || core68k.isEmpty()) QSKIP("Set FROND_IIIX_ROM and FROND_IIIX_CORE");
        const auto shots = qEnvironmentVariable("FROND_IIIX_SHOTS");
        const auto palmShot = [&](const QString& name) {
            if (!shots.isEmpty()) QCOMPARE(QProcess::execute("import", {"-window", "root", shots + '/' + name}), 0);
        };
        const auto desktopShot = [&](QWidget& window, const QString& name) {
            if (!shots.isEmpty()) QVERIFY(window.grab().save(shots + '/' + name));
        };
        const auto logText = [](QWidget& window) { return window.findChild<QPlainTextEdit*>()->toPlainText(); };
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        QStringList errors;
        const auto session = managedSession("Palm IIIx");
        {
            MainWindow window(QString(), core68k); window.show();
            QTimer poll; poll.setInterval(50);
            connect(&poll, &QTimer::timeout, &window, [&] {
                if (auto* box = window.findChild<QMessageBox*>(); box && box->isVisible()) { errors << box->text(); box->accept(); }
                auto* firmware = window.findChild<FirmwareDialog*>();
                if (!firmware || !firmware->isVisible()) return;
                firmware->findChild<QLineEdit*>("nor")->setText(rom);
                firmware->machineSettings->windowScale->setValue(4);
                firmware->findChild<QPushButton*>("import")->click();
            });
            poll.start();
            window.findChild<QPushButton*>("addDevice")->click();
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("hotsync")->isVisibleTo(&window), 30000);
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("hotsync")->isEnabled(), 30000);
            QVERIFY(logText(window).contains("frond-68k: palm-iiix"));
            QTest::qWait(8000);
            desktopShot(window, "desktop-running.png");
            palmShot("palm-contrast-50.png");
            for (int value : {90, 15}) {
                window.findChild<QSlider*>("runningContrast")->setValue(value);
                frondSessions::Preferences prefs;
                QTRY_VERIFY_WITH_TIMEOUT(frondSessions::readPreferences(session, &prefs) && prefs.contrast == value, 10000);
                QTest::qWait(1000);
                palmShot(QString("palm-contrast-%1.png").arg(value));
            }
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 30000);
            poll.stop();
            QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
            if (!shots.isEmpty()) {
                QFile log(shots + "/first-run-log.txt"); QVERIFY(log.open(QIODevice::WriteOnly)); log.write(logText(window).toUtf8());
            }
        }
        const auto saved = frondSessions::describe(session);
        QVERIFY(saved.saved && !saved.old); QCOMPARE(saved.device, QString("palm-iiix"));
        QCOMPARE(frondSessions::readHeader(session).device, QString("palm-iiix"));
        {
            // Resume on startup runs the saved session again with contrast 15.
            QSettings().setValue("resume", true);
            MainWindow window(QString(), core68k); window.show();
            QTimer poll; poll.setInterval(50);
            connect(&poll, &QTimer::timeout, &window, [&] {
                if (auto* box = window.findChild<QMessageBox*>(); box && box->isVisible()) { errors << box->text(); box->accept(); }
            });
            poll.start();
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("hotsync")->isEnabled(), 30000);
            QVERIFY(logText(window).contains("from session " + QFileInfo(session).canonicalFilePath()));
            QCOMPARE(window.findChild<QSlider*>("runningContrast")->value(), 15);
            QTest::qWait(5000);
            desktopShot(window, "desktop-resumed.png");
            palmShot("palm-resumed.png");
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 30000);
            poll.stop();
            QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
            if (!shots.isEmpty()) {
                QFile log(shots + "/resumed-log.txt"); QVERIFY(log.open(QIODevice::WriteOnly)); log.write(logText(window).toUtf8());
            }
        }
        QDir(managedFolder()).removeRecursively();
        QSettings().clear();
    }
    // Edit settings: a IIIx has Contrast and Original/Uncapped speed, never the Engine; ARM devices have
    // the speed too, no contrast, and the Engine.
    void iiixSettings() {
        const auto iiix = frondFirmware::deviceFromId("palm-iiix");
        frondSessions::Preferences prefs;
        QCOMPARE(prefs.contrast, 50);
        prefs.contrast = 20; prefs.uncapped = true; prefs.fastEngine = true;
        {
            MachineSettingsDialog dialog(iiix, prefs); dialog.show();
            auto* c = dialog.controls;
            for (QWidget* w : {static_cast<QWidget*>(c->contrast), static_cast<QWidget*>(c->speed),
                               static_cast<QWidget*>(c->windowScale), static_cast<QWidget*>(c->smoothLcd), static_cast<QWidget*>(c->audio)})
                QVERIFY(w->isVisibleTo(&dialog));
            for (QWidget* w : {static_cast<QWidget*>(c->engine), static_cast<QWidget*>(c->fastAudio), static_cast<QWidget*>(c->showKeyboard),
                               static_cast<QWidget*>(c->jogControls)})
                QVERIFY(!w->isVisibleTo(&dialog));
            QVERIFY(c->audio->isEnabled());
            QCOMPARE(c->contrast->value(), 20); QVERIFY(c->speed->currentData().toBool());
            c->contrast->setValue(65); c->speed->setCurrentIndex(c->speed->findData(false));
            const auto values = c->values();
            QCOMPARE(values.contrast, 65); QVERIFY(!values.uncapped);
        }
        {
            MachineSettingsDialog dialog(frondFirmware::Device::tx, prefs); dialog.show();
            QVERIFY(dialog.controls->engine->isVisibleTo(&dialog));
            QVERIFY(!dialog.controls->contrast->isVisibleTo(&dialog) && dialog.controls->speed->isVisibleTo(&dialog));
        }
        {
            MachineSettingsDialog iiixDialog(iiix, prefs); iiixDialog.show();
            QVERIFY(!iiixDialog.controls->engine->isVisibleTo(&iiixDialog) && iiixDialog.controls->speed->isVisibleTo(&iiixDialog));
        }
        // The firmware field is a ROM for the IIIx.
        QTemporaryDir root;
        FirmwareDialog firmware(root.path(), iiix);
        bool rom = false;
        for (auto* label : firmware.findChildren<QLabel*>()) rom = rom || label->text() == "ROM image";
        QVERIFY(rom);
    }
    // Sessions in the managed folder, written the way the core does (magic, version, device name).
    static QByteArray sessionBytes(quint32 version, const QByteArray& device) {
        QByteArray bytes;
        for (quint32 value : {0x19800819u, version})
            for (int i = 0; i < 4; i++) bytes.append(char(value >> (8 * i)));
        return bytes + device.leftJustified(32, '\0', true) + "body";
    }
    // The newest machine settings file, and one key from it.
    static QString newestSettings() {
        QString newest; QDateTime when;
        QDirIterator it(managedFolder(), {"*.settings.json"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const auto path = it.next();
            if (newest.isEmpty() || QFileInfo(path).lastModified() >= when) { newest = path; when = QFileInfo(path).lastModified(); }
        }
        return newest;
    }
    static QString machineSetting(const char* key, const QString& file = newestSettings()) {
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(f.readAll()).object().value(key).toString();
    }
    static QString managedFolder() {
        return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/frond/sessions";
    }
    static QString managedSession(const QString& name) {
        return frondSessions::filePath(managedFolder(), name);
    }
    static void putSession(const QString& name, quint32 version, const QByteArray& device, int daysAgo) {
        const auto path = managedSession(name);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write(sessionBytes(version, device)); file.close();
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QDateTime::currentDateTime().addDays(-daysAgo), QFileDevice::FileModificationTime));
    }
    static QStringList rowTexts(MainWindow& window, const QString& label) {
        QStringList texts;
        auto* list = window.findChild<QListWidget*>("sessionList");
        for (int i = 0; i < list->count(); i++) texts << list->itemWidget(list->item(i))->findChild<QLabel*>(label)->text();
        return texts;
    }
    static void rowAction(MainWindow& window, int row, const QString& action) {
        auto* list = window.findChild<QListWidget*>("sessionList");
        auto* menu = list->itemWidget(list->item(row))->findChild<QPushButton*>("rowMenu")->menu();
        for (auto* a : menu->actions()) if (a->objectName() == action) { QVERIFY(a->isEnabled()); a->trigger(); return; }
        QFAIL("no such row action");
    }
    void sessionListRows() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("Trip", 6, "palm-tx", 1);
        putSession("Work", 6, "palm-e2", 0);
        putSession("Ancient", 5, "palm-tx", 2);
        QVERIFY(frondSessions::create(managedFolder(), "Fresh", "palm-tx"));
        QFile marker(frondSessions::markerPath(managedSession("Fresh")));
        QVERIFY(marker.open(QIODevice::ReadWrite)); marker.setFileTime(QDateTime::currentDateTime().addSecs(60), QFileDevice::FileModificationTime);
        MainWindow window{QString()}; window.show();
        auto* list = window.findChild<QListWidget*>("sessionList");
        QCOMPARE(rowTexts(window, "sessionName"), QStringList({"Fresh", "Work", "Trip", "Ancient"}));
        const auto detail = rowTexts(window, "sessionDetail");
        QCOMPARE(detail[0], QString::fromUtf8("Palm TX · Not saved"));
        QVERIFY(detail[1].startsWith(QString::fromUtf8("Tungsten E2 · Saved ")));
        QVERIFY(detail[1].endsWith(QString::fromUtf8(" · ") + QLocale().formattedDataSize(sessionBytes(6, "palm-e2").size())));
        QVERIFY(detail[2].startsWith(QString::fromUtf8("Palm TX · Saved ")));
        QCOMPARE(detail[3], QString("Saved by an older Frond"));  // a version 5 file names no device
        QVERIFY(!window.findChild<QLabel*>("emptyNote")->isVisible());
        // The old one can only be deleted.
        list->setCurrentRow(3);
        QVERIFY(!window.findChild<QPushButton*>("start")->isEnabled());
        auto* menu = list->itemWidget(list->item(3))->findChild<QPushButton*>("rowMenu")->menu();
        for (auto* a : menu->actions()) QCOMPARE(a->isEnabled(), a->objectName() == "deleteSession");
        // The unsaved one can't be exported.
        menu = list->itemWidget(list->item(0))->findChild<QPushButton*>("rowMenu")->menu();
        for (auto* a : menu->actions()) QCOMPARE(a->isEnabled(), a->objectName() != "exportSession");
        // A saved one exports on a worker; the window says where it went.
        {
            QTemporaryDir out;
            whenShown<QFileDialog>([&](QFileDialog* d) {
                d->setDirectory(out.path());
                d->selectFile("Work.frond");
                QMetaObject::invokeMethod(d, "accept");
            });
            menu = list->itemWidget(list->item(1))->findChild<QPushButton*>("rowMenu")->menu();
            for (auto* a : menu->actions()) if (a->objectName() == "exportSession") a->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().startsWith("Session exported to"), 10000);
            QVERIFY(QFileInfo(out.filePath("Work/Work.frond")).isFile());
            QVERIFY(!window.findChild<QProgressDialog*>("cardJob") || !window.findChild<QProgressDialog*>("cardJob")->isVisible());
        }
        list->setCurrentRow(1);
        QVERIFY(window.findChild<QPushButton*>("start")->isEnabled());
        QDir(managedFolder()).removeRecursively();
        MainWindow empty{QString()};
        QVERIFY(empty.findChild<QListWidget*>("sessionList")->count() == 0);
        empty.show();
        QVERIFY(empty.findChild<QLabel*>("emptyNote")->isVisible());
        QVERIFY(!empty.findChild<QPushButton*>("start")->isEnabled());
        QSettings().clear();
    }
    void sessionStartFromList() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("Trip", 6, "palm-tx", 1);
        QTemporaryDir files;
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,sys
pathlib.Path(sys.argv[0]+'.args').write_text('\n'.join(sys.argv[1:]))
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    message={b'keyboard-state':b'detached',b'network-state':b'disabled',b'sd-state':b'none',b'device-state':b'palm-tx',
             b'serial-state':b'tcp 127.0.0.1:5077 waiting'}.get(cmd,b'done')
    if cmd==b'save': pathlib.Path(bytes.fromhex(arg.decode()).decode()).write_text('saved')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit': break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        MainWindow window(core.fileName()); window.show();
        auto* list = window.findChild<QListWidget*>("sessionList");
        QCOMPARE(list->count(), 1);
        emit list->itemActivated(list->item(0));
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("stop")->isEnabled(), 5000);
        QFile args(core.fileName() + ".args"); QVERIFY(args.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(args.readAll()).split('\n').value(0), QFileInfo(managedSession("Trip")).canonicalFilePath());
        QVERIFY(QFileInfo::exists(managedSession("Trip") + ".lock"));
        QVERIFY(!window.findChild<QWidget*>("home")->isVisible());
        QVERIFY(window.findChild<RunningBar*>("runningBar")->isVisible());
        QCOMPARE(window.windowTitle(), QString("Trip"));
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        // The core's save went into the same session, and it is the one that resumes.
        QCOMPARE(QSettings().value("lastSession").toString(), QFileInfo(managedSession("Trip")).canonicalFilePath());
        QDir(managedFolder()).removeRecursively();
        QSettings().clear();
    }
    // The panels have the Android Menu sections, and the menu bar only File and Help.
    void hardReset() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        const auto sent = files.path() + "/sent";
        QFile core(files.path() + "/core");
        QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import socket,sys
s=socket.socket(socket.AF_UNIX)
s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
saves=0
log=open(sys.argv[0].rsplit('/',1)[0]+'/sent','a')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    log.write(cmd.decode()+'\n'); log.flush()
    if cmd==b'save':
        saves+=1
        if saves==1: s.sendall(i+b'\terror\twrite\t'+b'Disk full'.hex().encode()+b'\n'); continue
    message={b'keyboard-state':b'detached',b'network-state':b'disabled',b'device-state':b'palm-tx',b'sd-state':b'none'}.get(cmd,b'done')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit': break
)PY");
        core.close();
        QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        auto commands = [&] {
            QFile f(sent); if (!f.open(QIODevice::ReadOnly)) return QStringList();
            QStringList all = QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts), work;
            for (const auto& c : all) if (c == "save" || c == "hard-reset" || c == "quit") work << c;
            return work;
        };
        // A managed machine whose slot changed since the last save; the save before a hard reset
        // clears its swap.
        QDir(managedFolder()).removeRecursively();
        putSession("Hard", 6, "palm-tx", 0);
        const auto hardDir = frondSessions::machineDirOf(managedSession("Hard"));
        auto swapped = [&] { return frondCards::readSlot(hardDir).swap; };
        QVERIFY(frondCards::writeSlot(hardDir, {true, {}, {}, "hard", true}));
        MainWindow window(core.fileName()); window.show(); window.openSession(managedSession("Hard"), true);
        auto* hard = window.findChild<QPushButton*>("hardReset");
        QTRY_VERIFY_WITH_TIMEOUT(hard->isEnabled(), 5000);
        QVERIFY(window.findChild<QAction*>("machineHardReset")->isVisible());
        QString answer, text, failed;
        QTimer poll; poll.setInterval(30);
        connect(&poll, &QTimer::timeout, &window, [&] {
            auto* box = window.findChild<QMessageBox*>();
            if (!box || !box->isVisible()) return;
            if (box->text().contains("Disk full")) { failed = box->text(); box->done(QMessageBox::Ok); return; }
            text = box->text();
            if (answer == "Cancel") {
                if (const auto shots = qEnvironmentVariable("FROND_TEST_SHOTS"); !shots.isEmpty()) {
                    box->grab().save(shots + "/hard-reset-dialog.png");
                    window.grab().save(shots + "/hard-reset-running.png");
                }
                box->button(QMessageBox::Cancel)->click();
                return;
            }
            for (auto* b : box->buttons()) if (b->text() == answer) { b->click(); return; }
        });
        poll.start();
        answer = "Cancel"; hard->click();
        QCOMPARE(text, QString("Erase everything on the Palm TX?"));
        QTRY_VERIFY(hard->isEnabled());
        QCOMPARE(commands(), QStringList());
        // A failed save leaves the Palm alone.
        answer = "Save first"; hard->click();
        QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty(), 3000);
        QTRY_VERIFY(hard->isEnabled());
        QCOMPARE(commands(), QStringList({"save"}));
        QVERIFY(swapped());
        hard->click();
        QTRY_COMPARE_WITH_TIMEOUT(commands(), QStringList({"save", "save", "hard-reset"}), 3000);
        QTRY_VERIFY(hard->isEnabled());
        QVERIFY(!swapped());
        answer = "Erase"; hard->click();
        QTRY_COMPARE_WITH_TIMEOUT(commands(), QStringList({"save", "save", "hard-reset", "hard-reset"}), 3000);
        poll.stop();
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        QDir(managedFolder()).removeRecursively();
        QSettings().clear();
    }
    // Save first, then the core dies before the save answers: the next session's save doesn't erase.
    void hardResetDroppedWhenCoreDies() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        QFile source(files.path() + "/hard.frond");
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write("session"); source.close();
        const auto sent = files.path() + "/sent";
        QFile core(files.path() + "/core");
        QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import os,socket,sys
s=socket.socket(socket.AF_UNIX)
s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
folder=sys.argv[0].rsplit('/',1)[0]
first=not os.path.exists(folder+'/ran')
open(folder+'/ran','w').close()
log=open(folder+'/sent','a')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    log.write(cmd.decode()+'\n'); log.flush()
    if cmd==b'save' and first: os._exit(3)
    message={b'keyboard-state':b'detached',b'network-state':b'disabled',b'device-state':b'palm-tx',b'sd-state':b'none'}.get(cmd,b'done')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit': break
)PY");
        core.close();
        QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        auto commands = [&] {
            QFile f(sent); if (!f.open(QIODevice::ReadOnly)) return QStringList();
            QStringList all = QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts), work;
            for (const auto& c : all) if (c == "save" || c == "hard-reset" || c == "quit") work << c;
            return work;
        };
        MainWindow window(core.fileName()); window.show(); window.openSession(source.fileName(), true);
        auto* hard = window.findChild<QPushButton*>("hardReset");
        QTRY_VERIFY_WITH_TIMEOUT(hard->isEnabled(), 5000);
        QTimer poll; poll.setInterval(30);
        connect(&poll, &QTimer::timeout, &window, [&] {
            auto* box = window.findChild<QMessageBox*>();
            if (!box || !box->isVisible()) return;
            if (!box->text().startsWith("Erase everything")) { box->done(QMessageBox::Ok); return; }
            for (auto* b : box->buttons()) if (b->text() == "Save first") { b->click(); return; }
        });
        poll.start();
        hard->click();
        QTRY_COMPARE_WITH_TIMEOUT(commands(), QStringList({"save"}), 3000);
        // The core is gone; start the session again and save it.
        QTest::qWait(1000);
        window.openSession(source.fileName(), true);
        QTRY_VERIFY_WITH_TIMEOUT(hard->isEnabled(), 5000);
        window.findChild<QPushButton*>("save")->click();
        QTRY_COMPARE_WITH_TIMEOUT(commands(), QStringList({"save", "save"}), 3000);
        QTest::qWait(300);
        QCOMPARE(commands(), QStringList({"save", "save"}));
        poll.stop();
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        QSettings().clear();
    }
    // The core dies while Files has the running machine's card: Files closes, and the card stays in
    // the machine folder, swapped, to go in at the next start.
    void coreCrashInSdFiles() {
        cardsReset();
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        const auto core = pickerCore(files.path(), "palm-tx");
        { QFile f(files.filePath("die-after-take-out")); QVERIFY(f.open(QIODevice::WriteOnly)); }
        const auto a = cardsMachine("Palm TX", true);
        frondCards::reconcile(cardsSessions());
        MainWindow window(core);
        window.show();
        window.openSession(a + "/Palm TX.frond", true);
        auto* action = window.findChild<QPushButton*>("sdExchange");
        QTRY_VERIFY_WITH_TIMEOUT(action->isEnabled(), 10000);
        bool sawWindow = false;
        QStringList errors;
        QTimer poll; poll.setInterval(10);
        connect(&poll, &QTimer::timeout, &window, [&] {
            for (auto* box : window.findChildren<QMessageBox*>()) {
                if (!box->isVisible()) continue;
                if (box->windowTitle().contains("Open the card")) box->button(QMessageBox::Yes)->click();
                else { errors << box->text(); box->done(QMessageBox::Ok); }
            }
            if (auto* w = window.findChild<CardFilesWindow*>("cardFilesWindow"); w && w->isVisible()) sawWindow = true;
        });
        poll.start(); action->click();
        QTRY_VERIFY_WITH_TIMEOUT(sawWindow, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(errors.join(' ').contains("The card is kept in the machine and goes in at the next start."), 10000);
        QTest::qWait(300);
        poll.stop();
        auto* w = window.findChild<CardFilesWindow*>("cardFilesWindow");
        QVERIFY(!w || !w->isVisible());
        QVERIFY(frondCards::readSlot(a).swap);
        QVERIFY(frondCardFile::ready(a + "/sd.frondsd"));
        QVERIFY(frondCardFile::verify(a + "/sd.frondsd").has_value());
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        QSettings().clear();
    }
    void sdFilesCloseDuringJob_data() {
        QTest::addColumn<QString>("mode");
        for (const auto* mode : {"close", "core-exit"}) QTest::newRow(mode) << QString(mode);
    }
    // Files closed, or the core gone, while a job still writes the card: the window stays until
    // the worker has stopped, and only then is the card closed and put back in (or kept).
    void sdFilesCloseDuringJob() {
        QFETCH(QString, mode);
        cardsReset();
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        const auto core = pickerCore(files.path(), "palm-tx");
        const auto a = cardsMachine("Palm TX", true);
        frondCards::reconcile(cardsSessions());
        const auto cardPath = a + "/sd.frondsd";
        QFile host(files.filePath("big.bin"));
        QVERIFY(host.open(QIODevice::WriteOnly));
        host.write(QByteArray(4 << 20, 'x'));
        host.close();
        MainWindow window(core);
        window.show();
        window.openSession(a + "/Palm TX.frond", true);
        auto* action = window.findChild<QPushButton*>("sdExchange");
        QTRY_VERIFY_WITH_TIMEOUT(action->isEnabled(), 10000);
        std::atomic<bool> entered{false}, release{false};
        QPointer<CardFilesWindow> w;
        bool started = false, heldBack = false;
        QStringList errors;
        QTimer poll; poll.setInterval(10);
        connect(&poll, &QTimer::timeout, &window, [&] {
            for (auto* box : window.findChildren<QMessageBox*>()) {
                if (!box->isVisible()) continue;
                if (box->windowTitle().contains("Open the card")) box->button(QMessageBox::Yes)->click();
                else { errors << box->text(); box->done(QMessageBox::Ok); }
            }
            if (!w)
                if (auto* f = window.findChild<CardFilesWindow*>("cardFilesWindow"); f && f->isVisible()) {
                    w = f;
                    f->workHook = [&] { entered = true; while (!release) QThread::msleep(5); };
                    QTimer::singleShot(0, f, [f, &host] { f->addPaths({host.fileName()}); });
                }
            if (entered && !started) {
                started = true;
                if (mode == "close") w->reject();
                else {
                    QFile pid(files.filePath("pid"));
                    QVERIFY(pid.open(QIODevice::ReadOnly));
                    ::kill(pid.readAll().trimmed().toInt(), SIGKILL);
                }
                QTimer::singleShot(1000, &window, [&] {
                    heldBack = w && w->isVisible() && w->working();
                    release = true;
                });
            }
        });
        poll.start(); action->click();
        QTRY_VERIFY_WITH_TIMEOUT(started, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(release, 10000);
        QVERIFY(heldBack);
        // Closed once the worker stopped, then deleted.
        QTRY_VERIFY_WITH_TIMEOUT(!w, 10000);
        if (mode == "close") QTRY_VERIFY_WITH_TIMEOUT(action->isEnabled(), 10000);
        else QTRY_VERIFY_WITH_TIMEOUT(errors.join(' ').contains("The card is kept in the machine and goes in at the next start."), 10000);
        QTest::qWait(200);
        poll.stop();
        QFile trace(files.filePath("commands")); QVERIFY(trace.open(QIODevice::ReadOnly));
        QStringList got;
        for (const auto& line : QString::fromUtf8(trace.readAll()).split('\n', Qt::SkipEmptyParts)) {
            const auto command = line.section('\t', 0, 0);
            if (command == "save" || command.startsWith("sd-") && command != "sd-state") got << command;
        }
        if (mode == "close") QCOMPARE(got, QStringList({"sd-eject", "save", "sd-take-out", "sd-put-in"}));
        else QCOMPARE(got, QStringList({"sd-eject", "save", "sd-take-out"}));
        // The add was cancelled: the card is closed, settled, loads, and doesn't have the file.
        QVERIFY(!frondCardFile::inUse(cardPath));
        QVERIFY(frondCardFile::ready(cardPath));
        QVERIFY(frondCardFile::verify(cardPath).has_value());
        {
            auto c = frondCardFile::CardFile::open(cardPath, false);
            frondCardFile::FatImage fat(*c);
            QVERIFY(!fat.find("/big.bin"));
            QVERIFY2(fat.check().isEmpty(), qPrintable(fat.check().join("; ")));
        }
        if (mode == "core-exit") QVERIFY(frondCards::readSlot(a).swap);
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 10000);
        QSettings().clear();
    }
    void coreCrashThenStart() {
        // The core dies with a signal while running: one dialog, naming nothing that isn't there,
        // and Start works again afterwards.
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("Crash", 6, "palm-tx", 1);
        QTemporaryDir files;
        QFile core(files.path() + "/core");
        QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import os,signal,socket,sys,time
s=socket.socket(socket.AF_UNIX)
s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
print('core: about to crash' if not os.path.exists(sys.argv[0]+'.ran') else 'core: second run', flush=True)
s.sendall(b'0\tevent\tready\t\n')
first=not os.path.exists(sys.argv[0]+'.ran')
open(sys.argv[0]+'.ran','w').close()
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    message={b'keyboard-state':b'detached',b'network-state':b'disabled',b'device-state':b'palm-tx',b'sd-state':b'none'}.get(cmd,b'done')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'device-state' and first:
        # The control socket goes first, as when a crash handler tears down; then the process dies.
        s.shutdown(socket.SHUT_RDWR); time.sleep(.3); os.kill(os.getpid(),signal.SIGSEGV)
    if cmd==b'quit': break
)PY");
        core.close();
        QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        MainWindow window(core.fileName()); window.show();
        QStringList shown, details;
        // Close the newest dialog a while after it shows, as a person would, so a second one
        // stacks over the first if the window makes two.
        QTimer poll; poll.setInterval(600);
        connect(&poll, &QTimer::timeout, &window, [&] {
            const auto boxes = window.findChildren<QMessageBox*>();
            for (auto it = boxes.rbegin(); it != boxes.rend(); ++it) {
                if (!(*it)->isVisible()) continue;
                shown << (*it)->text(); details << (*it)->detailedText();
                (*it)->done(QMessageBox::Ok);
                return;
            }
        });
        poll.start();
        auto* list = window.findChild<QListWidget*>("sessionList");
        QCOMPARE(list->count(), 1);
        list->setCurrentRow(0);
        auto* start = window.findChild<QPushButton*>("start");
        QVERIFY(start->isEnabled());
        start->click();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(core.fileName() + ".ran"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!shown.isEmpty() && start->isVisible() && start->isEnabled(), 5000);
        QTest::qWait(2000);  // a second dialog would come by now
        QCOMPARE(shown.size(), 1);
        QVERIFY2(shown[0].contains("stopped unexpectedly") && !shown[0].contains("View"), qPrintable(shown[0]));
        QVERIFY2(details[0].contains("core: about to crash"), qPrintable(details[0]));
        start->click();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("stop")->isEnabled(), 5000);
        QCOMPARE(shown.size(), 1);
        poll.stop();
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        QDir(managedFolder()).removeRecursively();
        QSettings().clear();
    }
    void runningView() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("Trip", 6, "palm-e2", 1);
        QTemporaryDir files;
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,sys
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
card=b'inserted'
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    message={b'keyboard-state':b'detached',b'network-state':b'disabled',b'device-state':b'palm-e2'}.get(cmd,b'done')
    if cmd==b'sd-state': message=card
    if cmd==b'sd-eject': card=b'ejected'
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit': break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        MainWindow window(core.fileName()); window.show();
        QStringList menus;
        for (auto* action : window.menuBar()->actions()) menus << action->text();
        QCOMPARE(menus, QStringList({"&File", "&Help"}));
        QVERIFY(!window.findChild<RunningBar*>("runningBar")->isVisible());
        QStringList items;
        for (auto* action : window.menuBar()->actions().first()->menu()->actions()) if (!action->isSeparator()) items << action->text();
        QCOMPARE(items, QStringList({"&Import session…", "C&ards…", "&Pairing…", "&Firmware…", "&Settings…", "&Quit"}));
        auto* list = window.findChild<QListWidget*>("sessionList");
        emit list->itemActivated(list->item(0));
        auto* sd = window.findChild<QLabel*>("sdState");
        QTRY_COMPARE_WITH_TIMEOUT(sd->text(), QString("Inserted"), 5000);
        // The page is gone; the four panels open at the Palm.
        QVERIFY(!window.findChild<QWidget*>("running"));
        QStringList headings;
        auto* panelHost = window.findChild<PanelHost*>("panelHost");
        for (int page = 0; page < 4; ++page) { panelHost->open(page); headings << window.findChild<QLabel*>("panelTitle")->text(); }
        QCOMPARE(headings, QStringList({"Applications", "Storage", "Display", "Connections"}));
        panelHost->open(1);
        const auto shown = [&](const char* name) { return window.findChild<QPushButton*>(name)->isVisibleTo(&window); };
        QVERIFY(shown("internalFiles"));
        QVERIFY(shown("sdExchange") && shown("sdEject") && !shown("sdInsert") && shown("cardPick") && !shown("sdRetry"));
        // The card row says it once: the picker button names the card, and Inserted has no label.
        QVERIFY(!sd->isVisibleTo(&window));
        // Contrast is for 68k devices; the E2 keeps Rotate and has the HotSync button.
        panelHost->open(2);
        QVERIFY(shown("rotate") && !window.findChild<QSlider*>("runningContrast")->isVisibleTo(&window));
        panelHost->open(3);
        QVERIFY(shown("hotsync"));
        QTRY_VERIFY(window.findChild<QPushButton*>("installApp")->isEnabled());
        // The E2 has no network; its serial port, USB, keyboard and Bluetooth stay.
        QVERIFY(!window.findChild<QComboBox*>("networkMode")->isEnabled());
        QVERIFY(window.findChild<QComboBox*>("serialMode")->isEnabled());
        QVERIFY(window.findChild<QCheckBox*>("keyboardAttached")->isEnabled());
        // Sound here and in Settings are the same setting.
        auto* sound = window.findChild<QCheckBox*>("sound");
        QVERIFY(sound->isChecked());
        sound->click();
        frondSessions::Preferences prefs;
        QVERIFY(frondSessions::readPreferences(managedSession("Trip"), &prefs));
        QVERIFY(!prefs.audio);
        QVERIFY(!QSettings().contains("audio"));
        QTimer answer;
        answer.setInterval(30);
        connect(&answer, &QTimer::timeout, &window, [&] {
            if (auto* box = window.findChild<QMessageBox*>(); box && box->isVisible()) box->button(QMessageBox::Yes)->click();
        });
        answer.start();
        panelHost->open(1);
        QTRY_VERIFY(window.findChild<QPushButton*>("sdEject")->isEnabled());
        window.findChild<QPushButton*>("sdEject")->click();
        QTRY_COMPARE_WITH_TIMEOUT(sd->text(), QString("Ejected"), 5000);
        QTRY_VERIFY(shown("sdInsert") && shown("sdExchange") && shown("sdExport") && shown("cardPick") && !shown("sdEject"));
        QVERIFY(sd->isVisibleTo(&window));
        answer.stop();
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        QDir(managedFolder()).removeRecursively();
        QSettings().clear();
    }
    // A fake core that logs each command and its argument (a replay file's lines joined with |) to "sent".
    static void menuCore(const QString& dir, const QByteArray& device) {
        QFile core(dir + "/core");
        QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(QByteArray(R"PY(#!/usr/bin/python3
import socket,sys,threading,os,time
s=socket.socket(socket.AF_UNIX)
s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
here=sys.argv[0].rsplit('/',1)[0]
log=open(here+'/sent','a')
# Lines put in the "emit" file go to the app as shortcut events.
def emit():
    while True:
        time.sleep(0.02)
        try: lines=open(here+'/emit').read().split(); os.remove(here+'/emit')
        except OSError: continue
        for name in lines: s.sendall(b'0\tevent\tshortcut\t'+name.encode().hex().encode()+b'\n')
threading.Thread(target=emit,daemon=True).start()
mic=b'off'
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    text=bytes.fromhex(arg.decode())
    if cmd==b'input-replay': text=open(text,'rb').read().replace(b'\n',b'|')
    if cmd not in (b'keyboard-state',b'network-state',b'device-state',b'sd-state',b'microphone-state'):
        log.write(cmd.decode()+' '+text.decode()+'\n'); log.flush()
    if cmd==b'microphone-mode': mic=text
    if cmd==b'window-id':
        # A "windowid" file holds the answer; without one the core refuses.
        try: s.sendall(i+b'\tok\twindow\t'+open(here+'/windowid','rb').read().strip().hex().encode()+b'\n'); continue
        except OSError: pass
        s.sendall(i+b'\terror\tunsupported\t'+b'The window is not an X11 window.'.hex().encode()+b'\n'); continue
    message={b'keyboard-state':b'detached',b'network-state':b'disabled',b'device-state':b'DEVICE',b'sd-state':b'none',b'microphone-state':mic,b'microphone-mode':mic}.get(cmd,b'done')
    if cmd==b'keyboard-attach': message=b'attached'
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit': break
)PY").replace("DEVICE", device));
        core.close();
        QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }
    // Keys pressed over the Palm come back as shortcut events and do what the bar does.
    void shortcuts() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("Keys", 6, "palm-tx", 0);
        QTemporaryDir files;
        const auto sent = files.path() + "/sent";
        menuCore(files.path(), "palm-tx");
        if (QTest::currentTestFailed()) return;
        auto commands = [&] {
            QFile f(sent); if (!f.open(QIODevice::ReadOnly)) return QStringList();
            return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
        };
        auto press = [&](const QString& names) {
            QFile f(files.path() + "/emit.tmp"); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(names.toUtf8()); f.close();
            QVERIFY(QFile::rename(f.fileName(), files.path() + "/emit"));
            QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(files.path() + "/emit"), 3000);
        };
        MainWindow window(files.path() + "/core"); window.show(); window.openSession(managedSession("Keys"), true);
        auto* bar = window.findChild<RunningBar*>("runningBar");
        QTRY_VERIFY_WITH_TIMEOUT(bar->isVisible() && window.findChild<QPushButton*>("save")->isEnabled(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QAction*>("machineSave")->isEnabled(), 4000);
        press("ctrl+s");
        QTRY_VERIFY_WITH_TIMEOUT(commands().size() == 1 && commands()[0].startsWith("save ") && commands()[0].endsWith("/Keys.frond"), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("save")->isEnabled(), 4000);
        // An unknown name does nothing, and so does Ctrl+B and F11 without the embedded Palm.
        press("ctrl+z");
        press("ctrl+b");
        press("f11");
        QTest::qWait(150);
        QVERIFY(bar->isVisible() && !window.isFullScreen());
        QVERIFY(!window.findChild<QAction*>("machineHideBar")->isVisible());
        // Ctrl+1 opens the Applications panel and closes it again.
        auto* panels = window.findChild<PanelHost*>("panelHost");
        press("ctrl+1");
        QTRY_COMPARE(panels->current(), 0);
        press("ctrl+1");
        QTRY_COMPARE(panels->current(), -1);
        press("ctrl+3");
        QTRY_COMPARE(panels->current(), 2);
        press("ctrl+3");
        QTRY_COMPARE(panels->current(), -1);
        press("ctrl+m");
        QTRY_VERIFY_WITH_TIMEOUT(commands().contains("audio-off "), 4000);
        press("f12");
        QTRY_VERIFY_WITH_TIMEOUT(commands().last().startsWith("input-replay 0 snapshot \""), 4000);
        // The same keys work with the Qt window in front.
        const auto before = commands().size();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("save")->isEnabled(), 4000);
        QTest::keyClick(&window, Qt::Key_S, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(commands().size() == before + 1 && commands().last().startsWith("save "), 4000);
    }
    // The menu rows show their keys, and the keys still fire once: a row never takes the key itself.
    void menuShowsKeys() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("Show", 6, "palm-tx", 0);
        QTemporaryDir files;
        const auto sent = files.path() + "/sent";
        menuCore(files.path(), "palm-tx");
        if (QTest::currentTestFailed()) return;
        auto commands = [&] {
            QFile f(sent); if (!f.open(QIODevice::ReadOnly)) return QStringList();
            return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
        };
        MainWindow window(files.path() + "/core"); window.show(); window.openSession(managedSession("Show"), true);
        auto* bar = window.findChild<RunningBar*>("runningBar");
        QTRY_VERIFY_WITH_TIMEOUT(bar->isVisible() && window.findChild<QAction*>("machineSave")->isEnabled(), 5000);
        auto keys = [&](const char* name) { return window.findChild<QAction*>(name)->text().section('\t', 1); };
        QCOMPARE(keys("machineSave"), QString("Ctrl+S"));
        QCOMPARE(keys("machineSaveCopy"), QString("Ctrl+Shift+S"));
        QCOMPARE(keys("machineSaveStop"), QString("Ctrl+W"));
        QCOMPARE(keys("machineHideBar"), QString("Ctrl+B"));
        QCOMPARE(keys("machineLog"), QString("Ctrl+L"));
        QCOMPARE(keys("machineReset"), QString("Ctrl+R"));
        QCOMPARE(keys("machineDisplay"), QString("Ctrl+3"));
        QCOMPARE(keys("machineConnections"), QString("Ctrl+4"));
        QCOMPARE(keys("machineScreenshot"), QString("F12"));
        QCOMPARE(keys("machineFullScreen"), QString("F11"));
        QVERIFY(keys("machineHardReset").isEmpty() && keys("machineKeyboard").isEmpty());
        QCOMPARE(window.findChild<QAction*>("machineSave")->text().section('\t', 0, 0), QString("Save"));
        // The rows show the keys as text only: no action owns a key, so each key has the one
        // window shortcut and can't be ambiguous.
        for (auto* a : window.findChildren<QAction*>()) QVERIFY2(a->shortcut().isEmpty() || !a->objectName().startsWith("machine"), qPrintable(a->objectName()));
        QStringList seen;
        for (auto* shortcut : window.findChildren<QShortcut*>()) {
            QVERIFY2(!seen.contains(shortcut->key().toString()), qPrintable(shortcut->key().toString()));
            seen << shortcut->key().toString();
        }
        // Each key fires its action once: with the window in front, and with the menu open.
        auto* panels = window.findChild<PanelHost*>("panelHost");
        QTest::keyClick(&window, Qt::Key_3, Qt::ControlModifier);
        QCOMPARE(panels->current(), 2);
        QTest::keyClick(&window, Qt::Key_3, Qt::ControlModifier);
        QCOMPARE(panels->current(), -1);
        // Hide bar is only for the embedded Palm.
        QTest::keyClick(&window, Qt::Key_B, Qt::ControlModifier);
        QVERIFY(bar->isVisible());
        QTest::keyClick(&window, Qt::Key_S, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(commands().size() == 1 && commands()[0].startsWith("save "), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("save")->isEnabled(), 4000);
        bar->menu()->popup(window.mapToGlobal(QPoint(40, 40)));
        QTRY_VERIFY(bar->menu()->isVisible());
        QTest::keyClick(bar->menu(), Qt::Key_S, Qt::ControlModifier);
        QTest::qWait(300);
        QVERIFY(commands().size() <= 2);
        bar->menu()->close();
    }
    // With the core refusing window-id the Palm keeps its own window: bar-only window, no dialog,
    // everything else works.
    void embedFallback() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("Fall", 6, "palm-tx", 0);
        QTemporaryDir files;
        const auto sent = files.path() + "/sent";
        menuCore(files.path(), "palm-tx");
        if (QTest::currentTestFailed()) return;
        auto commands = [&] {
            QFile f(sent); if (!f.open(QIODevice::ReadOnly)) return QStringList();
            return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
        };
        qputenv("FROND_TRY_EMBED", "1");
        {
            MainWindow window(files.path() + "/core"); window.show(); window.openSession(managedSession("Fall"), true);
            auto* bar = window.findChild<RunningBar*>("runningBar");
            QTRY_VERIFY_WITH_TIMEOUT(bar->isVisible() && window.findChild<QAction*>("machineSave")->isEnabled(), 5000);
            QTRY_VERIFY_WITH_TIMEOUT(commands().contains("window-id "), 4000);
            auto* holder = window.findChild<PalmHolder*>("palmHolder");
            QVERIFY(holder && !holder->isVisible() && !holder->embedded());
            // The log names the core's refusal, and the core goes back to following the Palm's size.
            auto* log = window.findChild<QPlainTextEdit*>("logText");
            QTRY_VERIFY_WITH_TIMEOUT(log->toPlainText().contains("The Palm stays in its own window: The window is not an X11 window."), 4000);
            QTRY_VERIFY_WITH_TIMEOUT(commands().contains("display fit=none"), 4000);
            QVERIFY(commands().indexOf("display fit=none") == commands().indexOf("window-id ") + 1);
            // The chain went on after the refusal, and no error box is up.
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QAction*>("machineNetwork")->isEnabled(), 4000);
            QVERIFY(!QApplication::activeModalWidget());
            // Full screen and Hide bar are only offered with the embedded Palm.
            auto* fullScreen = window.findChild<QAction*>("machineFullScreen");
            QVERIFY(fullScreen->isVisible() && !fullScreen->isEnabled());
            QVERIFY(!window.findChild<QAction*>("machineHideBar")->isVisible());
            fullScreen->trigger();
            QVERIFY(!window.isFullScreen() && window.menuBar()->isVisible());
            // The bar-only window: a panel opens under the bar at the size the panel needs.
            auto* panels = window.findChild<PanelHost*>("panelHost");
            panels->open(2);
            QVERIFY(panels->isVisible());
            QCOMPARE(window.width(), 480);
            QVERIFY(panels->maximumWidth() > runningRules::panelWidth);
            // Display shows what applies: no Size without the embedding, Smooth pixels always.
            QVERIFY(!window.findChild<QSlider*>("runningSize")->isVisibleTo(&window));
            QVERIFY(window.findChild<QCheckBox*>("runningSmooth")->isVisibleTo(&window));
            QVERIFY(!window.findChild<QCheckBox*>("runningShowKeyboard")->isVisibleTo(&window));
            QVERIFY(!window.findChild<QComboBox*>("runningSpeed")->isVisibleTo(&window));
            panels->open(-1);
            window.findChild<QAction*>("machineSaveStop")->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(commands().last() == "quit ", 5000);
            QTRY_VERIFY_WITH_TIMEOUT(!bar->isVisible(), 5000);
            QVERIFY(!holder->embedded() && !window.findChild<QWidget*>("runArea")->isVisible());
        }
        qunsetenv("FROND_TRY_EMBED");
    }
    // The core answers with an X11 window that can't be wrapped (offscreen Qt): the same fallback,
    // with a log line that tells it apart from a refusal.
    void embedWrapFails() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("Wrap", 6, "palm-tx", 0);
        QTemporaryDir files;
        const auto sent = files.path() + "/sent";
        menuCore(files.path(), "palm-tx");
        if (QTest::currentTestFailed()) return;
        { QFile id(files.path() + "/windowid"); QVERIFY(id.open(QIODevice::WriteOnly)); id.write("x11 4242\n"); }
        auto commands = [&] {
            QFile f(sent); if (!f.open(QIODevice::ReadOnly)) return QStringList();
            return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
        };
        qputenv("FROND_TRY_EMBED", "1");
        {
            MainWindow window(files.path() + "/core"); window.show(); window.openSession(managedSession("Wrap"), true);
            auto* bar = window.findChild<RunningBar*>("runningBar");
            QTRY_VERIFY_WITH_TIMEOUT(bar->isVisible() && window.findChild<QAction*>("machineSave")->isEnabled(), 5000);
            auto* log = window.findChild<QPlainTextEdit*>("logText");
            QTRY_VERIFY_WITH_TIMEOUT(log->toPlainText().contains("The Palm stays in its own window: window 4242 couldn't be embedded."), 4000);
            QVERIFY(!log->toPlainText().contains("not an X11 window"));
            QVERIFY(!window.findChild<PalmHolder*>("palmHolder")->embedded());
            QTRY_VERIFY_WITH_TIMEOUT(commands().contains("display fit=none"), 4000);
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QAction*>("machineNetwork")->isEnabled(), 4000);
            QVERIFY(!QApplication::activeModalWidget());
            window.findChild<QAction*>("machineSaveStop")->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(commands().last() == "quit ", 5000);
        }
        qunsetenv("FROND_TRY_EMBED");
    }
    // The Display rows that can change while the Palm runs: each is kept in the machine's settings
    // and sent to the core.
    void liveDisplayRows() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("Live", 6, "palm-tx", 0);
        QTemporaryDir files;
        const auto sent = files.path() + "/sent";
        menuCore(files.path(), "palm-tx");
        if (QTest::currentTestFailed()) return;
        auto commands = [&] {
            QFile f(sent); if (!f.open(QIODevice::ReadOnly)) return QStringList();
            return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
        };
        MainWindow window(files.path() + "/core"); window.show(); window.openSession(managedSession("Live"), true);
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QAction*>("machineSave")->isEnabled(), 5000);
        window.findChild<PanelHost*>("panelHost")->open(2);
        auto* smooth = window.findChild<QCheckBox*>("runningSmooth");
        QTRY_VERIFY_WITH_TIMEOUT(smooth->isEnabled(), 4000);
        smooth->click();
        QTRY_VERIFY_WITH_TIMEOUT(commands().contains("display smooth=on"), 4000);
        frondSessions::Preferences kept;
        QVERIFY(frondSessions::readPreferences(managedSession("Live"), &kept));
        QVERIFY(kept.smooth);
        QTRY_VERIFY_WITH_TIMEOUT(smooth->isEnabled(), 4000);
        smooth->click();
        QTRY_VERIFY_WITH_TIMEOUT(commands().contains("display smooth=off"), 4000);
        QVERIFY(frondSessions::readPreferences(managedSession("Live"), &kept) && !kept.smooth);
        // Contrast's own bookkeeping isn't touched by the others.
        QVERIFY(!kept.contrastSet);
    }
    // The arrow keys walk every row of the Machine menu, the red Hard reset… row included, both ways.
    void machineMenuKeys() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("Nav", 6, "palm-tx", 0);
        QTemporaryDir files;
        menuCore(files.path(), "palm-tx");
        if (QTest::currentTestFailed()) return;
        MainWindow window(files.path() + "/core"); window.show(); window.openSession(managedSession("Nav"), true);
        auto* bar = window.findChild<RunningBar*>("runningBar");
        QTRY_VERIFY_WITH_TIMEOUT(bar->isVisible() && window.findChild<QAction*>("machineSave")->isEnabled(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QAction*>("machineHardReset")->isEnabled(), 4000);
        auto* menu = bar->menu();
        QStringList rows;
        for (auto* a : menu->actions()) if (!a->isSeparator() && a->isVisible() && a->isEnabled()) rows << a->objectName();
        QVERIFY(rows.contains("machineHardReset"));
        menu->popup(window.mapToGlobal(QPoint(40, 40)));
        QTRY_VERIFY(menu->isVisible());
        QStringList down, up;
        auto active = [&] { auto* a = menu->activeAction(); return a ? a->objectName() : QString("-"); };
        // A real popup hands keys to the focused row widget first.
        auto key = [&](Qt::Key k) { QTest::keyClick(menu->focusWidget() ? menu->focusWidget() : menu, k); };
        for (int i = 0; i < rows.size(); ++i) { key(Qt::Key_Down); down << active(); }
        QCOMPARE(down, rows);
        for (int i = rows.size() - 2; i >= 0; --i) { key(Qt::Key_Up); up << active(); }
        QStringList back = rows; std::reverse(back.begin(), back.end()); back.removeFirst();
        QCOMPARE(up, back);
        // Enter on the red row triggers it.
        QSignalSpy hard(window.findChild<QAction*>("machineHardReset"), &QAction::triggered);
        QTimer dismiss; dismiss.setInterval(30);
        connect(&dismiss, &QTimer::timeout, &window, [&] { if (auto* box = window.findChild<QMessageBox*>(); box && box->isVisible()) box->done(QMessageBox::Cancel); });
        dismiss.start();
        for (int i = 0; i < rows.size() && active() != "machineHardReset"; ++i) key(Qt::Key_Down);
        QCOMPARE(active(), QString("machineHardReset"));
        key(Qt::Key_Return);
        QCOMPARE(hard.count(), 1);
        QVERIFY(!menu->isVisible());
    }
    // The Machine menu and the bar: each row sends its command to the core.
    void runningMenu() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("Menu", 6, "palm-tx", 0);
        QTemporaryDir files;
        const auto sent = files.path() + "/sent";
        menuCore(files.path(), "palm-tx");
        if (QTest::currentTestFailed()) return;
        auto commands = [&] {
            QFile f(sent); if (!f.open(QIODevice::ReadOnly)) return QStringList();
            return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
        };
        MainWindow window(files.path() + "/core"); window.show(); window.openSession(managedSession("Menu"), true);
        auto* bar = window.findChild<RunningBar*>("runningBar");
        QTRY_VERIFY_WITH_TIMEOUT(bar->isVisible() && window.findChild<QPushButton*>("save")->isEnabled(), 5000);
        QStringList menus;
        for (auto* action : window.menuBar()->actions()) menus << action->text();
        QCOMPARE(menus, QStringList({"&File", "&Machine", "&Help"}));
        QVERIFY(!window.statusBar()->isVisible());
        QCOMPARE(bar->findChild<QLabel*>("barName")->text(), QString("Menu"));
        QCOMPARE(bar->findChild<QLabel*>("barSubtitle")->text(), QString("Exact"));
        auto menuAction = [&](const char* name) { auto* a = window.findChild<QAction*>(name); if (!a) qWarning("no %s", name); return a; };
        // The T|X has no Hold; the rows it has are there and the others are not.
        QVERIFY(!menuAction("machineHold")->isVisible());
        QVERIFY(menuAction("machineMicrophone")->isVisible() && !menuAction("machineMicrophone")->isEnabled());
        for (auto* name : {"machineKeyboard", "machineNetwork", "machineScreenshot", "machineSave", "machineReset"})
            QTRY_VERIFY2_WITH_TIMEOUT(menuAction(name)->isVisible() && menuAction(name)->isEnabled(), name, 4000);
        QTimer answer; answer.setInterval(30);
        QString choice = "Yes";
        connect(&answer, &QTimer::timeout, &window, [&] {
            auto* box = window.findChild<QMessageBox*>();
            if (!box || !box->isVisible()) return;
            for (auto* b : box->buttons()) if (b->text() == choice || (choice == "Yes" && box->standardButton(b) == QMessageBox::Yes)) { b->click(); return; }
        });
        answer.start();
        auto expect = [&](const QStringList& list) {
            QTRY_COMPARE_WITH_TIMEOUT(commands(), list, 4000);
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("save")->isEnabled(), 4000);
        };
        menuAction("machineSave")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(commands().size() == 1 && commands()[0].startsWith("save ") && commands()[0].endsWith("/Menu.frond"), 4000);
        QStringList want = commands();
        expect(want);
        menuAction("machineKeyboard")->trigger();
        want << "keyboard-attach on"; expect(want);
        QTRY_VERIFY(menuAction("machineKeyboard")->isChecked());
        QVERIFY(bar->findChild<QAction*>("barKeyboard")->isVisible());
        menuAction("machineNetwork_internet")->trigger();
        want << "network-mode internet"; expect(want);
        QTRY_VERIFY(menuAction("machineNetwork_internet")->isChecked());
        // Screenshot and Power are replays.
        menuAction("machineScreenshot")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(commands().size() >= want.size() + 1, 4000);
        QVERIFY(commands()[want.size()].startsWith("input-replay 0 snapshot \""));
        QVERIFY(commands()[want.size()].contains("/Frond/Frond Menu "));
        want = commands();
        expect(want);
        window.findChild<QAction*>("barPower")->trigger();
        want << "input-replay 0 key-down power|80 key-up power|"; expect(want);
        window.findChild<QAction*>("barSound")->trigger();
        want << "audio-off "; expect(want);
        QVERIFY(!window.findChild<QAction*>("barSound")->isChecked());
        choice = "Yes";
        menuAction("machineReset")->trigger();
        want << "reset "; expect(want);
        // Hard reset is a coloured menu row, so it is a widget action; it sits in the menu bar's and the dots' menu.
        QVERIFY(qobject_cast<QWidgetAction*>(menuAction("machineHardReset")));
        QVERIFY(bar->menu()->actions().contains(menuAction("machineHardReset")));
        choice = "Erase";
        menuAction("machineHardReset")->trigger();
        want << "hard-reset "; expect(want);
        // Hide bar leaves the thin strip with its tab, and the machine remembers it.
        auto* handle = window.findChild<BarHandle*>("barHandle");
        QVERIFY(!handle->isVisible());
        menuAction("machineHideBar")->trigger();
        QVERIFY(!bar->isVisible() && handle->isVisible());
        QCOMPARE(handle->height(), 8);
        QCOMPARE(menuAction("machineHideBar")->text(), QString("Show bar\tCtrl+B"));
        {
            frondSessions::Preferences kept;
            QVERIFY(frondSessions::readPreferences(managedSession("Menu"), &kept));
            QVERIFY(!kept.barShown);
        }
        QTest::mouseClick(handle, Qt::LeftButton);
        QVERIFY(bar->isVisible() && !handle->isVisible());
        {
            frondSessions::Preferences kept;
            QVERIFY(frondSessions::readPreferences(managedSession("Menu"), &kept));
            QVERIFY(kept.barShown);
        }
        menuAction("machineHideBar")->trigger();
        QVERIFY(!bar->isVisible());
        menuAction("machineHideBar")->trigger();
        QVERIFY(bar->isVisible());
        QVERIFY(!menuAction("machineFullScreen")->isEnabled());
        menuAction("machineSaveStop")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(commands().last() == "quit ", 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!bar->isVisible() && window.statusBar()->isVisible(), 5000);
        menus.clear();
        for (auto* action : window.menuBar()->actions()) menus << action->text();
        QCOMPARE(menus, QStringList({"&File", "&Help"}));
        answer.stop();
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        QDir(managedFolder()).removeRecursively();
        QSettings().clear();
    }
    // The Applications panel: the list, Launch, Delete (the app first, then its data), a refusal,
    // Other… and a drop.
    void appsPanel() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("Apps", 6, "palm-tx", 0);
        QTemporaryDir files;
        const auto sent = files.path() + "/sent";
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import os,socket,sys
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
log=open(sys.argv[0].rsplit('/',1)[0]+'/sent','a')
dbs=[('appl','memo',1,'Memos'),('DATA','memo',0,'Memo Data'),('appl','nots',1,'Notes'),('DATA','nots',0,'Notes Extra'),
     ('appl','lock',1,'Locked'),('DATA','lock',0,'Locked Data'),('DATA','othr',0,'Other Data')]
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    text=bytes.fromhex(arg.decode()).decode()
    if cmd not in (b'keyboard-state',b'network-state',b'device-state',b'sd-state',b'microphone-state'):
        log.write(cmd.decode()+' '+text+'\n'); log.flush()
    message={b'keyboard-state':b'detached',b'network-state':b'disabled',b'device-state':b'palm-tx',b'sd-state':b'none',b'microphone-state':b'off'}.get(cmd,b'done')
    if cmd==b'db-list':
        message='\n'.join(t.encode().hex()+' '+c.encode().hex()+' %08x '%a+n.encode().hex() for t,c,a,n in dbs).encode()
    if cmd==b'delete-db':
        if text=='Locked':
            s.sendall(i+b'\terror\trunning\t'+b'The application is running.'.hex().encode()+b'\n'); continue
        dbs=[d for d in dbs if d[3]!=text]
    if cmd==b'install': dbs.append(('appl','inst',1,os.path.basename(text).rsplit('.',1)[0]))
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit': break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        auto commands = [&] {
            QFile f(sent); if (!f.open(QIODevice::ReadOnly)) return QStringList();
            return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
        };
        MainWindow window(files.path() + "/core"); window.show(); window.openSession(managedSession("Apps"), true);
        auto* bar = window.findChild<RunningBar*>("runningBar");
        auto* panels = window.findChild<PanelHost*>("panelHost");
        auto* apps = window.findChild<AppsPanel*>("appsPanel");
        QTRY_VERIFY_WITH_TIMEOUT(bar->isVisible() && window.findChild<QPushButton*>("save")->isEnabled(), 5000);
        // No panel yet: the window is the menu bar and the bar.
        QVERIFY(!panels->isVisible() && !window.findChild<QWidget*>("home")->isVisible());
        const int slim = window.height();
        QVERIFY(slim < 200);
        window.findChild<QAction*>("barApplications")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(apps->names(), QStringList({"Locked", "Memos", "Notes"}), 5000);
        QVERIFY(panels->isVisible() && window.height() > slim + 300);
        QCOMPARE(window.findChild<QLabel*>("panelTitle")->text(), QString("Applications"));
        QVERIFY(window.findChild<QAction*>("barApplications")->isChecked());
        if (const auto shot = qEnvironmentVariable("FROND_TEST_SHOT"); !shot.isEmpty()) window.grab().save(shot);
        auto row = [&](const QString& app, const char* button) -> QToolButton* {
            auto* list = apps->findChild<QListWidget*>("appsList");
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->data(Qt::UserRole).toString() == app) return list->itemWidget(list->item(i))->findChild<QToolButton*>(button);
            return nullptr;
        };
        auto idle = [&] { QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("save")->isEnabled(), 5000); };
        row("Memos", "appLaunch")->click();
        QTRY_VERIFY_WITH_TIMEOUT(commands().contains("launch-app Memos"), 5000);
        idle();
        // Delete asks first; No leaves everything.
        QTimer answer; answer.setInterval(30);
        QString choice = "No";
        connect(&answer, &QTimer::timeout, &window, [&] {
            if (auto* dialog = window.findChild<QDialog*>("otherDialog"); dialog && dialog->isVisible()) {
                dialog->findChild<QComboBox*>("otherName")->setEditText("Memo Data");
                dialog->findChild<QPushButton*>("otherLaunch")->click();
                return;
            }
            auto* box = window.findChild<QMessageBox*>();
            if (!box || !box->isVisible()) return;
            for (auto* b : box->buttons()) if (b->text().remove('&') == choice || (choice == "Yes" && box->standardButton(b) == QMessageBox::Yes) ||
                                               (choice == "No" && box->standardButton(b) == QMessageBox::No)) { b->click(); return; }
        });
        answer.start();
        row("Memos", "appDelete")->click();
        QTest::qWait(300);
        QVERIFY(!commands().contains("delete-db Memos"));
        choice = "Yes";
        row("Memos", "appDelete")->click();
        QTRY_COMPARE_WITH_TIMEOUT(apps->names(), QStringList({"Locked", "Notes"}), 5000);
        {
            // A fresh list first, then the application, then the data with its creator.
            const auto list = commands();
            const int at = int(list.indexOf("delete-db Memos"));
            QVERIFY(at > 0 && list[at - 1] == "db-list ");
            QCOMPARE(list[at + 1], QString("delete-db Memo Data"));
            QVERIFY(!list.contains("delete-db Notes") && !list.contains("delete-db Other Data"));
        }
        idle();
        // The core refuses the open application; its data is left.
        choice = "Yes";
        row("Locked", "appDelete")->click();
        QTRY_VERIFY_WITH_TIMEOUT(commands().contains("delete-db Locked"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.findChildren<QMessageBox*>().isEmpty() && window.findChildren<QMessageBox*>().last()->text() == "The application is running.", 5000);
        for (auto* box : window.findChildren<QMessageBox*>()) if (box->isVisible() && box->text().contains("running")) box->accept();
        QVERIFY(!commands().contains("delete-db Locked Data"));
        QCOMPARE(apps->names(), QStringList({"Locked", "Notes"}));
        idle();
        // Other… takes any database by name.
        apps->findChild<QPushButton*>("otherApp")->click();
        QTRY_VERIFY_WITH_TIMEOUT(commands().contains("launch-app Memo Data"), 5000);
        idle();
        // Files dropped on the window install, and the list picks them up.
        QFile(files.filePath("Dropped.prc")).open(QIODevice::WriteOnly);
        auto* mime = new QMimeData;
        mime->setUrls({QUrl::fromLocalFile(files.filePath("Dropped.prc")), QUrl::fromLocalFile(files.filePath("note.txt"))});
        QDragEnterEvent enter(QPoint(5, 5), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(5, 5), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &drop);
        QTRY_VERIFY_WITH_TIMEOUT(commands().contains("install " + files.filePath("Dropped.prc")), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(apps->names(), QStringList({"Dropped", "Locked", "Notes"}), 5000);
        delete mime;
        // Ctrl+2 swaps to the Card panel and Ctrl+1 comes back; the same key closes it.
        auto press = [&](const char* keys) {
            for (auto* shortcut : window.findChildren<QShortcut*>()) if (shortcut->key() == QKeySequence(QString::fromLatin1(keys))) { emit shortcut->activated(); return; }
            QFAIL(keys);
        };
        press("Ctrl+2");
        QTRY_COMPARE_WITH_TIMEOUT(panels->current(), 1, 2000);
        press("Ctrl+2");
        QTRY_COMPARE_WITH_TIMEOUT(panels->current(), -1, 2000);
        press("Ctrl+1");
        QTRY_COMPARE_WITH_TIMEOUT(panels->current(), 0, 2000);
        // Close with the panel's button; the window gets small again.
        window.findChild<QToolButton*>("panelClose")->click();
        QVERIFY(!panels->isVisible() && !window.findChild<QAction*>("barApplications")->isChecked());
        QTRY_COMPARE_WITH_TIMEOUT(window.height(), slim, 2000);
        answer.stop();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("stop")->isEnabled(), 5000);
        window.findChild<QPushButton*>("stop")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!bar->isVisible(), 5000);
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        QDir(managedFolder()).removeRecursively();
        QSettings().clear();
    }
    // The replay turns the microphone off in the core; the bar turns it back on.
    void runningMenuMicrophone() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("Mic", 6, "palm-zire-72", 0);
        QTemporaryDir files;
        const auto sent = files.path() + "/sent";
        menuCore(files.path(), "palm-zire-72");
        if (QTest::currentTestFailed()) return;
        auto commands = [&] {
            QFile f(sent); if (!f.open(QIODevice::ReadOnly)) return QStringList();
            return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
        };
        MainWindow window(files.path() + "/core"); window.show(); window.openSession(managedSession("Mic"), true);
        auto* mic = window.findChild<QAction*>("machineMicrophone");
        QTRY_VERIFY_WITH_TIMEOUT(mic->isVisible() && mic->isEnabled(), 5000);
        mic->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(commands(), QStringList({"microphone-mode on"}), 4000);
        QTRY_VERIFY(mic->isChecked());
        QTRY_VERIFY(window.findChild<QAction*>("barPower")->isEnabled());
        window.findChild<QAction*>("barPower")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(commands(), QStringList({"microphone-mode on", "input-replay 0 key-down power|80 key-up power|", "microphone-mode on"}), 4000);
        window.findChild<QAction*>("machineSaveStop")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(commands().last() == "quit ", 5000);
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        QDir(managedFolder()).removeRecursively();
        QSettings().clear();
    }
    void sdCreateSizes_data() {
        QTest::addColumn<QString>("device");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QStringList>("labels");
        const QStringList tx{"32 MB", "64 MB", "128 MB", "256 MB", "512 MB", "1 GB", "2 GB"};
        QTest::newRow("tx") << "palm-tx" << "create" << tx;
        QTest::newRow("tx-cancel") << "palm-tx" << "cancel" << tx;
        QTest::newRow("e2") << "palm-e2" << "create" << QStringList{"32 MB", "64 MB", "128 MB", "256 MB", "512 MB", "1 GB",
                                                                      "2 GB · Too big for Tungsten E2 (greyed)"};
    }
    // New card in the running picker offers 32 MB to the largest SD card, greys what the machine
    // can't take, makes the card with the card tool (progress, Cancel) and puts it in.
    void sdCreateSizes() {
        QFETCH(QString, device); QFETCH(QString, mode); QFETCH(QStringList, labels);
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively(); QDir(cardsLibrary()).removeRecursively();
        putSession("Card", 6, device.toUtf8(), 1);
        QTemporaryDir files;
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        auto script = QByteArray(R"PY(#!/usr/bin/python3
import pathlib,socket,sys
trace=pathlib.Path(__file__).with_name('commands')
s=socket.socket(socket.AF_UNIX);s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
card='none'; device='DEVICE'
for line in s.makefile('rb'):
    i,raw,arg=line.rstrip(b'\n').split(b'\t'); cmd=raw.decode(); arg=bytes.fromhex(arg.decode()).decode()
    with trace.open('a') as t: t.write(cmd+'\t'+arg+'\n')
    message='done'
    if cmd=='device-state': message=device
    elif cmd=='sd-state': message=card
    elif cmd=='network-state': message='disabled'
    elif cmd=='keyboard-state': message='detached'
    elif cmd=='sd-put-in': card='inserted'
    s.sendall(i+b'\tok\tresult\t'+message.encode().hex().encode()+b'\n')
    if cmd=='quit': break
)PY");
        script.replace("DEVICE", device.toUtf8());
        core.write(script); core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        MainWindow window(core.fileName()); window.show();
        window.openSession(managedSession("Card"), true);
        auto* state = window.findChild<QLabel*>("sdState");
        auto* pick = window.findChild<QPushButton*>("cardPick");
        QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("No card"), 5000);
        showCardPanel(pick); QTRY_VERIFY(pick->isVisible() && pick->isEnabled());
        QCOMPARE(pick->text(), QString("No card"));
        QVERIFY(!state->isVisible());
        std::unique_ptr<SlowCardTool> slow;
        if (mode == "cancel") slow = std::make_unique<SlowCardTool>();
        bool sawProgress = false;
        const auto size = mode == "cancel" ? (device == "palm-e2" ? "1 GB" : "2 GB") : "32 MB";
        const auto offered = pickNewCard(window, "SD card", size, mode == "cancel", &sawProgress);
        QCOMPARE(offered, labels);
        if (mode == "cancel") {
            QVERIFY(sawProgress);
            QVERIFY(frondCards::entries(cardsLibrary()).isEmpty());
            QVERIFY(QDir(cardsLibrary()).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).filter(".new-").isEmpty());
            QCOMPARE(state->text(), QString("No card"));
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Inserted"), 10000);
            const auto card = onlyCard();
            QCOMPARE(card.name, QString("SD card"));
            QCOMPARE(pick->text(), QString("SD card"));
            const auto dir = frondSessions::machineDirOf(managedSession("Card"));
            QVERIFY(QFileInfo(dir + "/sd.frondsd").isFile());
            const auto slot = frondCards::readSlot(dir);
            QVERIFY(slot.card == card.id && slot.swap);
            QFile trace(files.filePath("commands")); QVERIFY(trace.open(QIODevice::ReadOnly));
            QVERIFY(trace.readAll().contains("sd-put-in\t" + card.identity.toHex() + " writable\n"));
        }
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        QDir(managedFolder()).removeRecursively(); QDir(cardsLibrary()).removeRecursively(); QSettings().clear();
    }
    // A card put in while the machine was off resumes ejected; the core's card event when it goes
    // in shows it inserted.
    void swappedCardEvent() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("Swap", 6, "palm-tx", 1);
        QTemporaryDir files;
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        QVERIFY(core.write(R"PY(#!/usr/bin/python3
import socket,sys,threading,time
s=socket.socket(socket.AF_UNIX);s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
lock=threading.Lock()
def send(b):
    with lock: s.sendall(b)
def later():
    time.sleep(1.5); send(b'0\tevent\tcard\t'+b'inserted'.hex().encode()+b'\n')
for line in s.makefile('rb'):
    i,raw,arg=line.rstrip(b'\n').split(b'\t'); cmd=raw.decode()
    message={'device-state':'palm-tx','sd-state':'ejected','network-state':'disabled','keyboard-state':'detached'}.get(cmd,'done')
    send(i+b'\tok\tresult\t'+message.encode().hex().encode()+b'\n')
    if cmd=='sd-state': threading.Thread(target=later,daemon=True).start()
    if cmd=='quit': break
)PY") > 0);
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        MainWindow window(core.fileName()); window.show();
        window.openSession(managedSession("Swap"), true);
        auto* state = window.findChild<QLabel*>("sdState");
        QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Ejected"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Inserted"), 5000);
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        QDir(managedFolder()).removeRecursively(); QSettings().clear();
    }
    // A Memory Stick Pro made in the picker shows the progress dialog, and Cancel stops the card tool.
    void memoryStickCancel() {
        const auto device = frondFirmware::deviceFromId("sony-clie-tg50");
        if (!frondFirmware::profile(device).frontend.memoryStick) QSKIP("Memory Stick is not qualified yet");
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively(); QDir(cardsLibrary()).removeRecursively();
        putSession("Stick", 6, "sony-clie-tg50", 1);
        QTemporaryDir files;
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        QVERIFY(core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,sys
trace=pathlib.Path(__file__).with_name('commands')
s=socket.socket(socket.AF_UNIX);s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
for line in s.makefile('rb'):
    i,raw,arg=line.rstrip(b'\n').split(b'\t'); cmd=raw.decode(); arg=bytes.fromhex(arg.decode()).decode()
    with trace.open('a') as t: t.write(cmd+'\t'+arg+'\n')
    message={'device-state':'sony-clie-tg50','hold-state':'off','network-state':'disabled','keyboard-state':'detached','ms-state':'none'}.get(cmd,'done')
    s.sendall(i+b'\tok\tresult\t'+message.encode().hex().encode()+b'\n')
    if cmd=='quit': break
)PY") > 0);
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        MainWindow window(core.fileName()); window.show();
        window.openSession(managedSession("Stick"), true);
        auto* state = window.findChild<QLabel*>("sdState");
        QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("No card"), 5000);
        QTRY_VERIFY(window.findChild<QPushButton*>("cardPick")->isEnabled());
        QVERIFY(!state->isVisible());
        QCOMPARE(window.findChild<QPushButton*>("cardPick")->text(), QString("No card"));
        SlowCardTool slow;
        bool sawProgress = false;
        const auto offered = pickNewCard(window, "Memory Stick Pro", "2 GB", true, &sawProgress);
        QCOMPARE(offered, QStringList({"256 MB", "512 MB", "1 GB", "2 GB"}));
        QVERIFY(sawProgress);
        QVERIFY(frondCards::entries(cardsLibrary()).isEmpty());
        QVERIFY(QDir(cardsLibrary()).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).filter(".new-").isEmpty());
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        QFile trace(files.filePath("commands")); QVERIFY(trace.open(QIODevice::ReadOnly));
        QVERIFY(!trace.readAll().contains("ms-put-in"));
        QDir(managedFolder()).removeRecursively(); QDir(cardsLibrary()).removeRecursively(); QSettings().clear();
    }
    void memoryStickControls() {
        const auto device = frondFirmware::deviceFromId("sony-clie-tg50");
        if (!frondFirmware::profile(device).frontend.memoryStick) QSKIP("Memory Stick is not qualified yet");
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("Stick", 6, "sony-clie-tg50", 1);
        QTemporaryDir files;
        const auto tracePath = files.filePath("commands");
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        auto script = QByteArray(R"PY(#!/usr/bin/python3
import pathlib,socket,sys
trace=pathlib.Path(__file__).with_name('commands')
s=socket.socket(socket.AF_UNIX);s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
card='none'; protection='writable'
for line in s.makefile('rb'):
    i,raw,arg=line.rstrip(b'\n').split(b'\t'); cmd=raw.decode(); arg=bytes.fromhex(arg.decode()).decode()
    with trace.open('a') as f: f.write(cmd+'\t'+arg+'\n')
    message='done'
    if cmd.startswith('sd-'): raise AssertionError('Memory Stick used SD command')
    if cmd=='device-state': message='sony-clie-tg50'
    elif cmd=='hold-state': message='off'
    elif cmd=='network-state': message='disabled'
    elif cmd=='keyboard-state': message='detached'
    elif cmd=='ms-state': message=card
    elif cmd=='ms-protection': message=protection
    elif cmd=='ms-put-in':
        assert arg.endswith(' writable') and len(arg.split(' ')[0])==64 and card=='none';card='inserted'
    elif cmd=='ms-eject': assert card=='inserted';card='ejected'
    elif cmd=='ms-reinsert': assert card=='ejected';card='inserted'
    elif cmd=='ms-read-only':
        assert card=='ejected';protection='read-only' if arg=='on' else 'writable';message=protection
    s.sendall(i+b'\tok\tresult\t'+message.encode().hex().encode()+b'\n')
    if cmd=='quit': break
)PY");
        core.write(script); core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        MainWindow window(core.fileName()); window.show();
        window.openSession(managedSession("Stick"), true);
        auto* state = window.findChild<QLabel*>("sdState");
        auto* protection = window.findChild<QCheckBox*>("cardReadOnly");
        QDir(cardsLibrary()).removeRecursively();
        QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("No card"), 5000);
        showCardPanel(window.findChild<QPushButton*>("cardPick")); QTRY_VERIFY(window.findChild<QPushButton*>("cardPick")->isVisible() && window.findChild<QPushButton*>("cardPick")->isEnabled());
        QCOMPARE(window.findChild<QLabel*>("storageHeading")->text(), QString("Memory Stick"));
        QVERIFY(!window.findChild<QPushButton*>("sdExchange")->isVisible());
        // New card offers the six classic sizes smallest first, then the Pro type with its own.
        const auto offered = pickNewCard(window, "Memory Stick", "4 MB", false);
        QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Inserted"), 10000);
        QCOMPARE(offered, QStringList({"4 MB", "8 MB", "16 MB", "32 MB", "64 MB", "128 MB"}));
        QCOMPARE(window.findChild<QPushButton*>("cardPick")->text(), QString("Memory Stick"));
        QCOMPARE(onlyCard().kind, QString("ms"));
        QTRY_VERIFY(protection->isVisible()); QVERIFY(!protection->isEnabled());
        QTimer answer; answer.setInterval(10);
        connect(&answer, &QTimer::timeout, &window, [&] {
            if (auto* box = window.findChild<QMessageBox*>(); box && box->isVisible()) box->button(QMessageBox::Yes)->click();
        });
        answer.start();
        auto* eject = window.findChild<QPushButton*>("sdEject");
        QTRY_VERIFY(eject->isVisible() && eject->isEnabled());
        eject->click();
        QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Ejected"), 5000);
        QTRY_VERIFY(protection->isEnabled()); protection->click();
        QTRY_VERIFY(protection->isChecked() && protection->isEnabled());
        window.findChild<QPushButton*>("sdInsert")->click();
        QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Inserted"), 5000);
        QVERIFY(protection->isChecked() && !protection->isEnabled());
        QVERIFY(window.findChild<QPushButton*>("sdExchange")->isVisible());
        answer.stop(); window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        QFile trace(tracePath); QVERIFY(trace.open(QIODevice::ReadOnly));
        const auto commands = trace.readAll();
        QVERIFY(commands.contains("ms-put-in\t"));
        QVERIFY(!commands.contains("ms-create"));
        QVERIFY(commands.contains("ms-read-only\ton\n"));
        QVERIFY(commands.contains("ms-reinsert\t\n"));
        QVERIFY(!commands.contains("sd-"));
        QDir(managedFolder()).removeRecursively(); QDir(cardsLibrary()).removeRecursively(); QSettings().clear();
    }
    void qualifiedFirmwareSettings() {
        const auto norPath = qEnvironmentVariable("FROND_TEST_QUALIFIED_NOR");
        if (norPath.isEmpty()) QSKIP("FROND_TEST_QUALIFIED_NOR not set");
        const auto original = digest(norPath);
        const auto device = frondFirmware::detectDevice(norPath);
        QVERIFY(frondAudio::qualified(device, {}, norPath));
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        const auto firmwareRoot = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/frond/firmware";
        QDir(firmwareRoot).removeRecursively();
        QTemporaryDir files;
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,struct,sys,zlib
pathlib.Path(sys.argv[0]+'.args').write_text('\n'.join(sys.argv[1:]))
if sys.argv[1]=='--device':
    device=sys.argv[2]; nor=pathlib.Path(sys.argv[3]).read_bytes()
    data=struct.pack('<II',0x19800819,6)+device.encode().ljust(32,b'\0')+struct.pack('<8I',0,0,0,len(nor),0,0,0,0)+zlib.compress(nor)
else:
    data=pathlib.Path(sys.argv[1]).read_bytes(); device=data[8:40].split(b'\0')[0].decode()
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    if cmd==b'save': pathlib.Path(bytes.fromhex(arg.decode()).decode()).write_bytes(data)
    message={b'device-state':device.encode(),b'keyboard-state':b'detached',b'network-state':b'disabled',b'sd-state':b'none'}.get(cmd,b'done')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit': break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto path = frondSessions::filePath(managedFolder(), frondFirmware::deviceName(device));
        {
            MainWindow window(core.fileName()); window.show();
            QTimer poll; poll.setInterval(20);
            connect(&poll, &QTimer::timeout, &window, [&] {
                auto* dialog = window.findChild<FirmwareDialog*>();
                if (!dialog || !dialog->isVisible()) return;
                dialog->findChild<QLineEdit*>("nor")->setText(norPath);
                QVERIFY(dialog->machineSettings->fastAudio->isEnabled());
                dialog->machineSettings->fastAudio->click();
                dialog->findChild<QPushButton*>("import")->click();
            });
            poll.start(); window.findChild<QPushButton*>("addDevice")->click();
            QTRY_VERIFY(window.findChild<QCheckBox*>("sound")->isEnabled()); poll.stop();
            QFile args(core.fileName() + ".args"); QVERIFY(args.open(QIODevice::ReadOnly));
            QVERIFY(args.readAll().contains("--fast-audio"));
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        }
        QVERIFY(frondAudio::qualified(device, path, {}));
        {
            MainWindow next(core.fileName()); next.show();
            bool edited = false;
            QTimer poll; poll.setInterval(20);
            connect(&poll, &QTimer::timeout, &next, [&] {
                if (auto* dialog = next.findChild<MachineSettingsDialog*>(); dialog && dialog->isVisible()) {
                    QVERIFY(dialog->controls->fastAudio->isEnabled());
                    QVERIFY(dialog->controls->fastAudio->isChecked());
                    edited = true; dialog->reject();
                }
            });
            poll.start(); rowAction(next, 0, "editSession"); QTRY_VERIFY(edited); poll.stop();
            next.openSession(path, true);
            QTRY_VERIFY(next.findChild<QCheckBox*>("sound")->isEnabled());
            QFile args(core.fileName() + ".args"); QVERIFY(args.open(QIODevice::ReadOnly));
            QVERIFY(args.readAll().contains("--fast-audio"));
            next.close(); QTRY_VERIFY_WITH_TIMEOUT(!next.isVisible(), 5000);
        }
        QCOMPARE(digest(norPath), original);
        QDir(managedFolder()).removeRecursively(); QDir(firmwareRoot).removeRecursively(); QSettings().clear();
    }
    void machineLaunchPreferences() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("One", 6, "palm-tx", 0); putSession("Two", 6, "palm-tx", 1);
        const auto one = managedSession("One"), two = managedSession("Two");
        QVERIFY(frondSessions::writePreferences(one, {7, true, false, true}).isEmpty());
        QVERIFY(frondSessions::writePreferences(two, {4, false, true, false}).isEmpty());
        QTemporaryDir files;
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,sys
pathlib.Path(sys.argv[0]+'.args').write_text('\n'.join(sys.argv[1:]))
data=pathlib.Path(sys.argv[1]).read_bytes()
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    with open(sys.argv[0]+'.commands','ab') as trace: trace.write(cmd+b'\n')
    if cmd==b'save': pathlib.Path(bytes.fromhex(arg.decode()).decode()).write_bytes(data)
    message={b'device-state':b'palm-tx',b'keyboard-state':b'detached',b'network-state':b'disabled',b'sd-state':b'none'}.get(cmd,b'done')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit': break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        auto read = [&](const QString& suffix) {
            QFile file(core.fileName() + suffix); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        const auto copy = files.filePath("copy.frond");
        const auto copyFrond = files.filePath("copy/copy.frond");
        {
            MainWindow window(core.fileName()); window.show(); window.openSession(one, true);
            auto* sound = window.findChild<QCheckBox*>("sound");
            QTRY_VERIFY(sound->isEnabled()); QVERIFY(!sound->isChecked());
            QVERIFY(read(".args").contains("--window-scale\n1.75"));
            QVERIFY(read(".args").contains("--no-sound") && read(".args").contains("--smooth-lcd") && !read(".args").contains("--fast-audio"));
            QVERIFY(read(".args").contains("--low-latency-queue"));
            QVERIFY(!read(".args").contains("--mips"));
            sound->click(); QTRY_VERIFY(read(".commands").contains("audio-on"));
            frondSessions::Preferences prefs;
            QVERIFY(frondSessions::readPreferences(one, &prefs)); QVERIFY(prefs.audio);
            QVERIFY(frondSessions::readPreferences(two, &prefs)); QCOMPARE(prefs.scale, 4); QVERIFY(!prefs.smooth && !prefs.fastAudio && prefs.audio);
            const auto settingsBeforeCopy = digest(frondSessions::preferencesPath(one));
            const auto guestBeforeCopy = digest(one);
            QString chosenCopy, emittedCopy, fixtureError;
            QTimer deadline; deadline.setSingleShot(true); deadline.setInterval(5000);
            connect(&deadline, &QTimer::timeout, &window, [&] {
                fixtureError = "SaveCopy dialog deadline expired";
                for (auto* dialog : window.findChildren<QFileDialog*>())
                    if (dialog->parentWidget() == &window && dialog->windowTitle() == "Save a Copy" && dialog->isVisible()) dialog->reject();
            });
            QTRY_VERIFY(window.findChild<QPushButton*>("saveCopy")->isEnabled());
            deadline.start();
            QMetaObject::invokeMethod(&window, [&] {
                auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
                if (!dialog || dialog->parentWidget() != &window || dialog->windowTitle() != "Save a Copy") {
                    fixtureError = "Queued SaveCopy action ran without the owned dialog"; return;
                }
                connect(dialog, &QFileDialog::fileSelected, &window, [&](const QString& path) { emittedCopy = path; });
                auto* filename = dialog->findChild<QLineEdit*>("fileNameEdit");
                if (!filename) { fixtureError = "SaveCopy filename field missing"; dialog->reject(); return; }
                filename->setText(copy);
                chosenCopy = dialog->selectedFiles().value(0);
                if (chosenCopy != copy) { fixtureError = "SaveCopy selected the wrong path"; dialog->reject(); return; }
                if (!QMetaObject::invokeMethod(dialog, "accept")) {
                    fixtureError = "SaveCopy accept method missing"; dialog->reject();
                }
            }, Qt::QueuedConnection);
            window.findChild<QPushButton*>("saveCopy")->click();
            deadline.stop();
            QVERIFY2(fixtureError.isEmpty(), qPrintable(fixtureError));
            QCOMPARE(chosenCopy, copy); QCOMPARE(emittedCopy, copy);
            QTRY_VERIFY(read(".commands").contains("save"));
            QTRY_VERIFY(QFileInfo::exists(copyFrond));
            QCOMPARE(digest(copyFrond), guestBeforeCopy); QCOMPARE(digest(one), guestBeforeCopy);
            QCOMPARE(digest(frondSessions::preferencesPath(one)), settingsBeforeCopy);
            QVERIFY(frondSessions::readPreferences(copyFrond, &prefs));
            QCOMPARE(prefs.scale, 7); QVERIFY(prefs.smooth && prefs.fastAudio && prefs.audio);
            QVERIFY(!QSettings().contains("lastSession"));
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
            QCOMPARE(QSettings().value("lastSession").toString(), QFileInfo(one).canonicalFilePath());
        }
        {
            // An ARM device's Uncapped speed is a high --mips; Original passes none, so the core uses the device's own.
            frondSessions::Preferences uncapped;
            QVERIFY(frondSessions::readPreferences(two, &uncapped)); uncapped.uncapped = true;
            QVERIFY(frondSessions::writePreferences(two, uncapped).isEmpty());
            MainWindow next(core.fileName()); next.show(); next.openSession(two, true);
            auto* sound = next.findChild<QCheckBox*>("sound"); QTRY_VERIFY(sound->isEnabled()); QVERIFY(sound->isChecked());
            QVERIFY(read(".args").contains("--window-scale\n1"));
            QVERIFY(!read(".args").contains("--no-sound") && !read(".args").contains("--smooth-lcd") && !read(".args").contains("--fast-audio"));
            QVERIFY(!read(".args").contains("--low-latency-queue"));
            QVERIFY(read(".args").contains("--mips\n1000\n"));
            sound->click(); QTRY_VERIFY(read(".commands").contains("audio-off"));
            frondSessions::Preferences prefs;
            QVERIFY(frondSessions::readPreferences(two, &prefs)); QVERIFY(!prefs.audio);
            QVERIFY(frondSessions::readPreferences(one, &prefs)); QVERIFY(prefs.audio);
            next.close(); QTRY_VERIFY_WITH_TIMEOUT(!next.isVisible(), 5000);
        }
        QDir(managedFolder()).removeRecursively(); QSettings().clear();
    }
    void machineControlVisibilityLaunch() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("TG50", 6, "sony-clie-tg50", 0);
        const auto path = managedSession("TG50");
        const auto guest = digest(path);
        QTemporaryDir files;
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,sys
pathlib.Path(sys.argv[0]+'.args').write_text('\n'.join(sys.argv[1:]))
data=pathlib.Path(sys.argv[1]).read_bytes()
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    if cmd==b'save': pathlib.Path(bytes.fromhex(arg.decode()).decode()).write_bytes(data)
    message={b'device-state':b'sony-clie-tg50',b'keyboard-state':b'detached',b'network-state':b'disabled',b'sd-state':b'none'}.get(cmd,b'done')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit': break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        for (bool keyboard : {false, true})
        for (auto jog : {frondSessions::JogControls::hidden, frondSessions::JogControls::screen, frondSessions::JogControls::keyboard}) {
            frondSessions::Preferences prefs;
            prefs.showKeyboard = keyboard; prefs.jogControls = jog; prefs.microphone = true;
            QVERIFY(frondSessions::writePreferences(path, prefs).isEmpty());
            QFile::remove(core.fileName() + ".args");
            MainWindow window(core.fileName()); window.show(); window.openSession(path, true);
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("stop")->isEnabled(), 5000);
            QFile args(core.fileName() + ".args"); QVERIFY(args.open(QIODevice::ReadOnly));
            const auto arguments = args.readAll().split('\n');
            QCOMPARE(arguments.contains("--hide-keyboard"), !keyboard);
            const auto at = arguments.indexOf("--jog-controls");
            QVERIFY(at >= 0 && at + 1 < arguments.size());
            QCOMPARE(QString::fromUtf8(arguments.at(at + 1)), frondSessions::jogControlsName(jog));
            QVERIFY(!arguments.contains("--hide-jog-controls"));
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
            QVERIFY(frondSessions::readPreferences(path, &prefs));
            QCOMPARE(prefs.showKeyboard, keyboard); QCOMPARE(prefs.jogControls, jog);
            QVERIFY(prefs.microphone);
            QCOMPARE(digest(path), guest);
        }
        QDir(managedFolder()).removeRecursively(); QSettings().clear();
    }
    void machineControlVisibilitySettings() {
        frondSessions::Preferences original{7, true, false, true, true, false};
        original.jogControls = frondSessions::JogControls::screen;
        MachineSettings controls(original, false);
        controls.setDevice(frondFirmware::deviceFromId("sony-clie-tg50"));
        QVERIFY(!controls.showKeyboard->isHidden());
        QVERIFY(!controls.jogControls->isHidden() && !controls.jogControlsLabel->isHidden());
        QCOMPARE(controls.jogControlsLabel->text(), QString("Jog controls"));
        QVERIFY(!controls.showKeyboard->isChecked());
        QCOMPARE(controls.jogControls->count(), 3);
        QCOMPARE(controls.jogControls->itemText(0), QString("Hidden"));
        QCOMPARE(controls.jogControls->itemText(1), QString("Left of screen"));
        QCOMPARE(controls.jogControls->itemText(2), QString("Left of keyboard"));
        QCOMPARE(controls.jogControls->currentText(), QString("Left of screen"));
        QCOMPARE(MachineSettings(frondSessions::Preferences{}, false).jogControls->currentText(), QString("Left of keyboard"));
        for (bool keyboard : {false, true})
        for (auto jog : {frondSessions::JogControls::hidden, frondSessions::JogControls::screen, frondSessions::JogControls::keyboard}) {
            controls.showKeyboard->setChecked(keyboard); controls.jogControls->setCurrentIndex(int(jog));
            const auto saved = controls.values();
            QCOMPARE(saved.showKeyboard, keyboard); QCOMPARE(saved.jogControls, jog);
            QCOMPARE(saved.scale, original.scale);
            QVERIFY(saved.smooth && !saved.audio && saved.fastAudio && saved.microphone);
        }
        controls.setDevice(frondFirmware::Device::tx);
        QVERIFY(controls.showKeyboard->isHidden());
        QVERIFY(controls.jogControls->isHidden() && controls.jogControlsLabel->isHidden());
        QVERIFY(controls.values().microphone);
    }
    void legacyTg50KeepsJogBesideScreen() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("TG", 6, "sony-clie-tg50", 0);
        const auto tg = managedSession("TG");
        QVERIFY(!QFileInfo::exists(frondSessions::preferencesPath(tg)));
        {
            MainWindow window(QString("/nonexistent")); window.show();
            frondSessions::Preferences prefs;
            QVERIFY(frondSessions::readPreferences(tg, &prefs));
            QVERIFY(prefs.jogControls == frondSessions::JogControls::screen);
        }
        // The default size follows the layout: beside the keyboard is the smallest TG50 layout with a keyboard.
        const auto tg50 = frondFirmware::deviceFromId("sony-clie-tg50");
        using Jog = frondSessions::JogControls;
        QVERIFY(fittingScaleQuarters(tg50, true, Jog::keyboard) >= fittingScaleQuarters(tg50, true, Jog::screen));
        QVERIFY(fittingScaleQuarters(tg50, true, Jog::hidden) >= fittingScaleQuarters(tg50, true, Jog::screen));
        QVERIFY(fittingScaleQuarters(tg50, false, Jog::screen) >= fittingScaleQuarters(tg50, true, Jog::screen));
    }
    void machinePreferences() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QSettings().setValue("windowScale", 6); QSettings().setValue("audio", false);
        QSettings().setValue("smoothLcd", true); QSettings().setValue("fastAudio", true);
        QDir(managedFolder()).removeRecursively();
        putSession("One", 6, "palm-tx", 0); putSession("Two", 6, "palm-tx", 1);
        const auto one = managedSession("One"), two = managedSession("Two");
        QTemporaryDir files;
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write("#!/bin/sh\ntouch \"$0.started\"\n"); core.close();
        QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto guest = digest(one); const auto when = QFileInfo(one).lastModified();
        {
            MainWindow window(core.fileName()); window.show();
            frondSessions::Preferences prefs;
            QVERIFY(frondSessions::readPreferences(one, &prefs));
            QCOMPARE(prefs.scale, 6); QVERIFY(prefs.smooth && !prefs.audio && prefs.fastAudio);
            const auto metadata = digest(frondSessions::preferencesPath(one));
            bool accept = false, shown = false;
            QTimer poll; poll.setInterval(20);
            connect(&poll, &QTimer::timeout, &window, [&] {
                auto* dialog = window.findChild<MachineSettingsDialog*>();
                if (!dialog || !dialog->isVisible()) return;
                shown = true;
                // Unqualified T|X firmware still gets the queue-only low-latency mode.
                QVERIFY(dialog->controls->fastAudio->isEnabled());
                QVERIFY(dialog->controls->fastAudio->toolTip().isEmpty());
                QVERIFY(dialog->controls->fastAudio->isChecked() || accept);
                QLockFile contender(one + ".lock"); contender.setStaleLockTime(0);
                QVERIFY(!contender.tryLock());
                dialog->controls->windowScale->setValue(8);
                dialog->controls->smoothLcd->setChecked(false);
                dialog->controls->audio->setChecked(true);
                dialog->controls->fastAudio->setChecked(false);
                if (accept) dialog->accept(); else dialog->reject();
            });
            poll.start(); rowAction(window, 0, "editSession");
            QTRY_VERIFY(shown); QCOMPARE(digest(frondSessions::preferencesPath(one)), metadata);
            accept = true; shown = false; rowAction(window, 0, "editSession"); QTRY_VERIFY(shown);
            QVERIFY(frondSessions::readPreferences(one, &prefs));
            QCOMPARE(prefs.scale, 8); QVERIFY(!prefs.smooth && prefs.audio && !prefs.fastAudio);
            QVERIFY(frondSessions::readPreferences(two, &prefs));
            QCOMPARE(prefs.scale, 6); QVERIFY(prefs.smooth && !prefs.audio && prefs.fastAudio);
            poll.stop();
            QCOMPARE(digest(one), guest); QCOMPARE(QFileInfo(one).lastModified(), when);
            QVERIFY(!QFileInfo::exists(core.fileName() + ".started"));
            QVERIFY(frondSessions::rename(managedFolder(), frondSessions::describe(one), "Renamed").isEmpty());
        }
        QSettings().setValue("windowScale", 4); QSettings().setValue("audio", true);
        {
            MainWindow next(core.fileName()); next.show();
            frondSessions::Preferences prefs;
            const auto renamed = managedSession("Renamed");
            QVERIFY(frondSessions::readPreferences(renamed, &prefs)); QCOMPARE(prefs.scale, 8);
            QVERIFY(frondSessions::readPreferences(two, &prefs)); QCOMPARE(prefs.scale, 6); QVERIFY(!prefs.audio);
            QLockFile lock(renamed + ".lock"); QVERIFY(lock.tryLock());
            QString error;
            QTimer poll; poll.setInterval(20);
            connect(&poll, &QTimer::timeout, &next, [&] {
                if (auto* box = next.findChild<QMessageBox*>(); box && box->isVisible()) { error = box->text(); box->accept(); }
            });
            poll.start(); rowAction(next, 0, "editSession");
            QTRY_VERIFY(error.contains("another Frond")); poll.stop();
            QVERIFY(!next.findChild<MachineSettingsDialog*>());
            QVERIFY(!QFileInfo::exists(core.fileName() + ".started"));
        }
        QDir(managedFolder()).removeRecursively(); QSettings().clear();
    }
    void sessionRenameDelete() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("Trip", 6, "palm-tx", 1);
        putSession("Work", 6, "palm-e2", 0);
        MainWindow window{QString()}; window.show();
        auto* list = window.findChild<QListWidget*>("sessionList");
        QString text, name;
        QTimer poll;
        poll.setInterval(30);
        QMessageBox::StandardButton answer = QMessageBox::Cancel;
        connect(&poll, &QTimer::timeout, &window, [&] {
            if (auto* input = window.findChild<QInputDialog*>(); input && input->isVisible()) {
                name = input->textValue(); input->setTextValue(text); input->accept();
            } else if (auto* box = window.findChild<QMessageBox*>(); box && box->isVisible()) {
                name = box->text();
                if (auto* button = box->button(answer)) button->click(); else box->done(answer);
            }
        });
        poll.start();
        // Rename (row 0 is Work, the newest).
        text = "Journal";
        rowAction(window, 0, "renameSession");
        QTRY_COMPARE(rowTexts(window, "sessionName"), QStringList({"Journal", "Trip"}));
        QCOMPARE(name, QString("Work"));
        QVERIFY(QFileInfo::exists(managedSession("Journal")) && !QFileInfo::exists(managedSession("Work")));
        QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(), managedSession("Journal"));
        // A name in use is refused and says so.
        text = "Trip"; name.clear();
        rowAction(window, 0, "renameSession");
        QTRY_VERIFY(name.contains("already a session called Trip"));
        QVERIFY(QFileInfo::exists(managedSession("Journal")));
        // Delete asks first.
        name.clear(); answer = QMessageBox::Cancel;
        rowAction(window, 1, "deleteSession");
        QTRY_COMPARE(name, QString("Delete Trip?"));
        QCOMPARE(list->count(), 2);
        QVERIFY(QFileInfo::exists(managedSession("Trip")));
        answer = QMessageBox::Yes;
        rowAction(window, 1, "deleteSession");
        QTRY_COMPARE(list->count(), 1);
        QVERIFY(!QFileInfo::exists(managedSession("Trip")));
        poll.stop();
        QDir(managedFolder()).removeRecursively();
        QSettings().clear();
    }
    // A real mouse click (routed by the window, so by childAt) on a row's "..." opens its menu.
    void rowMenuClick() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("Trip", 6, "palm-tx", 1);
        MainWindow window{QString()}; window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto* list = window.findChild<QListWidget*>("sessionList");
        auto* more = list->itemWidget(list->item(0))->findChild<QPushButton*>("rowMenu");
        QTest::mouseClick(window.windowHandle(), Qt::LeftButton, {}, more->mapTo(&window, more->rect().center()));
        QTRY_VERIFY(qobject_cast<QMenu*>(QApplication::activePopupWidget()));
        QCOMPARE(QApplication::activePopupWidget(), more->menu());
        QApplication::activePopupWidget()->close();
        QDir(managedFolder()).removeRecursively();
        QSettings().clear();
    }
    // Opening a row asks for missing firmware once, also when the dialog is cancelled.
    void rowActivateOnce() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/frond/firmware").removeRecursively();
        QVERIFY(frondSessions::create(managedFolder(), "Fresh", "palm-tx"));
        QTemporaryDir files;
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly)); core.write("#!/bin/sh\n"); core.close();
        QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        MainWindow window(core.fileName()); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        int shown = 0;
        QTimer poll;
        poll.setInterval(20);
        connect(&poll, &QTimer::timeout, &window, [&] {
            if (auto* firmware = window.findChild<FirmwareDialog*>(); firmware && firmware->isVisible()) { shown++; firmware->reject(); }
        });
        poll.start();
        auto* list = window.findChild<QListWidget*>("sessionList");
        const auto at = list->visualItemRect(list->item(0)).center();
        QTest::mouseDClick(window.windowHandle(), Qt::LeftButton, {}, list->viewport()->mapTo(&window, at));
        QTest::qWait(300);
        QCOMPARE(shown, 1);
        poll.stop();
        QDir(managedFolder()).removeRecursively();
        QSettings().clear();
    }
    // Firmware for another device leaves a message and starts nothing.
    void wrongFirmware() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        const auto firmwareRoot = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/frond/firmware";
        QDir(firmwareRoot).removeRecursively();
        QTemporaryDir files;
        const auto norPath = files.filePath("e2-nor");
        {
            QFile nor(norPath); QVERIFY(nor.open(QIODevice::WriteOnly)); QVERIFY(nor.resize(8 << 20));
            nor.seek(0x20008); nor.write("\xef\xbe\xed\xfe", 4);
            nor.seek(0x20080); nor.write("mlaP", 4); nor.write("rpsh", 4);
        }
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly)); core.write("#!/bin/sh\n"); core.close();
        QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        MainWindow window(core.fileName()); window.show();
        QString message;
        QTimer poll;
        poll.setInterval(30);
        connect(&poll, &QTimer::timeout, &window, [&] {
            if (auto* box = window.findChild<QMessageBox*>(); box && box->isVisible()) { message = box->text(); box->accept(); }
            else if (auto* firmware = window.findChild<FirmwareDialog*>(); firmware && firmware->isVisible()) {
                if (!message.isEmpty()) { firmware->reject(); return; }
                auto* devices = firmware->findChild<QComboBox*>("device");
                devices->setCurrentIndex(devices->findData("palm-tx"));
                firmware->findChild<QLineEdit*>("nor")->setText(norPath);
                QTimer::singleShot(0, firmware, [firmware] {
                    firmware->findChild<QPushButton*>("import")->click();
                });
            }
        });
        poll.start();
        window.findChild<QPushButton*>("addDevice")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!message.isEmpty(), 5000);
        poll.stop();
        QVERIFY(message.contains("Tungsten E2") && message.contains("Choose Tungsten E2"));
        QCOMPARE(window.findChild<QListWidget*>("sessionList")->count(), 0);
        QVERIFY(QDir(managedFolder()).entryList(QDir::Files).isEmpty());
        QDir(firmwareRoot).removeRecursively();
        QSettings().clear();
    }
    // Renaming keeps resume pointing at the session; deleting a locked one leaves it alone.
    void sessionResumeSetting() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("Trip", 6, "palm-tx", 1);
        putSession("Work", 6, "palm-e2", 0);
        const auto canonical = [](const QString& name) {
            return QFileInfo(managedSession(name)).canonicalFilePath();
        };
        QSettings().setValue("lastSession", canonical("Work"));
        MainWindow window{QString()}; window.show();
        QString text = "Journal";
        QMessageBox::StandardButton answer = QMessageBox::Yes;
        QTimer poll;
        poll.setInterval(30);
        connect(&poll, &QTimer::timeout, &window, [&] {
            if (auto* input = window.findChild<QInputDialog*>(); input && input->isVisible()) { input->setTextValue(text); input->accept(); }
            else if (auto* box = window.findChild<QMessageBox*>(); box && box->isVisible()) box->button(answer) ? box->button(answer)->click() : box->done(answer);
        });
        poll.start();
        rowAction(window, 0, "renameSession");
        QTRY_COMPARE(rowTexts(window, "sessionName"), QStringList({"Journal", "Trip"}));
        QCOMPARE(QSettings().value("lastSession").toString(), canonical("Journal"));
        // The session is open in another Frond: Delete says no and keeps the setting.
        QLockFile lock(managedSession("Journal") + ".lock"); QVERIFY(lock.tryLock());
        answer = QMessageBox::Yes;
        rowAction(window, 0, "deleteSession");
        QTest::qWait(300);
        QVERIFY(QFileInfo::exists(managedSession("Journal")));
        QCOMPARE(QSettings().value("lastSession").toString(), canonical("Journal"));
        lock.unlock();
        rowAction(window, 0, "deleteSession");
        QTRY_COMPARE(window.findChild<QListWidget*>("sessionList")->count(), 1);
        QVERIFY(!QSettings().contains("lastSession"));
        poll.stop();
        QDir(managedFolder()).removeRecursively();
        QSettings().clear();
    }
    // A session opened from outside the folder takes its name at once and runs from that folder.
    void openOutsideReservesName() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        QTemporaryDir files;
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly)); core.write("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$0.args\"\nsleep 5\n"); core.close();
        QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QFile source(files.filePath("Outside.frond")); QVERIFY(source.open(QIODevice::WriteOnly));
        source.write(sessionBytes(6, "palm-tx")); source.close();
        const auto before = QDir(files.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
        MainWindow window(core.fileName()); window.show();
        window.openSession(source.fileName(), true);
        QVERIFY(QFileInfo::exists(frondSessions::markerPath(managedSession("Palm TX"))));
        QCOMPARE(frondSessions::freeName(managedFolder(), "Palm TX"), QString("Palm TX 2"));
        // The core runs from the new folder, and nothing is written beside the original.
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(core.fileName() + ".args"), 3000);
        QFile args(core.fileName() + ".args"); QVERIFY(args.open(QIODevice::ReadOnly));
        QVERIFY(QString::fromUtf8(args.readAll()).contains("--work-folder\n" + QFileInfo(frondSessions::machineDirOf(managedSession("Palm TX"))).canonicalFilePath()));
        auto expected = before + QStringList{"core.args"};
        expected.sort();
        QCOMPARE(QDir(files.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name), expected);
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 8000);
        QDir(managedFolder()).removeRecursively();
        QSettings().clear();
    }
    void sessionImport() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        QTemporaryDir files;
        const auto put = [&](const QString& name, const QByteArray& data) -> QString {
            QFile f(files.filePath(name)); if (!f.open(QIODevice::WriteOnly)) return {}; f.write(data); return f.fileName();
        };
        const auto good = put("Holiday.frond", sessionBytes(6, "palm-e2"));
        MainWindow window{QString()}; window.show();
        QVERIFY(window.importSession(good).isEmpty());
        QVERIFY(window.importSession(good).isEmpty());
        { const auto names = rowTexts(window, "sessionName"); QCOMPARE(QSet<QString>(names.begin(), names.end()), QSet<QString>({"Holiday", "Holiday 2"})); }
        // The copy is in the managed folder and the original stays where it was.
        QVERIFY(QFileInfo::exists(managedSession("Holiday")) && QFileInfo::exists(good));
        const auto packed = files.filePath("Packed");
        QDir().mkpath(packed);
        QFile packedFrond(packed + "/Packed.frond"); QVERIFY(packedFrond.open(QIODevice::WriteOnly));
        packedFrond.write(sessionBytes(6, "palm-tx")); packedFrond.close();
        QFile packedCard(packed + "/sd.frondsd"); QVERIFY(packedCard.open(QIODevice::WriteOnly));
        packedCard.write("card"); packedCard.close();
        QFile packedTemp(packed + "/.frond.tmp-x"); QVERIFY(packedTemp.open(QIODevice::WriteOnly));
        packedTemp.write("tmp"); packedTemp.close();
        QVERIFY(window.importSession(packed).isEmpty());
        QVERIFY(QFileInfo::exists(managedSession("Packed")));
        QVERIFY(QFileInfo::exists(frondSessions::machineDir(managedFolder(), "Packed") + "/sd.frondsd"));
        QVERIFY(!QFileInfo::exists(frondSessions::machineDir(managedFolder(), "Packed") + "/.frond.tmp-x"));
        QCOMPARE(window.importSession(put("text.frond", "not a session")), QString("That isn't a Frond session."));
        QVERIFY(window.importSession(put("old.frond", sessionBytes(5, "palm-tx"))).contains("older Frond"));
        QCOMPARE(window.findChild<QListWidget*>("sessionList")->count(), 3);
        QDir(managedFolder()).removeRecursively();
        QSettings().clear();
    }
    void unsupportedSoundKeepsMachinePreference() {
        frondSessions::Preferences prefs;
        prefs.audio = true;
        prefs.fastAudio = true;
        MachineSettings widget(prefs, false);
        widget.setDevice(frondFirmware::Device::automatic);
        QVERIFY(!widget.audio->isEnabled());
        QVERIFY(!widget.fastAudio->isEnabled());
        QVERIFY(widget.values().audio && widget.values().fastAudio);
        for (const auto& profile : deviceProfiles5) {
            if (profile.deviceType == deviceTypeInvalid || profile.capabilities.sound) continue;
            widget.setDevice(static_cast<frondFirmware::Device>(profile.deviceType));
            QVERIFY(!widget.audio->isEnabled());
            QVERIFY(widget.values().audio);
        }
        widget.setDevice(frondFirmware::Device::tx);
        QVERIFY(widget.audio->isEnabled());
        QVERIFY(widget.audio->isChecked());
        QVERIFY(widget.fastAudio->isEnabled());
        widget.setDevice(frondFirmware::deviceFromId("sony-clie-tg50"));
        QVERIFY(widget.fastAudio->isEnabled() && widget.fastAudio->toolTip().isEmpty());
    }
    void settingsDialog() {
        QSettings().clear(); QSettings().setValue("resume", false);
        MainWindow window{QString()}; window.show();
        auto* dialog = window.findChild<SettingsDialog*>(); QVERIFY(dialog);
        QVERIFY(dialog->resume && !dialog->resume->isChecked());
        QVERIFY(!dialog->findChild<QSlider*>("windowScale"));
        QVERIFY(!dialog->findChild<QCheckBox*>("audio"));
        dialog->resume->setChecked(true);
        QVERIFY(QSettings().value("resume").toBool());
        QSettings().clear();
    }
    // Optional actual frontend run; private firmware/session and captures stay outside the repo.
    void linkPairing() {
        QTemporaryDir dir(QDir::currentPath() + "/build/lk-XXXXXX");
        TestHub ha(dir.path(), "a", freePort()), hb(dir.path(), "b", freePort()), hc(dir.path(), "c", freePort());
        HubUser a(ha), b(hb), c(hc);
        a.client.send({{"op", "rename"}, {"name", "Ay"}});
        b.client.send({{"op", "rename"}, {"name", "Bee"}});
        c.client.send({{"op", "rename"}, {"name", "Cee"}});
        QTRY_VERIFY_WITH_TIMEOUT(!a.last("hello").isEmpty() && !c.last("hello").isEmpty(), 10000);
        // Not watching: B refuses to pair.
        a.client.send({{"op", "pair"}, {"address", "127.0.0.1:" + hb.env.value("FROND_LINK_PORT")}});
        QTRY_COMPARE_WITH_TIMEOUT(a.last("pair-result").value("result").toString(), QString("notpairing"), 10000);
        a.client.send({{"op", "pair-cancel"}});
        QVERIFY(pairHubs(a, b, true));
        QCOMPARE(a.last("pair-peer").value("name").toString(), QString("Bee"));
        QCOMPARE(b.last("pair-request").value("name").toString(), QString("Ay"));
        const auto aId = a.client.hello().value("id").toString(), bId = b.client.hello().value("id").toString();
        QCOMPARE(storedPeer(ha, bId).value("name").toString(), QString("Bee"));
        QCOMPARE(storedPeer(hb, aId).value("name").toString(), QString("Ay"));
        QCOMPARE(storedPeer(ha, bId).value("key").toString().size(), 64);
        for (const auto& file : {ha.store() + "/identity.json", ha.store() + "/connections.json", hb.store() + "/connections.json"})
            QCOMPARE(QFileInfo(file).permissions() & ~(QFile::ReadUser | QFile::WriteUser), QFile::ReadOwner | QFile::WriteOwner);
        QTRY_VERIFY_WITH_TIMEOUT(b.last("state").value("connections").toArray().size() == 1, 5000);
        QCOMPARE(ConnectionsWindow::status(b.last("state").value("connections").toArray()[0].toObject()), QString("Not seen"));
        // Lockout: three wrong codes, then B refuses for a while.
        b.got.clear();
        c.client.send({{"op", "pair"}, {"address", "127.0.0.1:" + hb.env.value("FROND_LINK_PORT")}});
        QTRY_VERIFY_WITH_TIMEOUT(!b.last("pair-request").isEmpty(), 10000);
        const auto code = b.last("pair-request").value("code").toString().remove(' ');
        const auto wrong = QString("%1").arg((code.toInt() + 7) % 1000000, 6, 10, QChar('0'));
        for (int i = 0; i < 3; ++i) {
            c.got.clear();
            c.client.send({{"op", "pair-code"}, {"code", wrong}});
            QTRY_COMPARE_WITH_TIMEOUT(c.last("pair-result").value("result").toString(), QString("wrongcode"), 10000);
        }
        QTRY_COMPARE_WITH_TIMEOUT(b.last("pair-request-end").value("result").toString(), QString("lockedout"), 5000);
        c.got.clear();
        c.client.send({{"op", "pair-code"}, {"code", code}});
        QTRY_COMPARE_WITH_TIMEOUT(c.last("pair-result").value("result").toString(), QString("notpairing"), 10000);
        QVERIFY(storedPeer(hc, bId).isEmpty() && storedPeer(hb, c.client.hello().value("id").toString()).isEmpty());
        for (auto* user : {&a, &b, &c}) killHub(*user);
    }
    void linkInfrared() {
        QTemporaryDir dir(QDir::currentPath() + "/build/lk-XXXXXX");
        TestHub ha(dir.path(), "a", freePort()), hb(dir.path(), "b", freePort());
        HubUser a(ha), b(hb);
        QVERIFY(pairHubs(a, b, false));
        const auto aId = a.client.hello().value("id").toString(), bId = b.client.hello().value("id").toString();
        FakeIr coreA1(ha.run + "/a1.sock"), coreA2(ha.run + "/a2.sock"), coreB1(hb.run + "/b1.sock");
        auto attach = [](HubUser& u, const QString& machine, const QString& link, const FakeIr& core) {
            u.client.setSticky("ir", {{"op", "ir-attach"}, {"machine", machine}, {"device", "Palm TX"}, {"link", link}, {"socket", core.path}});
        };
        // One side only: the link comes up and the channel waits.
        HubUser a1(ha), b1(hb), a2(ha);
        attach(a1, "Palm A1", bId, coreA1);
        QTRY_COMPARE_WITH_TIMEOUT(a1.ir(), QString("waiting"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(coreA1.peer, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(b.last("state").value("connections").toArray()[0].toObject().value("up").toBool(), 5000);
        // The other side joins it.
        attach(b1, "Palm B1", aId, coreB1);
        QTRY_COMPARE_WITH_TIMEOUT(a1.ir(), QString("joined"), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(b1.ir(), QString("joined"), 10000);
        QCOMPARE(a1.last("ir").value("machine").toString(), QString("Palm B1"));
        QCOMPARE(b1.last("ir").value("peer").toString(), QString(storedPeer(hb, aId).value("name").toString()));
        QTRY_VERIFY_WITH_TIMEOUT(coreB1.peer, 5000);
        coreA1.send("beam to B");
        QTRY_COMPARE_WITH_TIMEOUT(coreB1.got, QByteArray("beam to B"), 5000);
        coreB1.send("beam to A");
        QTRY_COMPARE_WITH_TIMEOUT(coreA1.got, QByteArray("beam to A"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(b.last("state").value("connections").toArray()[0].toObject().value("channels").toString()
                                 .contains("Palm B1 – Palm A1"), 5000);
        // A second machine on A for the same connection is busy.
        attach(a2, "Palm A2", bId, coreA2);
        QTRY_COMPARE_WITH_TIMEOUT(a2.ir(), QString("busy"), 10000);
        QCOMPARE(a2.last("ir").value("busyBy").toString(), QString("Palm A1"));
        a2.client.setSticky("ir", {});
        // B's Palm sleeps: its socket closes, A sees it asleep, and it reopens on wake.
        coreB1.sleep();
        QTRY_COMPARE_WITH_TIMEOUT(a1.last("ir").value("far").toString(), QString("asleep"), 5000);
        QVERIFY(coreB1.listen());
        QTRY_COMPARE_WITH_TIMEOUT(a1.last("ir").value("far").toString(), QString("running"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(coreB1.peer, 5000);
        coreB1.got.clear();
        coreA1.send("after wake");
        QTRY_COMPARE_WITH_TIMEOUT(coreB1.got, QByteArray("after wake"), 5000);
        // B's hub crashes: B's window starts a new one, the link comes back and the channel reopens.
        const auto oldPid = b1.last("hello").value("pid").toDouble();
        killHub(b1);
        QTRY_VERIFY_WITH_TIMEOUT(a1.ir() != "joined", 5000);
        QTRY_VERIFY_WITH_TIMEOUT(b1.last("hello").value("pid").toDouble() != oldPid, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(a1.ir(), QString("joined"), 20000);
        QTRY_COMPARE_WITH_TIMEOUT(b1.ir(), QString("joined"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(coreB1.peer && coreB1.peer->state() == QLocalSocket::ConnectedState, 5000);
        coreB1.got.clear();
        coreA1.send("after restart");
        QTRY_COMPARE_WITH_TIMEOUT(coreB1.got, QByteArray("after restart"), 5000);
        // A pinned key that no longer matches: A shows it and keeps trying with backoff, storing
        // nothing, as a refusal can come from anyone. B doesn't dial A meanwhile, so it only sees
        // the failed handshake as the side dialled.
        b1.client.setSticky("ir", {});
        a1.client.setSticky("ir", {});
        a.client.setSticky("watch", {});
        QTRY_VERIFY_WITH_TIMEOUT(!a.client.isConnected() && !a1.client.isConnected() && !a2.client.isConnected() &&
                                 !b1.client.isConnected(), 5000);
        killHub(a);
        QTest::qWait(300);
        const auto keyOnB = storedPeer(hb, aId).value("key").toString();
        QTRY_VERIFY_WITH_TIMEOUT(!listed(b, "connections", aId).value("up").toBool(), 10000);
        QVERIFY(!listed(b, "connections", aId).value("keyChanged").toBool());
        {
            QFile f(ha.store() + "/connections.json"); QVERIFY(f.open(QIODevice::ReadOnly));
            auto doc = QJsonDocument::fromJson(f.readAll()).object();
            auto entries = doc.value("entries").toArray();
            auto entry = entries[0].toObject();
            entry["key"] = QString(64, 'a');
            entries[0] = entry; doc["entries"] = entries;
            f.close(); QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); f.write(QJsonDocument(doc).toJson());
        }
        a1.got.clear();
        attach(a1, "Palm A1", bId, coreA1);
        QTRY_COMPARE_WITH_TIMEOUT(a1.ir(), QString("keychanged"), 15000);
        QVERIFY(!storedPeer(ha, bId).value("keyChanged").toBool());
        // B was dialled with the failed handshake: it shows the change too, but keeps A's key.
        QTRY_VERIFY_WITH_TIMEOUT(listed(b, "connections", aId).value("keyChanged").toBool(), 5000);
        QCOMPARE(ConnectionsWindow::status(listed(b, "connections", aId)), QString("Key changed, pair again"));
        QVERIFY(!storedPeer(hb, aId).value("keyChanged").toBool());
        QCOMPARE(storedPeer(hb, aId).value("key").toString(), keyOnB);
        // Pairing again fixes it on both sides.
        QVERIFY(pairHubs(a, b, false));
        attach(b1, "Palm B1", aId, coreB1);
        QTRY_COMPARE_WITH_TIMEOUT(a1.ir(), QString("joined"), 20000);
        QVERIFY(!storedPeer(ha, bId).value("keyChanged").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!listed(b, "connections", aId).value("keyChanged").toBool(), 5000);
        // Someone else sends a first message with A's id while A's link is up: B ignores it.
        {
            frond::link::LinkConfig cfg;
            cfg.local.identity = frond::link::makeIdentity();
            QVERIFY(frondLink::parseId(aId, cfg.local.id));
            cfg.local.name = "spoof";
            frond::link::InstallId to{};
            QVERIFY(frondLink::parseId(bId, to));
            frond::link::Key32 wrong{};
            wrong.fill(7);
            frond::link::Link spoof(cfg, to, wrong, 0);
            spoof.start();
            const auto first = spoof.takeOutput();
            QTcpSocket s; s.connectToHost("127.0.0.1", quint16(b.client.hello().value("port").toInt()));
            QVERIFY(s.waitForConnected(3000));
            s.write(reinterpret_cast<const char*>(first.data()), qint64(first.size()));
            QVERIFY(s.waitForBytesWritten(3000));
            QTRY_VERIFY_WITH_TIMEOUT(s.state() == QAbstractSocket::UnconnectedState, 5000);
            QTest::qWait(500);
            QVERIFY(!listed(b, "connections", aId).value("keyChanged").toBool());
            QCOMPARE(a1.ir(), QString("joined"));
        }
        // B forgets A: A is told to pair again.
        b.client.send({{"op", "forget"}, {"id", aId}});
        QTRY_COMPARE_WITH_TIMEOUT(a1.ir(), QString("forgotten"), 20000);
        QVERIFY(storedPeer(hb, aId).isEmpty());
        for (auto* user : {&a, &b}) killHub(*user);
    }
    void linkThisComputer() {
        QTemporaryDir dir(QDir::currentPath() + "/build/lk-XXXXXX");
        TestHub h(dir.path(), "h", freePort());
        HubUser l1(h), l2(h), l3(h);
        FakeIr c1(h.run + "/l1.sock"), c2(h.run + "/l2.sock"), c3(h.run + "/l3.sock");
        auto attach = [](HubUser& u, const QString& machine, const FakeIr& core) {
            u.client.setSticky("ir", {{"op", "ir-attach"}, {"machine", machine}, {"device", "Tungsten E2"}, {"link", "local"}, {"socket", core.path}});
        };
        attach(l1, "Palm L1", c1);
        QTRY_COMPARE_WITH_TIMEOUT(l1.ir(), QString("waiting"), 10000);
        QCOMPARE(l1.last("ir").value("peer").toString(), QString("This computer"));
        attach(l2, "Palm L2", c2);
        QTRY_COMPARE_WITH_TIMEOUT(l1.ir(), QString("joined"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(l2.ir(), QString("joined"), 5000);
        QCOMPARE(l2.last("ir").value("machine").toString(), QString("Palm L1"));
        QTRY_VERIFY_WITH_TIMEOUT(c1.peer && c2.peer, 5000);
        c1.send("one"); QTRY_COMPARE_WITH_TIMEOUT(c2.got, QByteArray("one"), 5000);
        c2.send("two"); QTRY_COMPARE_WITH_TIMEOUT(c1.got, QByteArray("two"), 5000);
        attach(l3, "Palm L3", c3);
        QTRY_COMPARE_WITH_TIMEOUT(l3.ir(), QString("busy"), 5000);
        QCOMPARE(l3.last("ir").value("busyBy").toString(), QString("Palm L1"));
        c2.sleep();
        QTRY_COMPARE_WITH_TIMEOUT(l1.last("ir").value("far").toString(), QString("asleep"), 5000);
        QVERIFY(c2.listen());
        QTRY_COMPARE_WITH_TIMEOUT(l1.last("ir").value("far").toString(), QString("running"), 5000);
        // L2 stops: L3 takes its place.
        l2.client.setSticky("ir", {});
        l2.client.send({{"op", "ir-detach"}});
        QTRY_COMPARE_WITH_TIMEOUT(l3.ir(), QString("joined"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(c3.peer, 5000);
        c3.send("three"); QTRY_VERIFY_WITH_TIMEOUT(c1.got.endsWith("three"), 5000);
        killHub(l1);
    }
    // Serial over a connection: the port opens only once joined, nothing is lost when the far
    // Palm holds RTS low (the link stops and the near core's socket fills), and a link that drops
    // closes both ports.
    void linkSerial() {
        QTemporaryDir dir(QDir::currentPath() + "/build/lk-XXXXXX");
        TestHub ha(dir.path(), "a", freePort()), hb(dir.path(), "b", freePort());
        HubUser a(ha), b(hb);
        QVERIFY(pairHubs(a, b, false));
        const auto aId = a.client.hello().value("id").toString(), bId = b.client.hello().value("id").toString();
        FakeSerial coreA(ha.run + "/sa.sock"), coreB(hb.run + "/sb.sock"), coreA2(ha.run + "/sa2.sock");
        FakeIr irA(ha.run + "/ia.sock"), irB(hb.run + "/ib.sock");
        auto attach = [](HubUser& u, const QString& machine, const QString& link, const QString& path) {
            u.client.setSticky("serial", {{"op", "serial-attach"}, {"machine", machine}, {"device", "Palm TX"}, {"link", link}, {"socket", path}});
        };
        auto a1 = std::make_unique<HubUser>(ha), b1 = std::make_unique<HubUser>(hb);
        HubUser a2(ha);
        attach(*a1, "Palm A1", bId, coreA.path);
        QTRY_COMPARE_WITH_TIMEOUT(a1->serial(), QString("waiting"), 10000);
        QTest::qWait(600);
        QCOMPARE(coreA.opens, 0);
        // Infrared on the same link as serial, alongside it.
        a1->client.setSticky("ir", {{"op", "ir-attach"}, {"machine", "Palm A1"}, {"device", "Palm TX"}, {"link", bId}, {"socket", irA.path}});
        b1->client.setSticky("ir", {{"op", "ir-attach"}, {"machine", "Palm B1"}, {"device", "Palm TX"}, {"link", aId}, {"socket", irB.path}});
        attach(*b1, "Palm B1", aId, coreB.path);
        QTRY_COMPARE_WITH_TIMEOUT(a1->serial(), QString("joined"), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(b1->serial(), QString("joined"), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(a1->ir(), QString("joined"), 10000);
        QCOMPARE(a1->last("serial").value("machine").toString(), QString("Palm B1"));
        QTRY_VERIFY_WITH_TIMEOUT(coreA.open() && coreB.open(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(b.last("state").value("connections").toArray()[0].toObject().value("channels").toString()
                                 .contains("Serial: Palm B1 – Palm A1"), 5000);
        coreA.send("to B"); QTRY_COMPARE_WITH_TIMEOUT(coreB.got, QByteArray("to B"), 5000);
        coreB.send("to A"); QTRY_COMPARE_WITH_TIMEOUT(coreA.got, QByteArray("to A"), 5000);
        coreA.got.clear(); coreB.got.clear();
        // B's Palm holds RTS low: A's megabyte stops in A's core socket and none is lost.
        coreB.setRts(false);
        const auto big = pattern(1 << 20, 3);
        coreA.send(big);
        QTest::qWait(1500);
        const auto stuck = coreA.unsent();
        QVERIFY2(stuck > 512 * 1024, qPrintable(QString::number(stuck)));
        QTest::qWait(500);
        QCOMPARE(coreA.unsent(), stuck);
        QVERIFY(coreB.got.isEmpty());
        // The other way, and infrared, still go.
        coreB.send("reply"); QTRY_COMPARE_WITH_TIMEOUT(coreA.got, QByteArray("reply"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(irA.peer && irB.peer, 5000);
        irA.send("beam"); QTRY_COMPARE_WITH_TIMEOUT(irB.got, QByteArray("beam"), 5000);
        coreB.setRts(true);
        QTRY_COMPARE_WITH_TIMEOUT(coreB.got.size(), big.size(), 30000);
        QVERIFY(coreB.got == big);
        // A second machine on A for serial is busy.
        attach(a2, "Palm A2", bId, coreA2.path);
        QTRY_COMPARE_WITH_TIMEOUT(a2.serial(), QString("busy"), 10000);
        QCOMPARE(a2.last("serial").value("busyBy").toString(), QString("Palm A1"));
        QCOMPARE(coreA2.opens, 0);
        a2.client.setSticky("serial", {});
        // B's hub dies mid-transfer: both ports close, the link comes back and they reopen.
        coreB.setRts(false);
        coreA.send(pattern(64 * 1024, 5));
        QTest::qWait(300);
        const int opensA = coreA.opens;
        killHub(*b1);
        QTRY_VERIFY_WITH_TIMEOUT(!coreA.open(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(a1->serial() != "joined", 5000);
        QTRY_COMPARE_WITH_TIMEOUT(a1->serial(), QString("joined"), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(coreA.open() && coreA.opens > opensA && coreB.open(), 10000);
        coreB.got.clear();
        coreB.setRts(true);
        coreB.got.clear();
        coreA.send("after reconnect");
        QTRY_COMPARE_WITH_TIMEOUT(coreB.got, QByteArray("after reconnect"), 5000);
        // B stops: A's port closes and it waits.
        b1->client.setSticky("serial", {});
        QTRY_VERIFY_WITH_TIMEOUT(!coreA.open(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(a1->serial(), QString("waiting"), 5000);
        a1->client.setSticky("serial", {}); a1->client.setSticky("ir", {}); b1->client.setSticky("ir", {});
        for (auto* user : {&a, &b}) killHub(*user);
    }
    // Bluetooth over a connection: every controller goes to the side whose radio it is, several
    // per link, lossless through the 64 KB window, and moving the radio moves the controllers.
    void linkBluetooth() {
        QTemporaryDir dir(QDir::currentPath() + "/build/lk-XXXXXX");
        TestHub ha(dir.path(), "a", freePort()), hb(dir.path(), "b", freePort());
        HubUser a(ha), b(hb);
        QVERIFY(pairHubs(a, b, false));
        const auto aId = a.client.hello().value("id").toString(), bId = b.client.hello().value("id").toString();
        a.client.setSticky("watch", {{"op", "watch"}});
        // A puts Bluetooth on B's radio.
        a.client.send({{"op", "peer-radio"}, {"id", bId}, {"radio", "there"}});
        QTRY_COMPARE_WITH_TIMEOUT(storedPeer(ha, bId).value("bluetoothRadio").toString(), QString("there"), 5000);
        const auto chosen = storedPeer(ha, bId).value("bluetoothRadioAt").toDouble();
        QVERIFY(chosen > 0);
        HubUser a1(ha), a2(ha), b1(hb);
        auto attach = [](HubUser& u, const QString& machine, const QString& link, const QString& path) {
            u.client.setSticky("bt", {{"op", "bt-attach"}, {"machine", machine}, {"device", "Palm TX"}, {"link", link}, {"socket", path}});
        };
        FakeBt coreA(ha.run + "/frond/bt-a1.sock"), coreA2(ha.run + "/frond/bt-a2.sock"), coreB(hb.run + "/frond/bt-b1.sock");
        attach(a1, "Palm A1", bId, coreA.path);
        QTRY_VERIFY_WITH_TIMEOUT(a1.last("bt").value("listening").toBool(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(a1.bt(), QString("there"), 15000);
        // B learns the choice once linked.
        QTRY_COMPARE_WITH_TIMEOUT(storedPeer(hb, aId).value("bluetoothRadio").toString(), QString("here"), 5000);
        QCOMPARE(storedPeer(hb, aId).value("bluetoothRadioAt").toDouble(), chosen);
        attach(b1, "Palm B1", aId, coreB.path);
        QTRY_COMPARE_WITH_TIMEOUT(b1.bt(), QString("here"), 10000);
        QVERIFY(coreA.connect() && coreB.connect());
        QTRY_COMPARE_WITH_TIMEOUT(radioOpened(hb.run), 2, 10000);
        QCOMPARE(radioOpened(ha.run), 0);
        coreA.send("hci from A"); QTRY_COMPARE_WITH_TIMEOUT(coreB.got, QByteArray("hci from A"), 5000);
        coreB.send("hci from B"); QTRY_COMPARE_WITH_TIMEOUT(coreA.got, QByteArray("hci from B"), 5000);
        // A second machine on A: another controller on the same link.
        attach(a2, "Palm A2", bId, coreA2.path);
        QTRY_VERIFY_WITH_TIMEOUT(a2.last("bt").value("listening").toBool(), 10000);
        QVERIFY(coreA2.connect());
        QTRY_COMPARE_WITH_TIMEOUT(radioOpened(hb.run), 3, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(a1.last("bt").value("controllers").toInt(), 1, 5000);
        coreA.got.clear(); coreA2.got.clear();
        coreB.send("to both"); QTRY_VERIFY_WITH_TIMEOUT(coreA.got == "to both" && coreA2.got == "to both", 5000);
        QTRY_VERIFY_WITH_TIMEOUT(listed(b, "connections", aId).value("channels").toString().contains("Bluetooth: Palm B1, Palm A1, Palm A2") ||
                                 listed(b, "connections", aId).value("channels").toString().contains("Bluetooth: Palm B1, Palm A2, Palm A1"), 5000);
        // A megabyte from A arrives whole on B's radio, past the 64 KB window.
        coreB.got.clear(); coreA2.got.clear();
        const auto big = pattern(1 << 20, 9);
        coreA.send(big);
        QTRY_COMPARE_WITH_TIMEOUT(coreB.got.size(), big.size(), 30000);
        QVERIFY(coreB.got == big);
        QTRY_COMPARE_WITH_TIMEOUT(coreA2.got.size(), big.size(), 30000);
        // B moves the radio to A: every controller goes and the cores reconnect, now on A's radio.
        b.client.send({{"op", "peer-radio"}, {"id", aId}, {"radio", "there"}});
        QTRY_COMPARE_WITH_TIMEOUT(storedPeer(ha, bId).value("bluetoothRadio").toString(), QString("here"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(coreA.closed && coreA2.closed && coreB.closed, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(a1.bt(), QString("here"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(b1.bt(), QString("there"), 5000);
        QVERIFY(coreA.connect() && coreB.connect());
        QTRY_COMPARE_WITH_TIMEOUT(radioOpened(ha.run), 2, 10000);
        coreB.send("again from B"); QTRY_COMPARE_WITH_TIMEOUT(coreA.got, QByteArray("again from B"), 5000);
        coreA.send("again from A"); QTRY_COMPARE_WITH_TIMEOUT(coreB.got, QByteArray("again from A"), 5000);
        // B's hub dies: A drops B's controller, B's window starts a new hub and the core reconnects.
        killHub(b1);
        QTRY_VERIFY_WITH_TIMEOUT(coreB.closed, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(b1.last("hello").value("pid").toDouble() != 0 && b1.client.isConnected(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(b1.bt(), QString("there"), 20000);
        QVERIFY(coreB.connectWithin(5000));
        coreA.got.clear();
        QTRY_COMPARE_WITH_TIMEOUT(radioOpened(ha.run), 3, 10000);
        coreB.send("after restart"); QTRY_COMPARE_WITH_TIMEOUT(coreA.got, QByteArray("after restart"), 5000);
        for (auto* u : {&a1, &a2, &b1}) u->client.setSticky("bt", {});
        a.client.setSticky("watch", {});
        for (auto* user : {&a, &b, &b1}) killHub(*user);
    }
    // The running page's Bluetooth picker: This Frond goes through the hub, the core is pointed at
    // the hub's socket once it listens, and machine settings have the same choices.
    void bluetoothConnection() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        QFile source(files.filePath("input.frond")); QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("session"); source.close();
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,sys
base=pathlib.Path(sys.argv[1]);pathlib.Path(str(base)+'.args').write_text('\n'.join(sys.argv[1:]))
modes=pathlib.Path(str(base)+'.modes');modes.write_text('')
s=socket.socket(socket.AF_UNIX);s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t');arg=bytes.fromhex(arg.decode())
    message={b'device-state':b'palm-tx',b'sd-state':b'none',b'network-state':b'disabled',b'keyboard-state':b'detached'}.get(cmd,b'done')
    if cmd==b'bluetooth-mode':
        with modes.open('a') as m: m.write(arg.decode()+'\n')
        if arg.endswith(b'missing'):
            s.sendall(i+b'\terror\tbluetooth\t'+b'Controller socket unavailable'.hex().encode()+b'\n');continue
        message=b'off' if arg==b'off' else arg+b' connected'
    if cmd==b'save':pathlib.Path(arg.decode()).write_text('saved')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit':break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        QString machine;
        const auto file = [&](const char* suffix) {
            QFile f((machine.isEmpty() ? source.fileName() : machine) + suffix);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        };
        const auto hubPath = frondLink::coreSocketPath("bt", QString::number(QCoreApplication::applicationPid()));
        QString shown; QTimer dismiss; dismiss.setInterval(20);
        {
            MainWindow window(core.fileName()); window.show(); window.openSession(source.fileName(), true);
            auto* mode = window.findChild<QComboBox*>("bluetoothMode"); auto* state = window.findChild<QLabel*>("bluetoothState");
            auto* path = window.findChild<QLineEdit*>("bluetoothSocket");
            connect(&dismiss, &QTimer::timeout, &window, [&] {
                if (auto* box = window.findChild<QMessageBox*>(); box && box->isVisible()) { shown = box->text(); box->done(QMessageBox::Ok); }
            }); dismiss.start();
            const auto pick = [mode](const char* value) {
                mode->setCurrentIndex(mode->findData(value));
                QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, mode->currentIndex()));
            };
            QTRY_VERIFY_WITH_TIMEOUT(mode->isEnabled(), 5000);
            QCOMPARE(mode->itemText(mode->findData("radio")), QString("This Frond"));
            QVERIFY(mode->findData("link:local") < 0);
            pick("radio");
            QTRY_VERIFY_WITH_TIMEOUT(file(".modes").contains(hubPath.toUtf8() + "\n"), 15000);
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Bluetooth: This Frond's radio"), 5000);
            QVERIFY(!path->isVisible());
            QTRY_COMPARE_WITH_TIMEOUT(machineSetting("bluetooth"), QString("radio"), 5000);
            // The hub's socket gives the core controllers on This Frond's radio.
            const int before = radioOpened(qEnvironmentVariable("XDG_RUNTIME_DIR"));
            FakeBt controller(hubPath);
            QVERIFY(controller.connect());
            QTRY_COMPARE_WITH_TIMEOUT(radioOpened(qEnvironmentVariable("XDG_RUNTIME_DIR")), before + 1, 10000);
            // A switch the core refuses leaves it, the picker and the hub on This Frond.
            path->setText(files.filePath("missing")); pick("socket");
            QTRY_VERIFY_WITH_TIMEOUT(shown.contains("Controller socket unavailable"), 5000);
            QTRY_COMPARE_WITH_TIMEOUT(mode->currentData().toString(), QString("radio"), 5000);
            QCOMPARE(machineSetting("bluetooth"), QString("radio"));
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Bluetooth: This Frond's radio"), 5000);
            QVERIFY(controller.s.state() == QLocalSocket::ConnectedState);
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
            machine = QSettings().value("lastSession").toString();
        }
        // At the next start the core starts without Bluetooth and is pointed at the hub once it listens.
        {
            MainWindow window(core.fileName()); window.show(); window.openSession(machine, true);
            auto* mode = window.findChild<QComboBox*>("bluetoothMode"); auto* state = window.findChild<QLabel*>("bluetoothState");
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("stop")->isEnabled(), 5000);
            QVERIFY(!file(".args").contains("--bluetooth"));
            QCOMPARE(mode->currentData().toString(), QString("radio"));
            QTRY_VERIFY_WITH_TIMEOUT(file(".modes").contains(hubPath.toUtf8() + "\n"), 15000);
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Bluetooth: This Frond's radio"), 5000);
            mode->setCurrentIndex(mode->findData("off"));
            QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, mode->currentIndex()));
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString(), 5000);
            QCOMPARE(machineSetting("bluetooth", frondSessions::preferencesPath(machine)), QString("off"));
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        }
        // Machine settings: Off, This Frond, a socket and the connections; none on the IIIx.
        {
            frondSessions::Preferences prefs; QVERIFY(frondSessions::readPreferences(machine, &prefs));
            prefs.bluetooth = "link:00112233445566778899aabbccddeeff";
            MachineSettingsDialog dialog(frondFirmware::Device::tx, prefs); dialog.show();
            auto* picker = dialog.findChild<QComboBox*>("machineBluetooth"); QVERIFY(picker && picker->isVisible());
            auto* field = dialog.findChild<QLineEdit*>("machineBluetoothSocket");
            QCOMPARE(picker->currentText(), QString("Unknown connection"));
            QVERIFY(picker->findData("link:local") < 0);
            QVERIFY(!field->isVisible());
            QCOMPARE(dialog.controls->values().bluetooth, prefs.bluetooth);
            picker->setCurrentIndex(picker->findData("radio"));
            QCOMPARE(dialog.controls->values().bluetooth, QString("radio"));
            picker->setCurrentIndex(picker->findData("socket"));
            QVERIFY(field->isVisible());
            field->setText("/run/bt.sock");
            QCOMPARE(dialog.controls->values().bluetooth, QString("socket:/run/bt.sock"));
            MachineSettingsDialog iiix(frondFirmware::deviceFromId("palm-iiix"), prefs); iiix.show();
            QVERIFY(!iiix.findChild<QComboBox*>("machineBluetooth")->isVisible());
            MachineSettingsDialog zire(frondFirmware::deviceFromId("palm-zire-72"), prefs); zire.show();
            QVERIFY(zire.findChild<QComboBox*>("machineBluetooth")->isVisible());
        }
        QSettings().clear();
    }
    // Mobile phone: This Frond's radio through the hub, which runs the phone beside it while the
    // machine wants it; machine settings have the choice too.
    void bluetoothPhone() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        QFile source(files.filePath("input.frond")); QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("session"); source.close();
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,sys
base=pathlib.Path(sys.argv[1]);modes=pathlib.Path(str(base)+'.modes');modes.write_text('')
s=socket.socket(socket.AF_UNIX);s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t');arg=bytes.fromhex(arg.decode())
    message={b'device-state':b'palm-tx',b'sd-state':b'none',b'network-state':b'disabled',b'keyboard-state':b'detached'}.get(cmd,b'done')
    if cmd==b'bluetooth-mode':
        with modes.open('a') as m: m.write(arg.decode()+'\n')
        message=b'off' if arg==b'off' else arg+b' connected'
    if cmd==b'save':pathlib.Path(arg.decode()).write_text('saved')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit':break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        const auto radioFolder = qEnvironmentVariable("XDG_RUNTIME_DIR") + "/frond/radio";
        QFile::remove(radioFolder + "/phone"); QFile::remove(radioFolder + "/phone-stopped"); QFile::remove(radioFolder + "/phone-dial");
        const auto read = [](const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); };
        const auto hubPath = frondLink::coreSocketPath("bt", QString::number(QCoreApplication::applicationPid()));
        {
            MainWindow window(core.fileName()); window.show(); window.openSession(source.fileName(), true);
            auto* mode = window.findChild<QComboBox*>("bluetoothMode"); auto* state = window.findChild<QLabel*>("bluetoothState");
            const auto pick = [mode](const char* value) {
                mode->setCurrentIndex(mode->findData(value));
                QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, mode->currentIndex()));
            };
            QTRY_VERIFY_WITH_TIMEOUT(mode->isEnabled(), 5000);
            QCOMPARE(mode->itemText(mode->findData("phone")), QString("Mobile phone"));
            pick("phone");
            QTRY_VERIFY_WITH_TIMEOUT(read(source.fileName() + ".modes").contains(hubPath.toUtf8() + "\n"), 15000);
            QTRY_COMPARE_WITH_TIMEOUT(machineSetting("bluetooth"), QString("phone"), 5000);
            // The phone is started on the radio's socket in DUN mode, and the status follows its call.
            QTRY_COMPARE_WITH_TIMEOUT(read(radioFolder + "/phone"), QByteArray(radioFolder.toUtf8() + "/bt-server-bredr\n--dun\n"), 10000);
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Bluetooth: This Frond's radio, Mobile phone: idle"), 5000);
            { QFile dial(radioFolder + "/phone-dial"); QVERIFY(dial.open(QIODevice::WriteOnly)); }
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Bluetooth: This Frond's radio, Mobile phone: dialling"), 5000);
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Bluetooth: This Frond's radio, Mobile phone: online at 33600"), 5000);
            // This Frond without the phone stops it, and the radio stays.
            pick("radio");
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Bluetooth: This Frond's radio"), 5000);
            QTRY_VERIFY_WITH_TIMEOUT(!read(radioFolder + "/phone-stopped").isEmpty(), 10000);
            QCOMPARE(machineSetting("bluetooth"), QString("radio"));
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        }
        {
            frondSessions::Preferences prefs;
            prefs.bluetooth = "phone";
            MachineSettingsDialog dialog(frondFirmware::Device::tx, prefs); dialog.show();
            auto* picker = dialog.findChild<QComboBox*>("machineBluetooth"); QVERIFY(picker && picker->isVisible());
            QCOMPARE(picker->currentText(), QString("Mobile phone"));
            QCOMPARE(dialog.controls->values().bluetooth, QString("phone"));
        }
        QSettings().clear();
    }
    // A live switch from a connection that the core refuses leaves the port off and the hub's
    // attach gone, for serial and infrared.
    void linkSwitchFails() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        QFile source(files.filePath("input.frond")); QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("session"); source.close();
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import socket,sys
s=socket.socket(socket.AF_UNIX);s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
modes={b'serial-mode':b'off',b'irda-mode':b'off'}
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t');arg=bytes.fromhex(arg.decode())
    message={b'device-state':b'palm-tx',b'sd-state':b'none',b'network-state':b'disabled',b'keyboard-state':b'detached'}.get(cmd,b'done')
    if cmd in modes:
        if arg==b'pty':
            s.sendall(i+b'\terror\t'+(b'serial' if cmd==b'serial-mode' else b'infrared')+b'\t'+b'No PTY here'.hex().encode()+b'\n');continue
        modes[cmd]=arg
        message=b'unix '+arg[5:]+b' waiting' if arg.startswith(b'unix:') else arg
        if cmd==b'irda-mode' and arg.startswith(b'unix:'): message=b'unix:'+arg[5:]+b' '+arg[5:]+b' waiting'
    if cmd==b'serial-state': message=modes[b'serial-mode']
    if cmd==b'save':open(arg.decode(),'w').write('saved')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit':break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        HubUser other(frondLink::hubSocketPath());
        FakeSerial otherSerial(QFileInfo(frondLink::hubSocketPath()).path() + "/sw-s.sock");
        FakeIr otherIr(QFileInfo(frondLink::hubSocketPath()).path() + "/sw-i.sock");
        other.client.setSticky("serial", {{"op", "serial-attach"}, {"machine", "Other"}, {"device", "Palm TX"}, {"link", "local"}, {"socket", otherSerial.path}});
        other.client.setSticky("ir", {{"op", "ir-attach"}, {"machine", "Other"}, {"device", "Palm TX"}, {"link", "local"}, {"socket", otherIr.path}});
        QTRY_COMPARE_WITH_TIMEOUT(other.serial(), QString("waiting"), 10000);
        QString shown; QTimer dismiss; dismiss.setInterval(20);
        {
            MainWindow window(core.fileName()); window.show(); window.openSession(source.fileName(), true);
            connect(&dismiss, &QTimer::timeout, &window, [&] {
                if (auto* box = window.findChild<QMessageBox*>(); box && box->isVisible()) { shown = box->text(); box->done(QMessageBox::Ok); }
            }); dismiss.start();
            for (const auto& [name, kind] : {std::pair<const char*, QString>{"serialMode", "serial"}, std::pair<const char*, QString>{"irdaMode", "ir"}}) {
                auto* mode = window.findChild<QComboBox*>(name);
                const auto pick = [mode](const char* value) {
                    mode->setCurrentIndex(mode->findData(value));
                    QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, mode->currentIndex()));
                };
                QTRY_VERIFY_WITH_TIMEOUT(mode->isEnabled(), 5000);
                pick("link:local");
                QTRY_COMPARE_WITH_TIMEOUT(other.last(kind).value("state").toString(), QString("joined"), 10000);
                QTRY_VERIFY_WITH_TIMEOUT(mode->isEnabled(), 5000);
                shown.clear();
                pick("pty");
                QTRY_VERIFY_WITH_TIMEOUT(shown.contains("No PTY here"), 5000);
                QTRY_COMPARE_WITH_TIMEOUT(other.last(kind).value("state").toString(), QString("waiting"), 5000);
                QCOMPARE(mode->currentData().toString(), QString("off"));
            }
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        }
        other.client.setSticky("serial", {}); other.client.setSticky("ir", {});
        QSettings().clear();
    }
    void linkSerialThisComputer() {
        QTemporaryDir dir(QDir::currentPath() + "/build/lk-XXXXXX");
        TestHub h(dir.path(), "h", freePort());
        HubUser l1(h), l2(h), l3(h);
        FakeSerial c1(h.run + "/s1.sock"), c2(h.run + "/s2.sock"), c3(h.run + "/s3.sock");
        auto attach = [](HubUser& u, const QString& machine, const FakeSerial& core) {
            u.client.setSticky("serial", {{"op", "serial-attach"}, {"machine", machine}, {"device", "Tungsten E2"}, {"link", "local"}, {"socket", core.path}});
        };
        attach(l1, "Palm L1", c1);
        QTRY_COMPARE_WITH_TIMEOUT(l1.serial(), QString("waiting"), 10000);
        QCOMPARE(l1.last("serial").value("peer").toString(), QString("This computer"));
        QTest::qWait(600);
        QCOMPARE(c1.opens, 0);
        attach(l2, "Palm L2", c2);
        QTRY_COMPARE_WITH_TIMEOUT(l1.serial(), QString("joined"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(l2.serial(), QString("joined"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(c1.open() && c2.open(), 5000);
        c1.send("one"); QTRY_COMPARE_WITH_TIMEOUT(c2.got, QByteArray("one"), 5000);
        c2.send("two"); QTRY_COMPARE_WITH_TIMEOUT(c1.got, QByteArray("two"), 5000);
        c2.got.clear();
        c2.setRts(false);
        const auto big = pattern(1 << 20, 9);
        c1.send(big);
        QTest::qWait(1500);
        const auto stuck = c1.unsent();
        QVERIFY2(stuck > 512 * 1024, qPrintable(QString::number(stuck)));
        c2.setRts(true);
        QTRY_COMPARE_WITH_TIMEOUT(c2.got.size(), big.size(), 30000);
        QVERIFY(c2.got == big);
        attach(l3, "Palm L3", c3);
        QTRY_COMPARE_WITH_TIMEOUT(l3.serial(), QString("busy"), 5000);
        QCOMPARE(c3.opens, 0);
        // L2 stops: L1's port closes and opens again joined to L3.
        const int opens1 = c1.opens;
        l2.client.setSticky("serial", {});
        QTRY_COMPARE_WITH_TIMEOUT(l3.serial(), QString("joined"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(c1.opens > opens1 && c1.open() && c3.open(), 5000);
        QVERIFY(!c2.open());
        c3.send("three"); QTRY_COMPARE_WITH_TIMEOUT(c1.got, QByteArray("twothree"), 5000);
        killHub(l1);
    }
    // Two hubs find each other by UDP beacon (sent to each other's port on loopback), pair from
    // Nearby, dial the address the beacon gave, and dial at once when a beacon shows a peer moved.
    void linkDiscovery() {
        QTemporaryDir dir(QDir::currentPath() + "/build/lk-XXXXXX");
        const quint16 ua = freeUdpPort(), ub = freeUdpPort(), pb = freePort();
        TestHub ha(dir.path(), "a", freePort()), hb(dir.path(), "b", pb);
        ha.env.insert("FROND_LINK_DISCOVERY_PORT", QString::number(ua));
        ha.env.insert("FROND_LINK_BEACON_TO", QString("127.0.0.1:%1,127.0.0.2:%1").arg(ub));
        hb.env.insert("FROND_LINK_DISCOVERY_PORT", QString::number(ub));
        hb.env.insert("FROND_LINK_BEACON_TO", QString("127.0.0.1:%1").arg(ua));
        HubUser a(ha), a1(ha);
        auto b = std::make_unique<HubUser>(hb), b1 = std::make_unique<HubUser>(hb);
        a.client.send({{"op", "rename"}, {"name", "Ay"}});
        b->client.send({{"op", "rename"}, {"name", "Bee"}});
        a.client.setSticky("watch", {{"op", "watch"}});
        b->client.setSticky("watch", {{"op", "watch"}});
        QTRY_VERIFY_WITH_TIMEOUT(!a.last("hello").isEmpty() && !b->last("hello").isEmpty(), 10000);
        const auto aId = a.client.hello().value("id").toString(), bId = b->client.hello().value("id").toString();
        // Each lists the other under Nearby.
        QTRY_VERIFY_WITH_TIMEOUT(listed(a, "nearby", bId).value("name") == "Bee" && listed(*b, "nearby", aId).value("name") == "Ay", 10000);
        QCOMPARE(listed(a, "nearby", bId).value("address").toString(), QString("127.0.0.1"));
        QCOMPARE(listed(a, "nearby", bId).value("port").toInt(), int(pb));
        // Pair from Nearby, by id.
        a.got.clear(); b->got.clear();
        a.client.send({{"op", "pair"}, {"id", bId}});
        QTRY_VERIFY_WITH_TIMEOUT(!a.last("pair-peer").isEmpty() && !b->last("pair-request").isEmpty(), 10000);
        a.client.send({{"op", "pair-code"}, {"code", b->last("pair-request").value("code").toString().remove(' ')}});
        QTRY_COMPARE_WITH_TIMEOUT(a.last("pair-result").value("result").toString(), QString("paired"), 10000);
        QVERIFY(storedPeer(ha, bId).value("address").toString().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(listed(a, "nearby", bId).isEmpty() && listed(a, "connections", bId).value("seen") == "127.0.0.1", 10000);
        QCOMPARE(ConnectionsWindow::status(listed(a, "connections", bId)), QString("Seen at 127.0.0.1"));
        // Infrared over the address the beacon gave.
        FakeIr coreA(ha.run + "/a.sock"), coreB(hb.run + "/b.sock");
        auto attach = [](HubUser& u, const QString& machine, const QString& link, const FakeIr& core) {
            u.client.setSticky("ir", {{"op", "ir-attach"}, {"machine", machine}, {"device", "Palm TX"}, {"link", link}, {"socket", core.path}});
        };
        attach(a1, "Palm A", bId, coreA);
        attach(*b1, "Palm B", aId, coreB);
        QTRY_COMPARE_WITH_TIMEOUT(a1.ir(), QString("joined"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(coreA.peer && coreB.peer, 5000);
        coreA.send("found"); QTRY_COMPARE_WITH_TIMEOUT(coreB.got, QByteArray("found"), 5000);
        // B's hub stops; A's dials fail and back off: 1, 1, 2, then 5 s, so 9 s on its next try is 10 s away.
        b1->client.setSticky("ir", {}); b->client.setSticky("watch", {});
        QTRY_VERIFY_WITH_TIMEOUT(!b->client.isConnected() && !b1->client.isConnected(), 5000);
        killHub(*b);
        b.reset(); b1.reset();
        QTRY_COMPARE_WITH_TIMEOUT(a1.ir(), QString("notfound"), 10000);
        QTest::qWait(9000);
        QCOMPARE(a1.ir(), QString("notfound"));
        // B comes back on another address and port: its first beacon has A dial at once.
        const auto pb2 = freePort();
        hb.env.insert("FROND_LINK_PORT", QString::number(pb2));
        hb.env.insert("FROND_LINK_DISCOVERY_BIND", "127.0.0.2");
        HubUser b2(hb);
        coreB.got.clear();
        QElapsedTimer back; back.start();
        attach(b2, "Palm B", aId, coreB);
        QTRY_COMPARE_WITH_TIMEOUT(a1.ir(), QString("joined"), 6000);
        QVERIFY2(back.elapsed() < 6000, qPrintable(QString::number(back.elapsed())));
        QCOMPARE(listed(a, "connections", bId).value("seen").toString(), QString("127.0.0.2"));
        QTRY_VERIFY_WITH_TIMEOUT(coreB.peer && coreB.peer->state() == QLocalSocket::ConnectedState, 5000);
        coreA.send("moved"); QTRY_COMPARE_WITH_TIMEOUT(coreB.got, QByteArray("moved"), 5000);
        for (auto* user : {&a, &b2}) killHub(*user);
    }
    // Someone on the LAN sends a beacon with B's id and its own address, then refuses A's handshake
    // as "key changed" in the clear. A stores nothing, tries the address its last handshake with B
    // worked at (kept across a hub restart) and links up there.
    void linkSpoofedBeacon() {
        QTemporaryDir dir(QDir::currentPath() + "/build/lk-XXXXXX");
        const quint16 ua = freeUdpPort();
        TestHub ha(dir.path(), "a", freePort()), hb(dir.path(), "b", freePort());
        ha.env.insert("FROND_LINK_DISCOVERY_PORT", QString::number(ua));
        ha.env.insert("FROND_LINK_BEACON_TO", QString("127.0.0.1:%1").arg(freeUdpPort()));
        HubUser a(ha), b(hb), a1(ha);
        QVERIFY(pairHubs(a, b, false));
        a.client.setSticky("watch", {{"op", "watch"}});
        const auto aId = a.client.hello().value("id").toString(), bId = b.client.hello().value("id").toString();
        // No typed address: only the one the pairing worked at.
        a.client.send({{"op", "peer-address"}, {"id", bId}, {"address", ""}});
        QTRY_VERIFY_WITH_TIMEOUT(storedPeer(ha, bId).value("address").toString().isEmpty(), 5000);
        QCOMPARE(storedPeer(ha, bId).value("lastHost").toString(), QString("127.0.0.1"));
        QCOMPARE(storedPeer(ha, bId).value("lastPort").toInt(), b.client.hello().value("port").toInt());
        const auto oldPid = a.client.hello().value("pid").toDouble();
        killHub(a);
        QTRY_VERIFY_WITH_TIMEOUT(a.client.hello().value("pid").toDouble() != oldPid && a.client.isConnected(), 10000);
        // The spoofer.
        QTcpServer spoof;
        QVERIFY(spoof.listen(QHostAddress::LocalHost, 0));
        int dialled = 0;
        QObject::connect(&spoof, &QTcpServer::newConnection, &spoof, [&] {
            while (auto* s = spoof.nextPendingConnection()) {
                ++dialled;
                const auto refuse = frond::link::refuseFrame(frond::link::RefuseReason::KeyChanged);
                s->write(reinterpret_cast<const char*>(refuse.data()), qint64(refuse.size()));
                s->disconnectFromHost();
            }
        });
        frond::link::Bytes beacon{'F', 'R', 'L', 'B', 1, 1};
        frond::link::InstallId id{};
        QVERIFY(frondLink::parseId(bId, id));
        frond::link::putBytes(beacon, id);
        frond::link::putU16(beacon, spoof.serverPort());
        frond::link::putStr8(beacon, "spoof");
        QUdpSocket udp;
        QVERIFY(udp.writeDatagram(reinterpret_cast<const char*>(beacon.data()), qint64(beacon.size()), QHostAddress::LocalHost, ua) > 0);
        QTest::qWait(300);
        FakeIr core(ha.run + "/a1.sock");
        a1.client.setSticky("ir", {{"op", "ir-attach"}, {"machine", "Palm A"}, {"device", "Palm TX"}, {"link", bId}, {"socket", core.path}});
        QTRY_COMPARE_WITH_TIMEOUT(a1.ir(), QString("waiting"), 15000);
        QVERIFY(dialled >= 1);
        QVERIFY(!storedPeer(ha, bId).value("keyChanged").toBool());
        QCOMPARE(storedPeer(ha, bId).value("lastHost").toString(), QString("127.0.0.1"));
        Q_UNUSED(aId);
        for (auto* user : {&a, &b}) killHub(*user);
    }
    // A code that runs out gets the same cooldown as a lockout, so codes can't be cycled.
    void linkPairCodeCooldown() {
        QTemporaryDir dir(QDir::currentPath() + "/build/lk-XXXXXX");
        TestHub ha(dir.path(), "a", freePort()), hb(dir.path(), "b", freePort());
        hb.env.insert("FROND_LINK_PAIR_CODE_MS", "1500");
        HubUser a(ha), b(hb);
        b.client.setSticky("watch", {{"op", "watch"}});
        QTRY_VERIFY_WITH_TIMEOUT(!b.last("hello").isEmpty(), 10000);
        const auto address = "127.0.0.1:" + QString::number(b.client.hello().value("port").toInt());
        a.client.send({{"op", "pair"}, {"address", address}});
        QTRY_VERIFY_WITH_TIMEOUT(!b.last("pair-request").isEmpty() && !a.last("pair-peer").isEmpty(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(b.last("pair-request-end").value("result").toString(), QString("expired"), 10000);
        a.got.clear(); b.got.clear();
        a.client.send({{"op", "pair"}, {"address", address}});
        QTRY_COMPARE_WITH_TIMEOUT(a.last("pair-result").value("result").toString(), QString("notpairing"), 10000);
        QVERIFY(b.last("pair-request").isEmpty());
        for (auto* user : {&a, &b}) killHub(*user);
    }
    // A window with Connections open stays on the hub. Its machine stops on one link and the next
    // starts on another: the old attach goes, so the new one is the window's.
    void linkReattachSameWindow() {
        QTemporaryDir dir(QDir::currentPath() + "/build/lk-XXXXXX");
        TestHub h(dir.path(), "h", freePort());
        HubUser w(h), other(h);
        w.client.setSticky("watch", {{"op", "watch"}});
        FakeIr c1(h.run + "/w1.sock"), c2(h.run + "/w2.sock"), c3(h.run + "/o.sock");
        auto attach = [](HubUser& u, const QString& machine, const QString& link, const FakeIr& core) {
            u.client.setSticky("ir", {{"op", "ir-attach"}, {"machine", machine}, {"device", "Palm TX"}, {"link", link}, {"socket", core.path}});
        };
        attach(other, "Palm O", "local", c3);
        attach(w, "Palm X", "local", c1);
        QTRY_COMPARE_WITH_TIMEOUT(other.ir(), QString("joined"), 10000);
        QCOMPARE(other.last("ir").value("machine").toString(), QString("Palm X"));
        // Stop, then start on a connection that's gone.
        w.client.setSticky("ir", {});
        attach(w, "Palm Y", "00112233445566778899aabbccddeeff", c2);
        QTRY_COMPARE_WITH_TIMEOUT(w.ir(), QString("unknown"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(other.ir(), QString("waiting"), 5000);
        QVERIFY(w.client.isConnected());
        // And again on This computer, under its own name.
        w.client.setSticky("ir", {});
        attach(w, "Palm Z", "local", c2);
        QTRY_COMPARE_WITH_TIMEOUT(other.ir(), QString("joined"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(other.last("ir").value("machine").toString(), QString("Palm Z"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(c2.peer && c3.peer, 5000);
        c3.send("to Z"); QTRY_COMPARE_WITH_TIMEOUT(c2.got, QByteArray("to Z"), 5000);
        QVERIFY(c1.got.isEmpty());
        killHub(w);
    }
    // A client with nothing sticky lets go after 1 s, and the hub it started exits once idle
    // (FROND_LINK_IDLE_EXIT_MS here, 30 s by default). A watching client keeps it running.
    void linkIdleExit() {
        QTemporaryDir dir(QDir::currentPath() + "/build/lk-XXXXXX");
        TestHub h(dir.path(), "h", freePort());
        h.env.insert("FROND_LINK_IDLE_EXIT_MS", "1500");
        QVERIFY(hubHello(h.socket()).isEmpty());
        qint64 pid = 0;
        {
            HubUser u(h);
            u.client.send({{"op", "state"}});
            QTRY_VERIFY_WITH_TIMEOUT(!u.last("state").isEmpty(), 10000);
            pid = qint64(u.last("hello").value("pid").toDouble());
            QVERIFY(alive(pid));
            QTRY_VERIFY_WITH_TIMEOUT(!u.client.isConnected(), 5000);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!alive(pid), 8000);
        QVERIFY(hubHello(h.socket()).isEmpty());
        HubUser w(h);
        w.client.setSticky("watch", {{"op", "watch"}});
        QTRY_VERIFY_WITH_TIMEOUT(!w.last("hello").isEmpty(), 10000);
        pid = qint64(w.last("hello").value("pid").toDouble());
        QTest::qWait(3000);
        QVERIFY(alive(pid) && w.client.isConnected());
        w.client.setSticky("watch", {});
        QTRY_VERIFY_WITH_TIMEOUT(!w.client.isConnected(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!alive(pid), 8000);
    }
    // File > Connections… with no hub running: the window starts one. Rename this Frond, pair by
    // address through the dialogs (a wrong code first), rename the connection and forget it.
    void connectionsWindowClicks() {
        QSettings().clear(); QSettings().setValue("resume", false);
        qputenv("FROND_LINK_IDLE_EXIT_MS", "1500");
        const auto socket = frondLink::hubSocketPath();
        QVERIFY(!socket.isEmpty() && hubHello(socket).isEmpty());
        QTemporaryDir dir(QDir::currentPath() + "/build/lk-XXXXXX");
        TestHub hb(dir.path(), "b", freePort());
        HubUser b(hb);
        b.client.send({{"op", "rename"}, {"name", "Bee"}});
        b.client.setSticky("watch", {{"op", "watch"}});
        QTRY_VERIFY_WITH_TIMEOUT(!b.last("hello").isEmpty(), 10000);
        qint64 pid = 0;
        {
            MainWindow window{QString()}; window.show();
            auto* action = window.findChild<QAction*>("connectionsAction"); QVERIFY(action);
            action->trigger();
            auto* cw = window.findChild<ConnectionsWindow*>(); QVERIFY(cw && cw->isVisible());
            QTRY_VERIFY_WITH_TIMEOUT(cw->header->text().startsWith("This Frond: "), 10000);
            pid = qint64(hubHello(socket).value("pid").toDouble());
            QVERIFY(alive(pid));
            QCOMPARE(cw->list->count(), 1);
            QVERIFY(cw->list->item(0)->text().startsWith("This computer\n"));
            QVERIFY(cw->nearbyEmpty->isVisible() && !cw->pairButton->isEnabled());
            typeInto("Desk");
            cw->rename->click();
            QTRY_COMPARE_WITH_TIMEOUT(cw->header->text(), QString("This Frond: Desk"), 5000);
            // Add by address, then the code Bee shows.
            b.got.clear();
            typeInto("127.0.0.1:" + hb.env.value("FROND_LINK_PORT"));
            cw->addAddress->click();
            QTRY_VERIFY_WITH_TIMEOUT(cw->codeDialog && cw->codeDialog->isVisible(), 5000);
            QTRY_COMPARE_WITH_TIMEOUT(cw->codeDialog->prompt->text(), QString("Enter the code shown on Bee"), 10000);
            QTRY_VERIFY_WITH_TIMEOUT(!b.last("pair-request").isEmpty(), 5000);
            QCOMPARE(b.last("pair-request").value("name").toString(), QString("Desk"));
            const auto code = b.last("pair-request").value("code").toString();
            const auto wrong = QString("%1").arg((QString(code).remove(' ').toInt() + 1) % 1000000, 6, 10, QChar('0'));
            QVERIFY(!cw->codeDialog->pair->isEnabled());
            QTest::keyClicks(cw->codeDialog->code, wrong.left(3) + " " + wrong.mid(3));
            QVERIFY(cw->codeDialog->pair->isEnabled());
            cw->codeDialog->pair->click();
            QTRY_VERIFY_WITH_TIMEOUT(cw->codeDialog->problem->isVisible(), 10000);
            QCOMPARE(cw->codeDialog->problem->text(), QString("Wrong code"));
            cw->codeDialog->code->clear();
            QTest::keyClicks(cw->codeDialog->code, code);
            cw->codeDialog->pair->click();
            QTRY_VERIFY_WITH_TIMEOUT(!cw->codeDialog, 10000);
            QTRY_COMPARE_WITH_TIMEOUT(cw->list->count(), 2, 5000);
            QTRY_VERIFY_WITH_TIMEOUT(cw->list->item(1)->text().startsWith("Bee\n"), 5000);
            QTRY_VERIFY_WITH_TIMEOUT(b.last("pair-request-end").value("result") == "paired", 5000);
            QCOMPARE(frondLink::pairedConnections().size(), 1);
            const auto id = frondLink::pairedConnections()[0].id;
            QCOMPARE(id, b.client.hello().value("id").toString());
            // Row menu: Rename…
            auto menuOn = [cw](int row) {
                emit cw->list->customContextMenuRequested(cw->list->visualItemRect(cw->list->item(row)).center());
            };
            pickMenu("renameConnection");
            typeInto("Bee's desk");
            menuOn(1);
            QTRY_VERIFY_WITH_TIMEOUT(cw->list->count() == 2 && cw->list->item(1)->text().startsWith("Bee's desk\n"), 5000);
            QCOMPARE(frondLink::connectionName(id), QString("Bee's desk"));
            // Row menu: Forget, confirmed.
            pickMenu("forgetConnection");
            whenShown<QMessageBox>([](QMessageBox* box) {
                for (auto* button : box->buttons()) if (button->text() == "Forget") { button->click(); return; }
                box->reject();
            });
            menuOn(1);
            QTRY_COMPARE_WITH_TIMEOUT(cw->list->count(), 1, 5000);
            QVERIFY(frondLink::pairedConnections().isEmpty());
            QVERIFY(!listed(b, "connections", hubHello(socket).value("id").toString()).isEmpty());
            cw->close();
            window.close();
        }
        // Nothing uses the hub the window started any more: it exits.
        QTRY_VERIFY_WITH_TIMEOUT(!alive(pid), 10000);
        killHub(b);
        qunsetenv("FROND_LINK_IDLE_EXIT_MS");
        QSettings().clear();
    }
    // A window with Infrared on This computer runs the core on a Unix socket and shows the hub's status.
    void infraredConnection() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        QFile source(files.filePath("input.frond")); QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("session"); source.close();
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,sys
base=pathlib.Path(sys.argv[1]);pathlib.Path(str(base)+'.args').write_text('\n'.join(sys.argv[1:]))
s=socket.socket(socket.AF_UNIX);s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t');arg=bytes.fromhex(arg.decode())
    message={b'device-state':b'palm-tx',b'sd-state':b'none',b'network-state':b'disabled',b'keyboard-state':b'detached'}.get(cmd,b'done')
    if cmd==b'irda-mode':
        state=b'off' if arg==b'off' else arg+b' '+arg[5:]+b' waiting'
        message=state
    if cmd==b'save':pathlib.Path(arg.decode()).write_text('saved')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit':break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        {
            MainWindow window(core.fileName()); window.show(); window.openSession(source.fileName(), true);
            auto* mode = window.findChild<QComboBox*>("irdaMode"); auto* state = window.findChild<QLabel*>("irdaState");
            auto* port = window.findChild<QSpinBox*>("irdaPort");
            QTRY_VERIFY_WITH_TIMEOUT(mode->isEnabled(), 5000);
            QVERIFY(mode->findData("link:local") > 0);
            mode->setCurrentIndex(mode->findData("link:local"));
            QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, mode->currentIndex()));
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Infrared: waiting for a machine on This computer"), 15000);
            QVERIFY(!port->isVisible());
            QCOMPARE(machineSetting("infrared"), QString("link:local"));
            mode->setCurrentIndex(mode->findData("off"));
            QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, mode->currentIndex()));
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString(), 5000);
            QCOMPARE(machineSetting("infrared"), QString("off"));
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        }
        // A machine set to a connection that's gone shows it and starts with Infrared off.
        {
            const auto settings = frondSessions::preferencesPath(QSettings().value("lastSession").toString());
            frondSessions::Preferences prefs; QVERIFY(frondSessions::readPreferences(QSettings().value("lastSession").toString(), &prefs));
            prefs.infrared = "link:00112233445566778899aabbccddeeff";
            QVERIFY(frondSessions::writePreferences(QSettings().value("lastSession").toString(), prefs).isEmpty());
            MachineSettingsDialog dialog(frondFirmware::Device::tx, prefs); dialog.show();
            auto* picker = dialog.findChild<QComboBox*>("machineInfrared"); QVERIFY(picker && picker->isVisible());
            QCOMPARE(picker->currentText(), QString("Unknown connection"));
            QVERIFY(picker->findData("link:local") > 0);
            picker->setCurrentIndex(picker->findData("tcp"));
            dialog.findChild<QSpinBox*>("machineInfraredPort")->setValue(5044);
            QCOMPARE(dialog.controls->values().infrared, QString("tcp:5044"));
            MachineSettingsDialog iiix(frondFirmware::deviceFromId("palm-iiix"), prefs); iiix.show();
            QVERIFY(iiix.findChild<QComboBox*>("machineInfrared")->isVisible());
            QVERIFY(!QFileInfo(settings).fileName().isEmpty());
        }
        QSettings().clear();
    }
    void serialConnection() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        QFile source(files.filePath("input.frond")); QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("session"); source.close();
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,sys
base=pathlib.Path(sys.argv[1]);pathlib.Path(str(base)+'.args').write_text('\n'.join(sys.argv[1:]))
s=socket.socket(socket.AF_UNIX);s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
state=b'off'
if '--serial' in sys.argv:
    arg=sys.argv[sys.argv.index('--serial')+1].encode()
    state=b'unix '+arg[5:]+b' waiting' if arg.startswith(b'unix:') else arg
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t');arg=bytes.fromhex(arg.decode())
    message={b'device-state':b'palm-tx',b'sd-state':b'none',b'network-state':b'disabled',b'keyboard-state':b'detached'}.get(cmd,b'done')
    if cmd==b'serial-mode':
        state=b'off' if arg==b'off' else b'unix '+arg[5:]+b' waiting' if arg.startswith(b'unix:') else arg
    if cmd in (b'serial-mode',b'serial-state'):message=state
    if cmd==b'save':pathlib.Path(arg.decode()).write_text('saved')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit':break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        QString machine;
        const auto args = [&] { QFile f((machine.isEmpty() ? source.fileName() : machine) + ".args"); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); };
        {
            MainWindow window(core.fileName()); window.show(); window.openSession(source.fileName(), true);
            auto* mode = window.findChild<QComboBox*>("serialMode"); auto* state = window.findChild<QLabel*>("serialState");
            auto* port = window.findChild<QSpinBox*>("serialPort");
            QTRY_VERIFY_WITH_TIMEOUT(mode->isEnabled(), 5000);
            QVERIFY(mode->findData("link:local") > 0);
            mode->setCurrentIndex(mode->findData("link:local"));
            QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, mode->currentIndex()));
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Serial: waiting for a machine on This computer"), 15000);
            QVERIFY(!port->isVisible());
            QCOMPARE(machineSetting("serial"), QString("link:local"));
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
            machine = QSettings().value("lastSession").toString();
        }
        // The setting is applied at the next start, with the core's port on a Unix socket.
        {
            MainWindow window(core.fileName()); window.show(); window.openSession(machine, true);
            auto* mode = window.findChild<QComboBox*>("serialMode"); auto* state = window.findChild<QLabel*>("serialState");
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("stop")->isEnabled(), 5000);
            QVERIFY(args().contains("--serial\nunix:") && args().contains("/serial-"));
            QCOMPARE(mode->currentData().toString(), QString("link:local"));
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Serial: waiting for a machine on This computer"), 15000);
            mode->setCurrentIndex(mode->findData("off"));
            QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, mode->currentIndex()));
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString(), 5000);
            QCOMPARE(machineSetting("serial", frondSessions::preferencesPath(machine)), QString("off"));
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        }
        // Machine settings: a connection that's gone shows as such; no Serial row without serial.
        {
            frondSessions::Preferences prefs; QVERIFY(frondSessions::readPreferences(machine, &prefs));
            prefs.serial = "link:00112233445566778899aabbccddeeff";
            MachineSettingsDialog dialog(frondFirmware::Device::tx, prefs); dialog.show();
            auto* picker = dialog.findChild<QComboBox*>("machineSerial"); QVERIFY(picker && picker->isVisible());
            QCOMPARE(picker->currentText(), QString("Unknown connection"));
            QVERIFY(picker->findData("link:local") > 0);
            QVERIFY(!dialog.findChild<QSpinBox*>("machineSerialPort")->isVisible());
            QCOMPARE(dialog.controls->values().serial, prefs.serial);
            picker->setCurrentIndex(picker->findData("link:local"));
            QCOMPARE(dialog.controls->values().serial, QString("link:local"));
            picker->setCurrentIndex(picker->findData("tcp"));
            dialog.findChild<QSpinBox*>("machineSerialPort")->setValue(5045);
            QCOMPARE(dialog.controls->values().serial, QString("tcp:5045"));
            MachineSettingsDialog iiix(frondFirmware::deviceFromId("palm-iiix"), prefs); iiix.show();
            QVERIFY(iiix.findChild<QComboBox*>("machineSerial")->isVisible());
            MachineSettingsDialog zire(frondFirmware::deviceFromId("palm-zire-72"), prefs); zire.show();
            QVERIFY(!zire.findChild<QComboBox*>("machineSerial")->isVisible());
            QVERIFY(zire.findChild<QComboBox*>("machineInfrared")->isVisible());
        }
        QSettings().clear();
    }
    // Each machine keeps its own peripherals: the old global choices go to a machine from before
    // once, and later global values don't reach it.
    void peripheralMigration() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QSettings().setValue("serialMode", "tcp"); QSettings().setValue("serialPort", 5077);
        QSettings().setValue("irdaMode", "pty"); QSettings().setValue("usbMode", "off");
        QTemporaryDir files;
        QFile source(files.filePath("input.frond")); QVERIFY(source.open(QIODevice::WriteOnly)); source.write("session"); source.close();
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import pathlib,socket,sys
pathlib.Path(sys.argv[1]+'.args').write_text('\n'.join(sys.argv[1:]))
s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    message={b'keyboard-state':b'detached',b'network-state':b'disabled',b'sd-state':b'none',b'device-state':b'palm-tx',
             b'serial-state':b'tcp 127.0.0.1:5077 waiting'}.get(cmd,b'done')
    if cmd==b'save': pathlib.Path(bytes.fromhex(arg.decode()).decode()).write_text('saved')
    s.sendall(i+b'\tok\tresult\t'+message.hex().encode()+b'\n')
    if cmd==b'quit': break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QString machine;
        const auto args = [&] { QFile f((machine.isEmpty() ? source.fileName() : machine) + ".args"); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); };
        {
            MainWindow window(core.fileName()); window.show(); window.openSession(source.fileName(), true);
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("stop")->isEnabled(), 5000);
            QVERIFY(args().contains("--serial\ntcp:5077") && args().contains("--irda\npty") && !args().contains("--usb"));
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
            machine = QSettings().value("lastSession").toString();
        }
        QCOMPARE(machineSetting("serial", frondSessions::preferencesPath(machine)), QString("tcp:5077"));
        QCOMPARE(machineSetting("infrared", frondSessions::preferencesPath(machine)), QString("pty"));
        // The globals are left alone, and the machine no longer follows them.
        QSettings().setValue("serialMode", "pty"); QSettings().setValue("irdaMode", "off");
        {
            MainWindow window(core.fileName()); window.show(); window.openSession(machine, true);
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("stop")->isEnabled(), 5000);
            QVERIFY(args().contains("--serial\ntcp:5077") && args().contains("--irda\npty"));
            QCOMPARE(window.findChild<QComboBox*>("serialMode")->currentData().toString(), QString("tcp"));
            QCOMPARE(window.findChild<QSpinBox*>("serialPort")->value(), 5077);
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        }
        QSettings().clear();
    }
    void tg50BluetoothControls() {
        const auto core = qEnvironmentVariable("FROND_TEST_TG50_CORE");
        const auto source = qEnvironmentVariable("FROND_TEST_TG50_HOST_SESSION");
        const auto endpoint = qEnvironmentVariable("FROND_TEST_TG50_BT_SOCKET");
        if (core.isEmpty() || source.isEmpty() || endpoint.isEmpty()) QSKIP("TG50 Bluetooth inputs not set");
        qputenv("SDL_AUDIODRIVER", "dummy"); qputenv("SDL_VIDEODRIVER", "dummy");
        QSettings().clear(); QSettings().setValue("resume", false);
        QSettings().setValue("bluetoothEnabled", true); QSettings().setValue("bluetoothSocket", endpoint);
        const auto original = digest(source);
        MainWindow window(core); window.show(); window.openSession(source, true);
        auto* enabled = window.findChild<QComboBox*>("bluetoothMode");
        auto* path = window.findChild<QLineEdit*>("bluetoothSocket");
        auto* state = window.findChild<QLabel*>("bluetoothState");
        auto* stop = window.findChild<QPushButton*>("stop");
        const auto pick = [enabled](const char* mode) {
            enabled->setCurrentIndex(enabled->findData(mode));
            QMetaObject::invokeMethod(enabled, "activated", Q_ARG(int, enabled->currentIndex()));
        };
        QTRY_VERIFY_WITH_TIMEOUT(stop->isEnabled() && enabled->isEnabled(), 20000);
        QCOMPARE(enabled->currentData().toString(), QString("off")); QVERIFY(!path->isEnabled());
        path->setText(endpoint); pick("socket"); QTRY_VERIFY_WITH_TIMEOUT(state->text().contains("provider connected"), 5000);
        QString shown; QTimer dismiss; dismiss.setInterval(20);
        connect(&dismiss, &QTimer::timeout, &window, [&] {
            if (auto* box = window.findChild<QMessageBox*>(); box && box->isVisible()) { shown = box->text(); box->done(QMessageBox::Ok); }
        }); dismiss.start();
        path->setText("relative"); emit path->editingFinished();
        QTRY_VERIFY_WITH_TIMEOUT(!shown.isEmpty(), 5000);
        QCOMPARE(machineSetting("bluetooth"), "socket:" + endpoint);
        path->setText(endpoint); pick("off");
        QTRY_VERIFY_WITH_TIMEOUT(enabled->isEnabled() && state->text().isEmpty(), 5000);
        pick("socket"); QTRY_VERIFY_WITH_TIMEOUT(state->text().contains("provider connected"), 5000);
        stop->click(); QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("addDevice")->isEnabled(), 15000);
        const auto saved = QSettings().value("lastSession").toString(); QVERIFY(QFileInfo::exists(saved));
        window.openSession(saved, true);
        QTRY_VERIFY_WITH_TIMEOUT(stop->isEnabled() && enabled->isEnabled(), 20000);
        QCOMPARE(enabled->currentData().toString(), QString("off"));
        stop->click(); QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("addDevice")->isEnabled(), 15000);
        QCOMPARE(digest(source), original); QSettings().clear();
    }
    void tg50HostControls() {
        const auto core = qEnvironmentVariable("FROND_TEST_TG50_CORE");
        const auto source = qEnvironmentVariable("FROND_TEST_TG50_HOST_SESSION");
        if (core.isEmpty() || source.isEmpty()) QSKIP("TG50 host inputs not set");
        qputenv("SDL_AUDIODRIVER", "dummy"); qputenv("SDL_VIDEODRIVER", "dummy");
        QSettings().clear(); QSettings().setValue("resume", false);
        const auto original = digest(source);
        MainWindow window(core); window.show(); window.openSession(source, true);
        auto* mic = window.findChild<QCheckBox*>("microphone");
        auto* sound = window.findChild<QCheckBox*>("sound");
        auto* mode = window.findChild<QComboBox*>("serialMode");
        auto* port = window.findChild<QSpinBox*>("serialPort");
        auto* state = window.findChild<QLabel*>("serialState");
        auto* stop = window.findChild<QPushButton*>("stop");
        QTRY_VERIFY_WITH_TIMEOUT(mic->isEnabled(), 20000); QVERIFY(!mic->isChecked());
        mic->click(); QTRY_VERIFY_WITH_TIMEOUT(mic->isEnabled() && mic->isChecked(), 5000);
        sound->click(); QTRY_VERIFY_WITH_TIMEOUT(mic->isEnabled(), 5000); QVERIFY(mic->isChecked());
        mode->setCurrentIndex(mode->findData("pty"));
        QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, mode->currentIndex()));
        QTRY_VERIFY_WITH_TIMEOUT(state->text().contains("/dev/pts/"), 5000);
        mode->setCurrentIndex(mode->findData("off"));
        QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, mode->currentIndex()));
        QTRY_VERIFY_WITH_TIMEOUT(state->text().isEmpty(), 5000);
        QTcpServer reservation; QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
        const auto chosen = reservation.serverPort(); reservation.close(); port->setValue(chosen);
        mode->setCurrentIndex(mode->findData("tcp"));
        QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, mode->currentIndex()));
        QTRY_VERIFY_WITH_TIMEOUT(state->text().contains("waiting"), 5000);
        QTcpSocket peer; peer.connectToHost(QHostAddress::LocalHost, chosen); QVERIFY(peer.waitForConnected(5000));
        QTRY_VERIFY_WITH_TIMEOUT(state->text().contains("connected") && !state->text().contains("waiting"), 5000);
        peer.disconnectFromHost(); QTRY_VERIFY_WITH_TIMEOUT(state->text().contains("waiting"), 5000);
        stop->click(); QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("addDevice")->isEnabled(), 15000);
        QVERIFY(!mic->isChecked());
        const auto saved = QSettings().value("lastSession").toString(); QVERIFY(QFileInfo::exists(saved));
        window.openSession(saved, true);
        QTRY_VERIFY_WITH_TIMEOUT(mic->isEnabled(), 20000); QVERIFY(!mic->isChecked());
        mic->click(); QTRY_VERIFY_WITH_TIMEOUT(mic->isEnabled() && mic->isChecked(), 5000);
        QTimer confirm; confirm.setInterval(20);
        connect(&confirm, &QTimer::timeout, &window, [&] {
            if (auto* box = window.findChild<QMessageBox*>(); box && box->isVisible()) box->button(QMessageBox::Discard)->click();
        });
        confirm.start(); window.findChild<QPushButton*>("forceStop")->click();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("addDevice")->isEnabled(), 15000); confirm.stop();
        QVERIFY(!mic->isChecked());
        window.openSession(saved, true);
        QTRY_VERIFY_WITH_TIMEOUT(mic->isEnabled(), 20000); QVERIFY(!mic->isChecked());
        stop->click(); QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("addDevice")->isEnabled(), 15000);
        QCOMPARE(digest(source), original); QVERIFY(window.close()); QSettings().clear();
    }
    void tg50Acceptance() {
        const auto core = qEnvironmentVariable("FROND_TEST_TG50_CORE");
        const auto nor = qEnvironmentVariable("FROND_TEST_TG50_NOR");
        const auto source = qEnvironmentVariable("FROND_TEST_TG50_SESSION");
        const auto shots = qEnvironmentVariable("FROND_TEST_TG50_SHOTS");
        if (core.isEmpty() || nor.isEmpty() || source.isEmpty() || shots.isEmpty()) QSKIP("TG50 acceptance inputs not set");
        QVERIFY(QDir().mkpath(shots));
        const auto original = digest(source), originalNor = digest(nor);
        QSettings().clear(); QSettings().setValue("resume", false);
        auto command = [](const QString& program, const QStringList& args) {
            QProcess process; process.start(program, args);
            if (!process.waitForFinished(15000) || process.exitCode()) return QString();
            return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
        };
        auto xd = [&](const QStringList& args) { return command("xdotool", args); };
        auto nativeWindow = [&] { return xd({"search", "--name", "Frond.*TG50"}).split('\n').last(); };
        auto capture = [&](MainWindow& window, const QString& id, const QString& name) {
            window.grab().save(shots + '/' + name + "-menu.png");
            command("import", {"-window", id, shots + '/' + name + "-guest.png"});
        };
        auto focus = [&](const QString& id) { xd({"windowraise", id}); xd({"windowfocus", "--sync", id}); };
        auto tap = [&](const QString& id, int x, int y) { xd({"mousemove", "--window", id, QString::number(x), QString::number(y), "mousedown", "1"}); QTest::qWait(200); xd({"mouseup", "1"}); QTest::qWait(400); };
        auto key = [&](const QString& name) { xd({"keydown", name}); QTest::qWait(name == "F6" ? 600 : 180); xd({"keyup", name}); QTest::qWait(250); };
        int panelProbes = 0;
        auto panelBlack = [&](const QString& id) {
            const auto path = shots + "/panel-probe-" + QString::number(++panelProbes) + ".png";
            command("import", {"-window", id, path});
            const QImage frame(path);
            return !frame.isNull() && frame.pixelColor(160,160) == QColor(Qt::black);
        };
        QString working;
        {
            MainWindow window(core); window.show();
            QTimer import; import.setInterval(40);
            int added = 0;
            connect(&import, &QTimer::timeout, &window, [&] {
                auto* dialog = window.findChild<FirmwareDialog*>();
                if (!dialog || !dialog->isVisible()) return;
                dialog->findChild<QLineEdit*>("nor")->setText(nor);
                dialog->machineSettings->windowScale->setValue(added ? 5 : 4);
                dialog->machineSettings->smoothLcd->setChecked(!added);
                dialog->machineSettings->audio->setChecked(!added);
                QVERIFY(dialog->machineSettings->audio->isEnabled());
                QVERIFY(dialog->machineSettings->fastAudio->isEnabled());
                ++added; dialog->findChild<QPushButton*>("import")->click();
            });
            import.start(); window.findChild<QPushButton*>("addDevice")->click(); import.stop();
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QCheckBox*>("hold")->isEnabled(), 30000);
            QCOMPARE(added, 1);
            window.grab().save(shots + "/firmware-added.png");
            window.findChild<QPushButton*>("stop")->click();
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("addDevice")->isEnabled(), 60000);
            import.start(); window.findChild<QPushButton*>("addDevice")->click(); import.stop();
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QCheckBox*>("hold")->isEnabled(), 30000);
            QCOMPARE(added, 2);
            window.findChild<QPushButton*>("stop")->click();
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("addDevice")->isEnabled(), 60000);
            const auto machines = frondSessions::list(managedFolder());
            QCOMPARE(machines.size(), 2);
            const auto editedPath = window.findChild<QListWidget*>("sessionList")->item(0)->data(Qt::UserRole).toString();
            const auto otherPath = machines[0].path == editedPath ? machines[1].path : machines[0].path;
            frondSessions::Preferences first, second;
            QVERIFY(frondSessions::readPreferences(editedPath, &first));
            QVERIFY(frondSessions::readPreferences(otherPath, &second));
            QVERIFY(first.scale != second.scale && first.smooth != second.smooth && first.audio != second.audio);
            const auto before = digest(frondSessions::preferencesPath(editedPath));
            bool saveEdit = false;
            int editsShown = 0;
            QTimer edit; edit.setInterval(30);
            connect(&edit, &QTimer::timeout, &window, [&] {
                auto* dialog = window.findChild<MachineSettingsDialog*>();
                if (!dialog || !dialog->isVisible()) return;
                QVERIFY(dialog->controls->fastAudio->isEnabled());
                dialog->controls->windowScale->setValue(8);
                ++editsShown;
                if (saveEdit) dialog->accept(); else dialog->reject();
            });
            edit.start(); rowAction(window, 0, "editSession");
            QTRY_COMPARE(editsShown, 1);
            QCOMPARE(digest(frondSessions::preferencesPath(editedPath)), before);
            saveEdit = true; rowAction(window, 0, "editSession"); QTRY_COMPARE(editsShown, 2); edit.stop();
            QVERIFY(frondSessions::readPreferences(editedPath, &first)); QCOMPARE(first.scale, 8);
            frondSessions::Preferences unchanged;
            QVERIFY(frondSessions::readPreferences(otherPath, &unchanged)); QCOMPARE(unchanged.scale, second.scale);
            QSettings().setValue("windowScale", 4);
            window.openSession(source, true);
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QCheckBox*>("hold")->isEnabled(), 30000);
            working = QSettings().value("lastSession").toString();
            const auto id = nativeWindow(); QVERIFY(!id.isEmpty());
            QTest::qWait(2000); focus(id); key("F6"); QTest::qWait(1500);
            capture(window, id, "launcher");
            QCOMPARE(window.findChild<QLabel*>("storageHeading")->text(), QString("Memory Stick"));
            showCardPanel(window.findChild<QPushButton*>("cardPick"));
            QCOMPARE(window.findChild<QPushButton*>("cardPick")->isVisible(), frondFirmware::profile(frondFirmware::deviceFromId("sony-clie-tg50")).frontend.memoryStick);
            QVERIFY(!window.findChild<QPushButton*>("sdExchange")->isVisible());
            QVERIFY(!window.findChild<QComboBox*>("usbMode")->isEnabled());
            QVERIFY(window.findChild<QComboBox*>("bluetoothMode")->isEnabled());
            QVERIFY(window.findChild<QCheckBox*>("keyboardAttached")->isEnabled());
            focus(id); key("F4"); QTest::qWait(1200); tap(id, 20, 306);
            key("q"); xd({"keydown", "Shift_L"}); key("a"); xd({"keyup", "Shift_L"});
            xd({"keydown", "Alt_L"}); key("q"); xd({"keyup", "Alt_L"}); QTest::qWait(600);
            capture(window, id, "typed-qA1");
            xd({"keydown", "Shift_L"}); xd({"windowfocus", QString::number(window.winId())}); QTest::qWait(200);
            xd({"keyup", "Shift_L"}); QTest::qWait(150);
            focus(id); key("b"); QTest::qWait(400); capture(window, id, "focus-release-b");
            key("F6"); QTest::qWait(800); capture(window, id, "home-before-jog");
            tap(id, 337, 394); capture(window, id, "jog-down");
            tap(id, 337, 368); capture(window, id, "jog-press");
            window.findChild<QCheckBox*>("sound")->click();
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QCheckBox*>("sound")->isEnabled(), 10000);
            window.findChild<QCheckBox*>("sound")->click();
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QCheckBox*>("sound")->isEnabled(), 10000);
            focus(id); key("F6"); key("F4"); tap(id, 80, 46);
            auto* hold = window.findChild<QCheckBox*>("hold"); hold->click();
            QTRY_VERIFY_WITH_TIMEOUT(hold->isEnabled() && hold->isChecked(), 10000);
            QTRY_VERIFY_WITH_TIMEOUT(panelBlack(id), 5000);
            focus(id); key("z"); tap(id, 100, 100);
            QTest::qWait(600); capture(window, id, "hold-on");
            const QImage black(shots + "/hold-on-guest.png"); QVERIFY(!black.isNull());
            QCOMPARE(black.pixelColor(160, 160), QColor(Qt::black));
            window.findChild<QPushButton*>("stop")->click();
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("addDevice")->isEnabled(), 60000);
            working = QSettings().value("lastSession").toString(); QVERIFY(QFileInfo::exists(working));
            QVERIFY(QFile::copy(working, shots + "/held.frond"));
            QVERIFY(QFile::copy(frondSessions::preferencesPath(working), shots + "/held.frond.settings.json"));
            window.close();
        }
        {
            MainWindow fresh(core); fresh.show(); fresh.openSession(working, true);
            auto* hold = fresh.findChild<QCheckBox*>("hold");
            QTRY_VERIFY_WITH_TIMEOUT(hold->isEnabled() && hold->isChecked(), 30000);
            const auto id = nativeWindow(); QVERIFY(!id.isEmpty()); QTest::qWait(800);
            capture(fresh, id, "fresh-held"); hold->click();
            QTRY_VERIFY_WITH_TIMEOUT(hold->isEnabled() && !hold->isChecked(), 10000);
            QTest::qWait(500); capture(fresh, id, "fresh-released-memo");
            focus(id); key("c"); capture(fresh, id, "first-typing-after-hold");
            key("F6"); QTest::qWait(1000); capture(fresh, id, "fresh-released-home");
            fresh.findChild<QPushButton*>("stop")->click();
            QTRY_VERIFY_WITH_TIMEOUT(fresh.findChild<QPushButton*>("addDevice")->isEnabled(), 60000);
            fresh.close();
        }
        const auto tone = qEnvironmentVariable("FROND_TEST_TG50_TONE_SESSION");
        if (!tone.isEmpty()) {
            MainWindow player(core); player.show(); player.openSession(tone, true);
            auto* hold = player.findChild<QCheckBox*>("hold");
            QTRY_VERIFY_WITH_TIMEOUT(hold->isEnabled(), 30000);
            const auto id = nativeWindow(); QVERIFY(!id.isEmpty()); focus(id); QTest::qWait(500);
            tap(id, 32, 250); hold->click();
            QTRY_VERIFY_WITH_TIMEOUT(hold->isEnabled() && hold->isChecked(), 10000);
            QTRY_VERIFY_WITH_TIMEOUT(panelBlack(id), 5000);
            const auto files = QDir(shots).entryInfoList({"audio-*.raw"}, QDir::Files, QDir::Time);
            QVERIFY(!files.isEmpty()); QFile audio(files.first().absoluteFilePath());
            QVERIFY(audio.open(QIODevice::ReadOnly)); const auto offset = audio.size(); QTest::qWait(400);
            QVERIFY(audio.seek(offset)); const auto pcm = audio.readAll();
            bool signal = false;
            for (int i = 0; i + 1 < pcm.size(); i += 2) {
                const auto sample = qint16(quint8(pcm[i]) | (quint16(quint8(pcm[i+1])) << 8));
                signal |= std::abs(int(sample)) > 256;
            }
            QVERIFY(signal); capture(player, id, "hold-audio-playing");
            hold->click(); QTRY_VERIFY_WITH_TIMEOUT(hold->isEnabled() && !hold->isChecked(), 10000);
            player.findChild<QPushButton*>("stop")->click();
            QTRY_VERIFY_WITH_TIMEOUT(player.findChild<QPushButton*>("addDevice")->isEnabled(), 60000);
            player.close();
        }
        QCOMPARE(digest(source), original); QCOMPARE(digest(nor), originalNor);
    }
    // Not run by ctest: FROND_TEST_SHOTS=<folder> saves pictures of the home window and Settings.
    void screenshots() {
        const auto folder = qEnvironmentVariable("FROND_TEST_SHOTS");
        if (folder.isEmpty()) QSKIP("FROND_TEST_SHOTS not set");
        QSettings().clear(); QSettings().setValue("resume", false);
        QDir(managedFolder()).removeRecursively();
        putSession("Palm TX", 6, "palm-tx", 1);
        putSession("Tungsten E2 for a trip with a very long name to see it cut off", 6, "palm-e2", 0);
        QVERIFY(frondSessions::create(managedFolder(), "Fresh", "palm-tx"));
        MainWindow window{QString()}; window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTest::qWait(200);
        QVERIFY(window.grab().save(folder + "/home.png"));
        auto* dialog = window.findChild<SettingsDialog*>();
        dialog->show();
        QVERIFY(QTest::qWaitForWindowExposed(dialog));
        QVERIFY(dialog->grab().save(folder + "/settings.png"));
        dialog->hide();
        QDir(managedFolder()).removeRecursively();
        // With FROND_TEST_CORE and FROND_TEST_E2_NOR (and optionally FROND_TEST_E2_NAND) a real E2 runs for running.png.
        const auto core = qEnvironmentVariable("FROND_TEST_CORE"), nor = qEnvironmentVariable("FROND_TEST_E2_NOR");
        if (!core.isEmpty() && !nor.isEmpty()) {
            const auto root = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/frond/firmware";
            QVERIFY(frondFirmware::importImages(root, frondFirmware::Device::e2, nor, qEnvironmentVariable("FROND_TEST_E2_NAND")).isEmpty());
            QVERIFY(frondSessions::create(managedFolder(), "Tungsten E2", "palm-e2"));
            MainWindow running(core); running.show();
            auto* list = running.findChild<QListWidget*>("sessionList");
            emit list->itemActivated(list->item(0));
            QTRY_VERIFY_WITH_TIMEOUT(running.findChild<QLabel*>("sdState")->text() != "Not known", 60000);
            QTest::qWait(500);
            QVERIFY(running.grab().save(folder + "/running.png"));
            running.close();
            QTRY_VERIFY_WITH_TIMEOUT(!running.isVisible(), 60000);
            QDir(managedFolder()).removeRecursively();
            QDir(root).removeRecursively();
        }
        QSettings().clear();
    }
    void persistence() {
        const auto core = qEnvironmentVariable("FROND_TEST_CORE");
        const auto source = qEnvironmentVariable("FROND_TEST_SESSION");
        QVERIFY(!core.isEmpty() && !source.isEmpty());
        const auto before = digest(source);
        QVERIFY(!before.isEmpty());
        QString saved, cardState, networkMode;
        bool keyboardAttached=false;
        {
            MainWindow window(core); window.show(); window.openSession(source, true);
            auto* stop = window.findChild<QPushButton*>("stop");
            QTRY_VERIFY_WITH_TIMEOUT(stop->isEnabled(), 30000);
            const auto screenshot = qEnvironmentVariable("FROND_TEST_SCREENSHOT");
            if (!screenshot.isEmpty()) QVERIFY(window.grab().save(screenshot));
            cardState = window.findChild<QLabel*>("sdState")->text();
            networkMode = window.findChild<QComboBox*>("networkMode")->currentData().toString();
            keyboardAttached=window.findChild<QCheckBox*>("keyboardAttached")->isChecked();
            const auto expectedKeyboard=qEnvironmentVariable("FROND_TEST_KEYBOARD");
            if (!expectedKeyboard.isEmpty()) QCOMPARE(keyboardAttached,expectedKeyboard=="attached");
            const auto expectedNetwork = qEnvironmentVariable("FROND_TEST_NETWORK");
            if (!expectedNetwork.isEmpty()) QCOMPARE(networkMode, expectedNetwork);
            QVERIFY(!window.close());
            QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 60000);
            saved = QSettings().value("lastSession").toString();
            QVERIFY(saved != source && QFileInfo::exists(saved));
            QCOMPARE(digest(source), before);
        }
        {
            MainWindow window(core); window.show();
            auto* stop = window.findChild<QPushButton*>("stop");
            QTRY_VERIFY_WITH_TIMEOUT(stop->isEnabled(), 30000);
            QCOMPARE(window.findChild<QLabel*>("sdState")->text(), cardState);
            QCOMPARE(window.findChild<QComboBox*>("networkMode")->currentData().toString(), networkMode);
            QCOMPARE(window.findChild<QCheckBox*>("keyboardAttached")->isChecked(),keyboardAttached);
            QVERIFY(!window.close());
            QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 60000);
            QCOMPARE(QSettings().value("lastSession").toString(), saved);
        }
        QSettings().setValue("resume", false);
        {
            MainWindow window(core); window.show();
            QTest::qWait(100);
            QVERIFY(window.findChild<QPushButton*>("start")->isEnabled());
            QVERIFY(!window.findChild<QPushButton*>("stop")->isEnabled());
            window.close();
        }
    }
    // ---- Cards (C4): the library, machine operations and the slot at start and close ----
    void cardsMoveIn() {
        cardsReset();
        const auto a = cardsMachine("Palm TX", true);
        cardsMachine("Empty", false);
        // A v6 machine is left alone; an unsaved one too.
        QDir().mkpath(cardsSessions() + "/Old");
        { QFile f(cardsSessions() + "/Old/Old.frond"); f.open(QIODevice::WriteOnly); QByteArray b(40, '\0'); putLe(b, 0, frondSessions::magic, 4); putLe(b, 4, 6, 4); f.write(b); }
        frondCards::startUp(cardsSessions());
        const auto card = onlyCard();
        QCOMPARE(card.name, QString("Palm TX card"));
        QCOMPARE(card.kind, QString("sd"));
        QCOMPARE(card.bytes, qint64(32) << 20);
        QCOMPARE(card.identity, machineIdentity("Palm TX"));
        QCOMPARE(card.machine, QString("Palm TX"));
        auto slot = frondCards::readSlot(a);
        QVERIFY(slot.present && slot.card == card.id && slot.link.isEmpty() && !slot.swap && slot.name == card.name);
        QVERIFY(QFileInfo(a + "/sd.frondsd").isFile());
        QVERIFY(!frondCards::readSlot(cardsSessions() + "/Empty").present);
        // Again: nothing changes.
        frondCards::reconcile(cardsSessions());
        QCOMPARE(frondCards::entries(cardsLibrary()).size(), 1);
        // A folder copied by hand holds the same id: the one the hint doesn't name gets "Palm TX card 2".
        QVERIFY(QDir().mkpath(cardsSessions() + "/Copy"));
        QVERIFY(QFile::copy(a + "/Palm TX.frond", cardsSessions() + "/Copy/Copy.frond"));
        QVERIFY(QFile::copy(a + "/sd.frondsd", cardsSessions() + "/Copy/sd.frondsd"));
        QVERIFY(QFile::copy(a + "/slot.json", cardsSessions() + "/Copy/slot.json"));
        frondCards::reconcile(cardsSessions());
        QCOMPARE(frondCards::readSlot(a).card, card.id);
        const auto second = cardNamed("Palm TX card 2");
        QVERIFY(!second.id.isEmpty() && second.machine == "Copy");
        QCOMPARE(frondCards::readSlot(cardsSessions() + "/Copy").card, second.id);
        // A machine open elsewhere is skipped.
        const auto b = cardsMachine("Busy", true);
        QLockFile busy(b + "/Busy.frond.lock"); busy.setStaleLockTime(0);
        QVERIFY(busy.tryLock());
        frondCards::reconcile(cardsSessions());
        QVERIFY(!frondCards::readSlot(b).present);
        busy.unlock();
        frondCards::reconcile(cardsSessions());
        QCOMPARE(frondCards::readSlot(b).card, cardNamed("Busy card").id);
        // Two machines with one serial and no slot.json (an older Frond's Duplicate): the second
        // gets a new serial and loads it as a swap.
        const auto twinA = cardsMachine("Twin A", true), twinB = cardsMachine("Twin B", true);
        TestMedia same; same.identity = machineIdentity("Twin A");
        writeFrond(twinB + "/Twin B.frond", "palm-tx", {same});
        frondCards::reconcile(cardsSessions());
        QCOMPARE(cardNamed("Twin A card").identity, machineIdentity("Twin A"));
        QVERIFY(!frondCards::readSlot(twinA).swap);
        const auto twin = cardNamed("Twin B card");
        QVERIFY(!twin.id.isEmpty() && twin.identity != machineIdentity("Twin A") && frondCards::readSlot(twinB).swap);
        QVERIFY(frondCards::launchArguments(cardsSessions(), twinB).join(' ').contains("--card-swap --card-identity " + twin.identity.toHex()));
    }
    // A core still saving after Frond died holds <session>.core: the machine counts as in use and
    // reconcile leaves it. Once it exits, a take-out Frond didn't finish (the save has no card, the
    // file is still there and the slot unchanged) puts the card in the drawer.
    void cardsCoreStillSaving() {
        cardsReset();
        const auto a = cardsMachine("Palm TX", true);
        frondCards::reconcile(cardsSessions());
        const auto card = onlyCard();
        const auto frond = a + "/Palm TX.frond";
        QVERIFY(!frondSessions::coreRunning(frond) && !frondSessions::inUse(frond));
        const int core = ::open(QFile::encodeName(frondSessions::corePath(frond)).constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        QVERIFY(core >= 0 && ::flock(core, LOCK_EX | LOCK_NB) == 0);
        writeFrond(frond, "palm-tx", {});
        QVERIFY(frondSessions::coreRunning(frond) && frondSessions::inUse(frond));
        QLockFile lock(frond + ".lock"); lock.setStaleLockTime(0);
        QVERIFY(!frondSessions::tryLockSession(lock, frond));
        frondCards::reconcile(cardsSessions());
        QVERIFY(QFileInfo(a + "/sd.frondsd").isFile());
        QCOMPARE(frondCards::readSlot(a).card, card.id);
        ::close(core);
        QVERIFY(!frondSessions::inUse(frond));
        frondCards::reconcile(cardsSessions());
        QVERIFY(!QFileInfo(a + "/sd.frondsd").isFile());
        QVERIFY(frondCards::readSlot(a).card.isEmpty());
        QVERIFY(frondCards::entries(cardsLibrary()).value(0).inDrawer);
        // A card put in while stopped (swap set) stays for the next start.
        {
            frondCards::LibraryLock library(cardsLibrary());
            QVERIFY(frondCards::toMachine(cardsLibrary(), card.id, a).isEmpty());
        }
        frondCards::reconcile(cardsSessions());
        QVERIFY(QFileInfo(a + "/sd.frondsd").isFile() && frondCards::readSlot(a).swap);
    }
    // A move to another file system fails rather than copying the card.
    void cardsMoveNeverCopies() {
        cardsReset();
        cardsMachine("Palm TX", true);
        frondCards::reconcile(cardsSessions());
        const auto a = cardsSessions() + "/Palm TX";
        const auto card = onlyCard();
        {
            frondCards::LibraryLock lock(cardsLibrary());
            QVERIFY(frondCards::toDrawer(cardsLibrary(), a, card.id).isEmpty());
        }
        QTemporaryDir other;
        if (QStorageInfo(other.path()).device() == QStorageInfo(cardsLibrary()).device())
            QSKIP("The temporary folder is on the same file system as the library");
        const auto away = other.path() + "/Away";
        QVERIFY(QDir().mkpath(away));
        {
            frondCards::LibraryLock lock(cardsLibrary());
            QVERIFY(frondCards::toMachine(cardsLibrary(), card.id, away).contains("same disk"));
        }
        QVERIFY(!QFileInfo::exists(away + "/sd.frondsd"));
        QVERIFY(QFileInfo(cardsLibrary() + '/' + card.id + "/sd.frondsd").isFile());
        QVERIFY(frondCards::readSlot(away).card.isEmpty());
        QVERIFY(!frondCards::moveFile(cardsLibrary() + '/' + card.id + "/sd.frondsd", away + "/sd.frondsd"));
        QVERIFY(QFileInfo(cardsLibrary() + '/' + card.id + "/sd.frondsd").isFile() && !QFileInfo::exists(away + "/sd.frondsd"));
    }
    void cardsDeleteKeepsCard() {
        cardsReset();
        // Not moved in yet: delete moves it in first.
        const auto a = cardsMachine("Palm TX", true);
        const auto digestBefore = digest(a + "/sd.frondsd");
        QVERIFY(frondSessions::remove(frondSessions::describe(a)).isEmpty());
        QVERIFY(!QFileInfo::exists(a));
        auto card = onlyCard();
        QCOMPARE(card.name, QString("Palm TX card"));
        QVERIFY(card.machine.isEmpty());
        QCOMPARE(digest(cardsLibrary() + '/' + card.id + "/sd.frondsd"), digestBefore);
        // Unsettled: the save's full copy at generation 2 is what the .frond describes.
        const auto b = cardsMachine("Tungsten", true);
        TestMedia two; two.generation = 2;
        writeFrond(b + "/Tungsten.frond", "palm-tx", {two});
        QVERIFY(QFile::copy(b + "/sd.frondsd", b + "/sd.frondsd.journal-2.full"));
        QVERIFY(setCardGeneration(b + "/sd.frondsd.journal-2.full", 2));
        QVERIFY(frondSessions::remove(frondSessions::describe(b)).isEmpty());
        QVERIFY(!QFileInfo::exists(b));
        card = cardNamed("Tungsten card");
        QCOMPARE(cardGeneration(cardsLibrary() + '/' + card.id + "/sd.frondsd"), quint64(2));
        QVERIFY(QDir(cardsLibrary() + '/' + card.id).entryList(QDir::Files).size() == 2);
        // A journal that can't be applied: refused, nothing removed.
        const auto c = cardsMachine("Zire", true);
        writeFrond(c + "/Zire.frond", "palm-tx", {two});
        { QFile j(c + "/sd.frondsd.journal-2"); j.open(QIODevice::WriteOnly); j.write("broken"); }
        const auto error = frondSessions::remove(frondSessions::describe(c));
        QCOMPARE(error, QString("Open Zire once to finish its last save."));
        QVERIFY(QFileInfo(c + "/sd.frondsd").isFile() && QFileInfo(c + "/Zire.frond").isFile());
        // A full disk stops the settle copy the same way.
        QFile::remove(c + "/sd.frondsd.journal-2");
        QVERIFY(QFile::copy(c + "/sd.frondsd", c + "/sd.frondsd.journal-2.full"));
        QVERIFY(setCardGeneration(c + "/sd.frondsd.journal-2.full", 2));
        const auto keep = frondCards::freeSpace();
        frondCards::freeSpace() = [](const QString&) { return qint64(1 << 20); };
        QVERIFY(frondSessions::remove(frondSessions::describe(c)).startsWith("Not enough free space"));
        frondCards::freeSpace() = keep;
        QVERIFY(QFileInfo(c + "/Zire.frond").isFile() && QFileInfo(c + "/sd.frondsd").isFile());
        QVERIFY(QDir(cardsLibrary()).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).filter(".new-").isEmpty());
        // Open elsewhere: refused.
        const auto d = cardsMachine("Open", true);
        QLockFile open(d + "/Open.frond.lock"); open.setStaleLockTime(0); QVERIFY(open.tryLock());
        QVERIFY(!frondSessions::remove(frondSessions::describe(d)).isEmpty());
        QVERIFY(QFileInfo(d + "/sd.frondsd").isFile());
        // Never saved (it crashed before its first save) with a library card put in: the card goes back.
        QVERIFY(frondSessions::create(cardsSessions(), "Unsaved", "palm-tx"));
        const auto e = cardsSessions() + "/Unsaved";
        const auto spare = cardNamed("Palm TX card");
        {
            frondCards::LibraryLock lock(cardsLibrary());
            QVERIFY(frondCards::toMachine(cardsLibrary(), spare.id, e).isEmpty());
        }
        const auto spareDigest = digest(e + "/sd.frondsd");
        QVERIFY(!frondSessions::describe(e).saved);
        QVERIFY(frondSessions::remove(frondSessions::describe(e)).isEmpty());
        QVERIFY(!QFileInfo::exists(e));
        QCOMPARE(digest(cardsLibrary() + '/' + spare.id + "/sd.frondsd"), spareDigest);
        QVERIFY(cardNamed("Palm TX card").machine.isEmpty());
    }
    void cardsDuplicate() {
        cardsReset();
        const auto a = cardsMachine("Palm TX", true);
        const auto privateMode = QFile::ReadOwner | QFile::WriteOwner;
        QVERIFY(QFile::setPermissions(frondSessions::sessionFrondIn(a), privateMode));
        QString name;
        // Copy: a settled copy as a new card in the copy, with its own serial, loaded as a swap.
        QVERIFY(frondSessions::duplicate(cardsSessions(), frondSessions::describe(a), &name, {}, frondSessions::CardChoice::copy).isEmpty());
        QCOMPARE(name, QString("Palm TX 2"));
        const auto original = cardNamed("Palm TX card"), copy = cardNamed("Palm TX card 2");
        QVERIFY(!copy.id.isEmpty() && copy.id != original.id && copy.identity != original.identity && copy.machine == "Palm TX 2");
        const auto copyDir = cardsSessions() + "/Palm TX 2";
        QCOMPARE(frondCards::readSlot(copyDir).card, copy.id);
        QVERIFY(frondCards::readSlot(copyDir).swap);
        QVERIFY(frondCards::launchArguments(cardsSessions(), copyDir).join(' ').contains("--card-identity " + copy.identity.toHex()));
        QCOMPARE(digest(copyDir + "/sd.frondsd"), digest(a + "/sd.frondsd"));
        QCOMPARE(QFile::permissions(frondSessions::sessionFrondIn(copyDir)) & 0x7077, privateMode);
        // No card.
        QVERIFY(frondSessions::duplicate(cardsSessions(), frondSessions::describe(a), &name, {}, frondSessions::CardChoice::none).isEmpty());
        auto slot = frondCards::readSlot(cardsSessions() + '/' + name);
        QVERIFY(slot.present && slot.card.isEmpty() && slot.link.isEmpty() && slot.swap);
        QVERIFY(frondCards::cardFileIn(cardsSessions() + '/' + name).isEmpty());
        // Link: nothing copied, both linked; the original keeps holding it until reconcile frees it.
        QVERIFY(frondSessions::duplicate(cardsSessions(), frondSessions::describe(a), &name, {}, frondSessions::CardChoice::link).isEmpty());
        const auto linked = cardsSessions() + '/' + name;
        slot = frondCards::readSlot(linked);
        QVERIFY(slot.card.isEmpty() && slot.link == original.id && slot.swap && frondCards::cardFileIn(linked).isEmpty());
        QCOMPARE(frondCards::readSlot(a).link, original.id);
        frondCards::reconcile(cardsSessions());
        QVERIFY(QFileInfo(cardsLibrary() + '/' + original.id + "/sd.frondsd").isFile());
        QVERIFY(frondCards::cardFileIn(a).isEmpty());
        slot = frondCards::readSlot(a);
        QVERIFY(slot.card.isEmpty() && slot.link == original.id && slot.swap);
        // A linked card that isn't in the machine: Copy is refused, Link still works.
        QVERIFY(!frondSessions::duplicate(cardsSessions(), frondSessions::describe(a), &name, {}, frondSessions::CardChoice::copy).isEmpty());
        QVERIFY(frondSessions::duplicate(cardsSessions(), frondSessions::describe(a), &name, {}, frondSessions::CardChoice::link).isEmpty());
        QCOMPARE(frondCards::readSlot(cardsSessions() + '/' + name).link, original.id);
        // Full disk: the copy fails and leaves no folder or card behind.
        const auto entriesBefore = frondCards::entries(cardsLibrary()).size();
        const auto keep = frondCards::freeSpace();
        frondCards::freeSpace() = [](const QString&) { return qint64(1 << 20); };
        QVERIFY(frondSessions::duplicate(cardsSessions(), frondSessions::describe(copyDir), &name, {}, frondSessions::CardChoice::copy).startsWith("Not enough free space"));
        frondCards::freeSpace() = keep;
        QCOMPARE(frondCards::entries(cardsLibrary()).size(), entriesBefore);
        QVERIFY(QDir(cardsSessions()).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).filter(".dup-").isEmpty());
        // Cancelled: the same.
        QVERIFY(frondSessions::duplicate(cardsSessions(), frondSessions::describe(copyDir), &name, [](qint64, qint64) { return false; },
                                         frondSessions::CardChoice::copy) == "Duplicate cancelled.");
        QCOMPARE(frondCards::entries(cardsLibrary()).size(), entriesBefore);
    }
    void cardsExportImport() {
        cardsReset();
        QTemporaryDir out(QDir::currentPath() + "/build/cards-export-XXXXXX");
        const auto a = cardsMachine("Palm TX", true);
        frondCards::reconcile(cardsSessions());
        const auto original = onlyCard();
        QVERIFY(frondSessions::exportFile(frondSessions::describe(a), out.path() + "/Trip.frond").isEmpty());
        QVERIFY(QFileInfo(out.path() + "/Trip/slot.json").isFile() && QFileInfo(out.path() + "/Trip/sd.frondsd").isFile());
        // Exporting again replaces it: a folder with slot.json is still a machine folder.
        QVERIFY(frondSessions::exportFile(frondSessions::describe(a), out.path() + "/Trip.frond").isEmpty());
        QString name;
        QVERIFY(frondSessions::importFile(cardsSessions(), out.path() + "/Trip", &name).isEmpty());
        const auto imported = cardsSessions() + '/' + name;
        const auto slot = frondCards::readSlot(imported);
        const auto card = cardNamed("Palm TX card 2");
        QVERIFY(!card.id.isEmpty() && card.id != original.id && slot.card == card.id && slot.link.isEmpty());
        QCOMPARE(digest(imported + "/sd.frondsd"), digest(a + "/sd.frondsd"));
        // An export from an older Frond has no slot.json: moved in the same way.
        QFile::remove(out.path() + "/Trip/slot.json");
        QVERIFY(frondSessions::importFile(cardsSessions(), out.path() + "/Trip/Trip.frond", &name).isEmpty());
        QCOMPARE(frondCards::readSlot(cardsSessions() + '/' + name).card, cardNamed(name + " card").id);
        // A linked machine whose card is free exports and imports without one, swap kept.
        QString linkName;
        QVERIFY(frondSessions::duplicate(cardsSessions(), frondSessions::describe(a), &linkName, {}, frondSessions::CardChoice::link).isEmpty());
        frondCards::reconcile(cardsSessions());
        QVERIFY(frondSessions::exportFile(frondSessions::describe(a), out.path() + "/Linked.frond").isEmpty());
        QVERIFY(frondSessions::importFile(cardsSessions(), out.path() + "/Linked", &name).isEmpty());
        const auto noCard = frondCards::readSlot(cardsSessions() + '/' + name);
        QVERIFY(noCard.swap && noCard.card.isEmpty() && noCard.link.isEmpty());
        // A classic 4 MB stick as an older Frond saved it, a v1 file with a nonzero change counter,
        // imports.
        const auto stick = out.path() + "/Stick";
        QVERIFY(QDir().mkpath(stick));
        QVERIFY(cardTool({"make", "ms", "4", out.path() + "/stick-v2.frondms"}));
        {
            std::vector<uint8_t> physical;
            QVERIFY(frondMs::readPhysical((out.path() + "/stick-v2.frondms").toStdString(), physical));
            auto v1 = frondMs::encode(physical);
            frondMs::putChanges(v1.data(), 3);
            QFile f(stick + "/ms.frondms"); QVERIFY(f.open(QIODevice::WriteOnly));
            QVERIFY(f.write(reinterpret_cast<const char*>(v1.data()), qint64(v1.size())) == qint64(v1.size()));
        }
        QFile::remove(out.path() + "/stick-v2.frondms");
        TestMedia ms; ms.kind = 2; ms.logical = 4ull << 20; ms.physical = 4ull * 2048 * 521; ms.generation = 3;
        writeFrond(stick + "/Stick.frond", "sony-clie-tg50", {ms});
        QVERIFY2(frondSessions::importFile(cardsSessions(), stick, &name).isEmpty(), "classic stick import");
        const auto stickCard = cardNamed("Stick card");
        QVERIFY(stickCard.kind == "ms" && stickCard.bytes == qint64(4) << 20);
        // The counter has to match the descriptor.
        writeFrond(stick + "/Stick.frond", "sony-clie-tg50", {TestMedia{2, 4ull << 20, 4ull * 2048 * 521, 4}});
        QVERIFY(!frondSessions::importFile(cardsSessions(), stick, &name).isEmpty());
    }
    void cardsCutOffMoves() {
        cardsReset();
        const auto a = cardsMachine("Palm TX", true), b = cardsMachine("Tungsten", false);
        frondCards::reconcile(cardsSessions());
        const auto card = onlyCard();
        // Cut off after the rename out of a machine: the drawer wins.
        auto slot = frondCards::readSlot(a);
        slot.swap = true;
        QVERIFY(frondCards::writeSlot(a, slot));
        QVERIFY(QFile::rename(a + "/sd.frondsd", cardsLibrary() + '/' + card.id + "/sd.frondsd"));
        frondCards::reconcile(cardsSessions());
        slot = frondCards::readSlot(a);
        QVERIFY(slot.card.isEmpty() && slot.swap);
        // Cut off before the rename into a machine: the same.
        QVERIFY(frondCards::writeSlot(b, {true, card.id, {}, card.name, true}));
        frondCards::reconcile(cardsSessions());
        QVERIFY(frondCards::readSlot(b).card.isEmpty() && QFileInfo(cardsLibrary() + '/' + card.id + "/sd.frondsd").isFile());
        // A whole move, then a machine deleted by an older Frond: its entry and links go.
        {
            frondCards::LibraryLock lock(cardsLibrary());
            QVERIFY(frondCards::toMachine(cardsLibrary(), card.id, b).isEmpty());
        }
        QVERIFY(QFileInfo(b + "/sd.frondsd").isFile());
        slot = frondCards::readSlot(a);
        slot.link = card.id;
        QVERIFY(frondCards::writeSlot(a, slot));
        QVERIFY(QDir(b).removeRecursively());
        frondCards::reconcile(cardsSessions());
        QVERIFY(frondCards::entries(cardsLibrary()).isEmpty());
        QVERIFY(frondCards::readSlot(a).link.isEmpty());
    }
    void cardsCrashLeftovers() {
        cardsReset();
        QDir().mkpath(cardsLibrary());
        // Hidden copies: an old unlocked one goes, a fresh one and a locked one stay.
        for (const auto* name : {"/.new-old", "/.new-fresh", "/.new-held"}) {
            QDir().mkpath(cardsLibrary() + name);
            QFile f(cardsLibrary() + name + "/sd.frondsd"); f.open(QIODevice::WriteOnly); f.write("x"); f.close();
        }
        for (const auto* name : {"/.new-old", "/.new-held"})
            for (const auto& path : {cardsLibrary() + name + "/sd.frondsd", cardsLibrary() + name}) {
                struct timeval old[2] = {{time(nullptr) - 3600, 0}, {time(nullptr) - 3600, 0}};
                QVERIFY(utimes(QFile::encodeName(path).constData(), old) == 0);
            }
        QLockFile held(cardsLibrary() + "/.new-held.lock"); held.setStaleLockTime(0); QVERIFY(held.tryLock());
        frondCards::startUp(cardsSessions());
        QVERIFY(!QFileInfo::exists(cardsLibrary() + "/.new-old"));
        QVERIFY(QFileInfo::exists(cardsLibrary() + "/.new-fresh") && QFileInfo::exists(cardsLibrary() + "/.new-held"));
        held.unlock();
        // A linked holder that crashed: settled, the card goes back to the drawer (rule 6).
        const auto a = cardsMachine("Palm TX", true);
        frondCards::reconcile(cardsSessions());
        const auto card = onlyCard();
        auto slot = frondCards::readSlot(a);
        slot.link = card.id;
        QVERIFY(frondCards::writeSlot(a, slot));
        // While it runs (locked), nothing moves.
        {
            QLockFile running(a + "/Palm TX.frond.lock"); running.setStaleLockTime(0); QVERIFY(running.tryLock());
            frondCards::reconcile(cardsSessions());
            QVERIFY(QFileInfo(a + "/sd.frondsd").isFile());
            QCOMPARE(frondCards::entries(cardsLibrary()).size(), 1);
        }
        // Unsettled, it stays held.
        { QFile j(a + "/sd.frondsd.journal-2"); j.open(QIODevice::WriteOnly); j.write("partial"); }
        frondCards::reconcile(cardsSessions());
        QVERIFY(QFileInfo(a + "/sd.frondsd").isFile());
        QFile::remove(a + "/sd.frondsd.journal-2");
        frondCards::reconcile(cardsSessions());
        QVERIFY(!QFileInfo::exists(a + "/sd.frondsd") && QFileInfo(cardsLibrary() + '/' + card.id + "/sd.frondsd").isFile());
        slot = frondCards::readSlot(a);
        QVERIFY(slot.card.isEmpty() && slot.link == card.id && slot.swap);
        QVERIFY(onlyCard().machine.isEmpty());
    }
    // Several files install one after the other in the order given; a failure doesn't stop the rest
    // and the failures come back in one message.
    void installSeveral() {
        cardsReset();
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        const auto core = fakeInstallCore(files.path());
        const auto machine = cardsMachine("Palm TX", false);
        MainWindow window(core);
        window.show();
        window.openSession(machine + "/Palm TX.frond", true);
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("installApp")->isEnabled(), 20000);
        const auto log = files.path() + "/installs";
        const auto read = [&] { QFile f(log); return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts) : QStringList(); };
        window.installFiles({"/x/b.prc", "/x/a.prc", "/x/c.pdb"});
        QTRY_COMPARE_WITH_TIMEOUT(read(), QStringList({"/x/b.prc", "/x/a.prc", "/x/c.pdb"}), 5000);
        QTRY_VERIFY(window.findChild<QPushButton*>("installApp")->isEnabled());
        QVERIFY(!window.findChild<QMessageBox*>());
        QFile::remove(log);
        window.installFiles({"/x/one.prc", "/x/bad1.prc", "/x/two.prc", "/x/bad2.pdb", "/x/three.prc"});
        QTRY_COMPARE_WITH_TIMEOUT(read().size(), 5, 5000);
        QCOMPARE(read(), QStringList({"/x/one.prc", "/x/bad1.prc", "/x/two.prc", "/x/bad2.pdb", "/x/three.prc"}));
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QMessageBox*>() && window.findChild<QMessageBox*>()->isVisible(), 5000);
        const auto text = window.findChild<QMessageBox*>()->text();
        QVERIFY(text.contains("bad1.prc: Not a Palm database.") && text.contains("bad2.pdb: Not a Palm database."));
        QVERIFY(!text.contains("one.prc") && !text.contains("three.prc"));
        window.findChild<QMessageBox*>()->close();
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 10000);
        QDir(managedFolder()).removeRecursively();
        QSettings().clear();
    }
    // The core dying partway through a batch ends the batch: installing works again after a restart.
    void installCoreDies() {
        cardsReset();
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        const auto core = fakeInstallCore(files.path());
        const auto machine = cardsMachine("Palm TX", false);
        MainWindow window(core);
        window.show();
        window.openSession(machine + "/Palm TX.frond", true);
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("installApp")->isEnabled(), 20000);
        const auto log = files.path() + "/installs";
        const auto read = [&] { QFile f(log); return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts) : QStringList(); };
        window.installFiles({"/x/a.prc", "/x/die.prc", "/x/b.prc"});
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(files.path() + "/died"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!window.findChild<QPushButton*>("stop")->isEnabled(), 10000);
        for (auto* box : window.findChildren<QMessageBox*>()) box->close();
        QFile::remove(log);
        window.openSession(machine + "/Palm TX.frond", true);
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("installApp")->isEnabled(), 20000);
        window.installFiles({"/x/c.prc", "/x/d.prc"});
        QTRY_COMPARE_WITH_TIMEOUT(read(), QStringList({"/x/c.prc", "/x/d.prc"}), 5000);
        for (auto* box : window.findChildren<QMessageBox*>()) box->close();
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 10000);
        QDir(managedFolder()).removeRecursively();
        QSettings().clear();
    }
    // Two machines linked to one card take turns in the window, with a fake core: each takes it at
    // start with --card-swap and its identity, and gives it back after its final save.
    void cardsLinkedTurns() {
        cardsReset();
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        const auto core = fakeCardCore(files.path());
        const auto a = cardsMachine("Palm TX", true), b = cardsMachine("Tungsten", false);
        frondCards::reconcile(cardsSessions());
        const auto card = onlyCard();
        QString name;
        for (const auto& dir : {a, b}) {
            auto slot = frondCards::readSlot(dir);
            slot.present = true; slot.link = card.id; slot.swap = slot.swap || dir == b;
            QVERIFY(frondCards::writeSlot(dir, slot));
        }
        frondCards::reconcile(cardsSessions());  // a, stopped, gives it up
        QVERIFY(QFileInfo(cardsLibrary() + '/' + card.id + "/sd.frondsd").isFile());
        const auto identity = QString::fromLatin1(card.identity.toHex());
        {
            MainWindow window(core);
            QVERIFY(runAndClose(window, b));
        }
        QVERIFY(argsOf(b).contains("--card-swap\n--card-identity\n" + identity));
        QVERIFY(QFileInfo(cardsLibrary() + '/' + card.id + "/sd.frondsd").isFile() && frondCards::cardFileIn(b).isEmpty());
        QVERIFY(frondCards::readSlot(b).swap);
        // b holds it while it runs; a second window's a starts empty and sees it come free.
        {
            MainWindow first(core), second(core);
            first.show();
            first.openSession(b + "/Tungsten.frond", true);
            QTRY_VERIFY_WITH_TIMEOUT(first.findChild<QPushButton*>("stop")->isEnabled(), 10000);
            QVERIFY(QFileInfo(b + "/sd.frondsd").isFile());
            QCOMPARE(onlyCard().machine, QString("Tungsten"));
            second.show();
            second.openSession(a + "/Palm TX.frond", true);
            QTRY_VERIFY_WITH_TIMEOUT(second.findChild<QPushButton*>("stop")->isEnabled(), 10000);
            QVERIFY(frondCards::cardFileIn(a).isEmpty());
            QVERIFY(argsOf(a).contains("--card-swap") && !argsOf(a).contains("--card-identity"));
            QVERIFY(!second.linkedCardFree());
            first.close();
            QTRY_VERIFY_WITH_TIMEOUT(!first.isVisible(), 10000);
            QTRY_VERIFY_WITH_TIMEOUT(second.linkedCardFree(), 8000);
            second.close();
            QTRY_VERIFY_WITH_TIMEOUT(!second.isVisible(), 10000);
        }
        // An unlinked machine keeps its card; swap clears after its save.
        auto slot = frondCards::readSlot(a);
        slot.link.clear();
        QVERIFY(frondCards::writeSlot(a, slot));
        {
            frondCards::LibraryLock lock(cardsLibrary());
            QVERIFY(frondCards::toMachine(cardsLibrary(), card.id, a).isEmpty());
        }
        QVERIFY(frondCards::readSlot(a).swap);
        {
            MainWindow window(core);
            QVERIFY(runAndClose(window, a));
        }
        QVERIFY(argsOf(a).contains("--card-swap\n--card-identity\n" + identity));
        QVERIFY(QFileInfo(a + "/sd.frondsd").isFile() && !frondCards::readSlot(a).swap);
        {
            MainWindow window(core);
            QVERIFY(runAndClose(window, a));
        }
        QVERIFY(!argsOf(a).contains("--card-"));
        // A 68k machine never gets card flags.
        const auto iiix = cardsMachine("Palm IIIx", false, "palm-iiix");
        QVERIFY(frondCards::writeSlot(iiix, {true, {}, {}, {}, true}));
        {
            MainWindow window(core, core);
            QVERIFY(runAndClose(window, iiix));
        }
        QVERIFY(!argsOf(iiix).isEmpty() && !argsOf(iiix).contains("--card-"));
        QSettings().clear();
    }
    // With the T|X smoke session (FROND_ARM_SESSIONS) and the real core: two linked machines take
    // turns, and a card changed between one's close and its next start resumes as a new card.
    void cardsRealCore() {
        const auto smoke = qEnvironmentVariable("FROND_ARM_SESSIONS");
        if (smoke.isEmpty() || !QFileInfo(smoke + "/palm-tx").isDir()) QSKIP("Set FROND_ARM_SESSIONS to the smoke sessions");
        cardsReset();
        QSettings().clear(); QSettings().setValue("resume", false);
        qputenv("SDL_VIDEODRIVER", "dummy"); qputenv("SDL_AUDIODRIVER", "dummy");
        auto machine = [&](const QString& name) {
            const auto dir = cardsSessions() + '/' + name;
            QDir().mkpath(dir);
            for (const auto& e : QDir(smoke + "/palm-tx").entryInfoList(QDir::Files)) {
                if (!frondSessions::packableName(e.fileName())) continue;
                QFile::copy(e.absoluteFilePath(), dir + '/' + frondSessions::packableDestName(e.fileName(), name));
            }
            frondSessions::Preferences prefs; prefs.audio = false; prefs.peripheralsSet = true;
            frondSessions::writePreferences(dir + '/' + name + ".frond", prefs);
            return dir;
        };
        const auto a = machine("Palm TX"), b = machine("Palm TX 2");
        QVERIFY(frondCards::cardFileIn(a).isEmpty());
        // A drawer card made by the card tool, linked to both.
        frondCards::Card card{frondCards::newId(), "Games", "sd", frondCards::now(), {}, qint64(32) << 20, frondCards::newIdentity(), false};
        const auto entry = cardsLibrary() + '/' + card.id;
        QVERIFY(QDir().mkpath(entry) && frondCards::writeCard(entry, card));
        QVERIFY(cardTool({"make", "sd", "32", entry + "/sd.frondsd"}));
        for (const auto& dir : {a, b}) QVERIFY(frondCards::writeSlot(dir, {true, {}, card.id, {}, false}));
        auto described = [](const QString& dir) {
            const auto cards = frondCards::descriptorsOf(dir + '/' + QFileInfo(dir).fileName() + ".frond");
            const auto* d = frondCards::describedCard(cards, "sd.frondsd");
            return d ? *d : frondCards::Descriptor{};
        };
        const auto core = cardsCore();
        for (const auto& dir : {a, b, a}) {
            MainWindow window(core);
            QVERIFY(runAndClose(window, dir));
            // The save recorded the card, then it went back to the drawer.
            const auto d = described(dir);
            QVERIFY2(d.kind == 1 && d.identity == card.identity, qPrintable(dir));
            QVERIFY(QFileInfo(entry + "/sd.frondsd").isFile() && frondCards::cardFileIn(dir).isEmpty());
            QCOMPARE(d.generation, cardGeneration(entry + "/sd.frondsd"));
        }
        // Changed between a's close and its next start: a takes the new generation.
        const auto next = cardGeneration(entry + "/sd.frondsd") + 5;
        QVERIFY(setCardGeneration(entry + "/sd.frondsd", next));
        {
            MainWindow window(core);
            QVERIFY(runAndClose(window, a));
        }
        QVERIFY(described(a).generation >= next);
        QCOMPARE(described(a).generation, cardGeneration(entry + "/sd.frondsd"));
        QSettings().clear();
    }
    // Part A: a machine's Copy card has its own serial. Written in both and swapped between the two
    // machines, each resumes with a changed card: ejected first, then inserted.
    void cardsCopyIdentity() {
        const auto smoke = qEnvironmentVariable("FROND_ARM_SESSIONS");
        if (smoke.isEmpty() || !QFileInfo(smoke + "/palm-tx").isDir()) QSKIP("Set FROND_ARM_SESSIONS to the smoke sessions");
        cardsReset();
        QSettings().clear(); QSettings().setValue("resume", false);
        qputenv("SDL_VIDEODRIVER", "dummy"); qputenv("SDL_AUDIODRIVER", "dummy");
        const auto a = cardsSessions() + "/Palm TX";
        QDir().mkpath(a);
        for (const auto& e : QDir(smoke + "/palm-tx").entryInfoList(QDir::Files)) {
            if (!frondSessions::packableName(e.fileName())) continue;
            QFile::copy(e.absoluteFilePath(), a + '/' + frondSessions::packableDestName(e.fileName(), "Palm TX"));
        }
        frondSessions::Preferences prefs; prefs.audio = false; prefs.peripheralsSet = true;
        frondSessions::writePreferences(a + "/Palm TX.frond", prefs);
        frondCards::Card card{frondCards::newId(), "Games", "sd", frondCards::now(), {}, qint64(32) << 20, frondCards::newIdentity(), false};
        const auto entry = cardsLibrary() + '/' + card.id;
        QVERIFY(QDir().mkpath(entry) && frondCards::writeCard(entry, card));
        QVERIFY(cardTool({"make", "sd", "32", entry + "/sd.frondsd"}));
        QVERIFY(frondCards::toMachine(cardsLibrary(), card.id, a).isEmpty());
        const auto core = cardsCore();
        // Each run: the sdState texts seen until it settles, then closed.
        auto run = [&](const QString& dir) {
            QStringList seen;
            MainWindow window(core);
            window.show();
            window.openSession(frondSessions::sessionFrondIn(dir), true);
            auto* state = window.findChild<QLabel*>("sdState");
            QTimer watch;
            QObject::connect(&watch, &QTimer::timeout, &window, [&] {
                if (seen.isEmpty() || seen.last() != state->text()) seen << state->text();
            });
            watch.start(5);
            QTest::qWaitFor([&] { return window.findChild<QPushButton*>("stop")->isEnabled(); }, 60000);
            QTest::qWaitFor([&] { return seen.contains("Inserted"); }, 10000);
            watch.stop();
            window.close();
            QTest::qWaitFor([&] { return !window.isVisible(); }, 120000);
            return seen;
        };
        auto seen = run(a);
        QVERIFY2(seen.contains("Inserted"), qPrintable(seen.join(", ")));
        QString name;
        QVERIFY(frondSessions::duplicate(cardsSessions(), frondSessions::describe(a), &name, {}, frondSessions::CardChoice::copy).isEmpty());
        const auto b = cardsSessions() + '/' + name;
        const auto copy = cardNamed("Games 2");
        QVERIFY(!copy.id.isEmpty() && copy.identity != card.identity && frondCards::readSlot(b).swap);
        seen = run(b);
        QVERIFY2(seen.contains("Inserted"), qPrintable(seen.join(", ")));
        // Written in both: a later generation in each file.
        for (const auto& dir : {a, b}) QVERIFY(setCardGeneration(dir + "/sd.frondsd", cardGeneration(dir + "/sd.frondsd") + 3));
        {
            frondCards::LibraryLock lock(cardsLibrary());
            QVERIFY(frondCards::toDrawer(cardsLibrary(), a, card.id).isEmpty());
            QVERIFY(frondCards::toDrawer(cardsLibrary(), b, copy.id).isEmpty());
            QVERIFY(frondCards::toMachine(cardsLibrary(), copy.id, a).isEmpty());
            QVERIFY(frondCards::toMachine(cardsLibrary(), card.id, b).isEmpty());
        }
        for (const auto& [dir, id] : {std::pair{a, copy.id}, std::pair{b, card.id}}) {
            QVERIFY(frondCards::launchArguments(cardsSessions(), dir).contains("--card-swap"));
            seen = run(dir);
            QVERIFY2(seen.contains("Ejected") && seen.indexOf("Ejected") < seen.lastIndexOf("Inserted"), qPrintable(seen.join(", ")));
            QCOMPARE(frondCards::readSlot(dir).card, id);
            QVERIFY(!frondCards::readSlot(dir).swap);
        }
        QSettings().clear();
    }
    // Part A: deleting a v6 machine keeps the card inside it as a drawer card.
    void cardsV6Delete() {
        cardsReset();
        auto v6 = [&](const QString& name, const QByteArray& sd) {
            const auto dir = cardsSessions() + '/' + name;
            QDir().mkpath(dir);
            QByteArray sections = QByteArray("meta") + QByteArray(1000, 'n') + QByteArray(3000, 'm') + QByteArray(77, 's');
            QByteArray head(40 + 32, '\0');
            putLe(head, 0, frondSessions::magic, 4); putLe(head, 4, 6, 4);
            for (int i = 0; i < 7; ++i) head[8 + i] = "palm-tx"[i];
            const quint32 toc[8]{0, 32, 4, 1000, 0, 3000, 77, quint32(sd.size())};
            for (int i = 0; i < 8; ++i) putLe(head, 40 + 4 * i, toc[i], 4);
            QByteArray body = sections + sd;
            uLongf size = compressBound(uLong(body.size()));
            QByteArray z(int(size), '\0');
            compress2(reinterpret_cast<Bytef*>(z.data()), &size, reinterpret_cast<const Bytef*>(body.constData()), uLong(body.size()), 1);
            z.resize(int(size));
            QFile f(dir + '/' + name + ".frond");
            f.open(QIODevice::WriteOnly);
            f.write(head + z);
            return dir;
        };
        QByteArray sd(32 << 20, '\0');
        for (int i = 0; i < sd.size(); i += 4096) sd[i] = char(i >> 12);
        const auto a = v6("Old TX", sd);
        QVERIFY(frondCards::v6HasCard(a + "/Old TX.frond"));
        QVERIFY(frondSessions::remove(frondSessions::describe(a)).isEmpty());
        QVERIFY(!QFileInfo::exists(a));
        const auto card = cardNamed("Old TX card");
        QVERIFY(!card.id.isEmpty() && card.machine.isEmpty() && card.kind == "sd");
        QCOMPARE(card.bytes, qint64(sd.size()));
        QCOMPARE(card.identity, QCryptographicHash::hash(sd, QCryptographicHash::Md5).toHex());
        QTemporaryDir out(QDir::currentPath() + "/build/cards-v6-XXXXXX");
        QVERIFY(frondCards::exportCardImage(cardsSessions(), card.id, out.path() + "/old.img").isEmpty());
        QFile img(out.path() + "/old.img"); QVERIFY(img.open(QIODevice::ReadOnly));
        QVERIFY(img.readAll() == sd);
        // No card inside: nothing kept.
        const auto b = v6("Empty TX", {});
        QVERIFY(frondSessions::remove(frondSessions::describe(b)).isEmpty());
        QCOMPARE(frondCards::entries(cardsLibrary()).size(), 1);
        // A cut-off file: refused, left as it is.
        const auto c = v6("Broken TX", sd);
        QFile cut(c + "/Broken TX.frond"); QVERIFY(cut.open(QIODevice::ReadWrite)); QVERIFY(cut.resize(cut.size() / 2)); cut.close();
        const auto error = frondSessions::remove(frondSessions::describe(c));
        QVERIFY2(error.endsWith("It wasn't deleted."), qPrintable(error));
        QVERIFY(QFileInfo(c + "/Broken TX.frond").isFile());
        QCOMPARE(frondCards::entries(cardsLibrary()).size(), 1);
        QVERIFY(QDir(cardsLibrary()).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).filter(".new-").isEmpty());
        // Two v6 machines duplicated from each other hold the same card: the second one kept gets
        // a new serial.
        QVERIFY(frondCards::deleteCard(cardsSessions(), card.id).isEmpty());
        const auto twinA = v6("Twin A", sd), twinB = v6("Twin B", sd);
        QVERIFY(frondSessions::remove(frondSessions::describe(twinA)).isEmpty());
        QVERIFY(frondSessions::remove(frondSessions::describe(twinB)).isEmpty());
        const auto first = cardNamed("Twin A card"), second = cardNamed("Twin B card");
        QVERIFY(!first.id.isEmpty() && !second.id.isEmpty());
        QCOMPARE(first.identity, card.identity);
        QVERIFY(second.identity != card.identity);
    }
    // Part A: Save a Copy carries slot.json as Export does, and the copy imports with its own card.
    void cardsSaveCopy() {
        cardsReset();
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files, out(QDir::currentPath() + "/build/cards-savecopy-XXXXXX");
        const auto core = pickerCore(files.path(), "palm-tx");
        const auto a = cardsMachine("Palm TX", true);
        frondCards::reconcile(cardsSessions());
        const auto card = onlyCard();
        {
            MainWindow window(core);
            window.show();
            window.openSession(a + "/Palm TX.frond", true);
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("stop")->isEnabled(), 10000);
            window.saveCopyTo(out.path() + "/Trip.frond");
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(out.path() + "/Trip/Trip.frond").isFile() && QFileInfo(out.path() + "/Trip/slot.json").isFile(), 10000);
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 10000);
        }
        const auto slot = frondCards::readSlot(out.path() + "/Trip");
        QVERIFY(slot.present && slot.card == card.id && !slot.swap);
        QVERIFY(QFileInfo(out.path() + "/Trip/sd.frondsd").isFile());
        QString name;
        QVERIFY(frondSessions::importFile(cardsSessions(), out.path() + "/Trip", &name).isEmpty());
        const auto imported = cardNamed("Palm TX card 2");
        QVERIFY(!imported.id.isEmpty() && imported.identity != card.identity);
        QCOMPARE(frondCards::readSlot(cardsSessions() + '/' + name).card, imported.id);
        QCOMPARE(frondCards::readSlot(a).card, card.id);
        QSettings().clear();
    }
    // The Cards window: rows, the row menu and New card.
    // Import folder as card: a folder with nested folders and long names becomes a card whose files
    // match, the smallest size is chosen, a folder that is too big or has unusable names is refused,
    // and Cancel leaves nothing.
    void importFolderCard() {
        cardsReset();
        if (QStandardPaths::findExecutable("mcopy").isEmpty()) QSKIP("mtools isn't installed (the test reads the card back with it)");
        QTemporaryDir work(QDir::currentPath() + "/build/cards-folder-XXXXXX");
        const auto folder = work.filePath("My Music Collection");
        auto put = [&](const QString& rel, const QByteArray& bytes) {
            QDir().mkpath(QFileInfo(folder + '/' + rel).absolutePath());
            QFile f(folder + '/' + rel);
            QVERIFY(f.open(QIODevice::WriteOnly));
            QVERIFY(f.write(bytes) == bytes.size());
        };
        QByteArray big(3 << 20, '\0');
        for (int i = 0; i < big.size(); i += 4096) big[i] = char(i >> 12);
        put("A rather long file name for a card.txt", "hello");
        put("empty", {});
        put("Songs/Album One/01 - A very long track title indeed.sunvox", big);
        put("Songs/Album One/notes.txt", "x");
        put("Songs/Album Two/Deeper/Deepest/readme.md", "nested");
        // Names past ASCII come back exactly, short ones and lower-case ones too.
        const QStringList names{"äpfel.txt", "ÄB.txt", "naïve café.txt", "Songs/émile.TXT", "日本語.md", "Ölfarben/ñ", "README.TXT"};
        for (const auto& n : names) put(n, n.toUtf8());
        const auto plan = frondFolderCard::measure(folder);
        QVERIFY2(plan.problem.isEmpty(), qPrintable(plan.problem));
        QCOMPARE(plan.files, 12);
        QCOMPARE(plan.folders, 6);
        QCOMPARE(frondFolderCard::smallestFit(plan, frondCards::sizesFor("sd")), 32);
        QString id;
        QList<qint64> seen;
        const auto error = frondCards::importFolderCard(cardsSessions(), folder, "My Music", "sd", 32,
                                                        [&](qint64 d, qint64 t) { seen << d * 1000 / qMax<qint64>(t, 1); return true; }, &id);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(seen.size() > 2 && seen.last() >= 900);
        const auto card = cardNamed("My Music");
        QCOMPARE(card.id, id);
        QCOMPARE(card.kind, QString("sd"));
        QCOMPARE(card.bytes, qint64(32) << 20);
        QVERIFY(QDir(cardsLibrary()).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).filter(".new-").isEmpty());
        // Back out as an image, and the files read off it are the folder's.
        QVERIFY(frondCards::exportCardImage(cardsSessions(), card.id, work.filePath("out.img")).isEmpty());
        QDir().mkpath(work.filePath("back"));
        {
            // mtools, a reader of its own, at the partition `frond --card make` puts at sector 63.
            QProcess mcopy;
            auto env = QProcessEnvironment::systemEnvironment();
            env.insert("MTOOLSRC", "/dev/null");
            env.insert("LC_ALL", "C.UTF-8");
            mcopy.setProcessEnvironment(env);
            mcopy.start("mcopy", {"-i", work.filePath("out.img") + "@@" + QString::number(63 * 512), "-s", "-m", "::/*", work.filePath("back")});
            QVERIFY(mcopy.waitForFinished(60000) && mcopy.exitCode() == 0);
        }
        QVERIFY(frondFolderCard::sameTree(folder, work.filePath("back")));
        // Dates come from the folder.
        QCOMPARE(QFileInfo(work.filePath("back/Songs/Album One/notes.txt")).lastModified().toSecsSinceEpoch() / 2,
                 QFileInfo(folder + "/Songs/Album One/notes.txt").lastModified().toSecsSinceEpoch() / 2);
        for (const auto& n : names) QVERIFY2(QFileInfo(work.filePath("back/" + n)).isFile(), qPrintable(n));
        // The short names are Android's FatImage ones (FolderCardTest checks the same).
        QCOMPARE(frondFolderCard::shortName("äpfel.txt", {}), QByteArray("_PFEL~1 TXT"));
        QCOMPARE(frondFolderCard::shortName("README.TXT", {}), QByteArray("README  TXT"));
        QCOMPARE(frondFolderCard::shortName("notes.txt", {"NOTES.TXT"}), QByteArray("NOTES~1 TXT"));
        QVERIFY(QFileInfo(work.filePath("back/Songs/Album Two/Deeper/Deepest/readme.md")).isFile());
        QVERIFY(QFileInfo(work.filePath("back/Songs/Album One/01 - A very long track title indeed.sunvox")).size() == big.size());
        // The same card is a name already taken; a classic stick is not offered for a folder.
        QVERIFY(frondCards::importFolderCard(cardsSessions(), folder, "My Music", "sd", 32).contains("already a card"));
        QVERIFY(!frondCards::folderKinds().contains("ms"));
        QVERIFY(frondCards::importFolderCard(cardsSessions(), folder, "Stick", "ms", 16).contains("Choose"));
        // Too big for 32 MB: refused with nothing left; the next size up takes it.
        put("Songs/huge.bin", QByteArray(40 << 20, 'z'));
        const auto bigPlan = frondFolderCard::measure(folder);
        QCOMPARE(frondFolderCard::smallestFit(bigPlan, frondCards::sizesFor("sd")), 64);
        {
            FolderCardDialog dialog(cardsSessions(), folder, bigPlan);
            QCOMPARE(dialog.name->text(), QString("My Music Collection"));
            QCOMPARE(dialog.type->itemText(0), QString("SD card"));
            QVERIFY(dialog.type->findText("Memory Stick") < 0);
            QCOMPARE(dialog.sizes->itemText(0), QString("32 MB · Too small"));
            QVERIFY(!qobject_cast<QStandardItemModel*>(dialog.sizes->model())->item(0)->isEnabled());
            QCOMPARE(dialog.sizes->currentText(), QString("64 MB"));
            QCOMPARE(dialog.size(), 64);
        }
        const auto refused = frondCards::importFolderCard(cardsSessions(), folder, "Too big", "sd", 32);
        QVERIFY2(refused.contains("doesn't fit"), qPrintable(refused));
        QVERIFY(cardNamed("Too big").id.isEmpty());
        QVERIFY(QDir(cardsLibrary()).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).filter(".new-").isEmpty());
        QCOMPARE(frondCards::entries(cardsLibrary()).size(), 1);
        // Cancel at the start, while the files are going in, and while the card file is made.
        auto nothingLeft = [&] {
            QVERIFY(cardNamed("Stopped").id.isEmpty());
            QVERIFY(QDir(cardsLibrary()).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).filter(".new-").isEmpty());
            QCOMPARE(frondCards::entries(cardsLibrary()).size(), 1);
        };
        QCOMPARE(frondCards::importFolderCard(cardsSessions(), folder, "Stopped", "sd", 64, [](qint64, qint64) { return false; }),
                 QString("Cancelled."));
        nothingLeft();
        QCOMPARE(frondCards::importFolderCard(cardsSessions(), folder, "Stopped", "sd", 64,
                                              [](qint64 d, qint64 t) { return d * 1000 / qMax<qint64>(t, 1) < 500; }),
                 QString("Cancelled."));
        nothingLeft();
        {
            SlowCardTool slow;
            QCOMPARE(frondCards::importFolderCard(cardsSessions(), folder, "Stopped", "sd", 64,
                                                  [](qint64 d, qint64 t) { return d * 1000 / qMax<qint64>(t, 1) < 2; }),
                     QString("Cancelled."));
            nothingLeft();
        }
        QVERIFY(frondCards::importFolderCard(cardsSessions(), folder, "Larger", "sd", 64).isEmpty());
        QCOMPARE(cardNamed("Larger").bytes, qint64(64) << 20);
        // Names a card can't hold, names that differ only in case, and too many names at the top.
        QTemporaryDir odd(QDir::currentPath() + "/build/cards-folder-XXXXXX");
        QFile(odd.filePath("Readme.txt")).open(QIODevice::WriteOnly);
        QFile(odd.filePath("README.TXT")).open(QIODevice::WriteOnly);
        QVERIFY(frondFolderCard::measure(odd.path()).problem.contains("differ only in case"));
        QFile(odd.filePath("README.TXT")).remove();
        QFile(odd.filePath("what?.txt")).open(QIODevice::WriteOnly);
        QVERIFY(frondFolderCard::measure(odd.path()).problem.contains("can't be used"));
        QFile(odd.filePath("what?.txt")).remove();
        for (int i = 0; i < 520; ++i) QFile(odd.filePath(QString("file-%1.txt").arg(i))).open(QIODevice::WriteOnly);
        QVERIFY(frondFolderCard::measure(odd.path()).problem.contains("too many names"));
        QVERIFY(frondCards::importFolderCard(cardsSessions(), odd.path(), "Odd", "sd", 32).contains("too many names"));
        QCOMPARE(frondCards::entries(cardsLibrary()).size(), 2);
    }
    // While a stopped machine exports, Cards greys its card as for a running machine, and gives
    // it back after.
    void exportGreysCards() {
        cardsReset();
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files, out(QDir::currentPath() + "/build/export-greys-XXXXXX");
        const auto core = fakeCardCore(files.path());
        cardsMachine("Palm TX", true);
        frondCards::reconcile(cardsSessions());
        MainWindow window(core);
        window.show();
        auto* cards = window.showCards();
        auto* sd = cards->findChild<QListWidget*>("sdCards");
        QCOMPARE(rowLabel(sd, 0, "cardName"), QString("Palm TX card"));
        const QStringList media{"cardFiles", "duplicateCard", "exportCard", "takeOutCard"};
        for (const auto& a : media) QVERIFY2(cardAction(sd, 0, a.toLatin1())->isEnabled(), qPrintable(a));
        std::atomic<bool> entered{false}, release{false};
        window.exportHook = [&] {
            entered = true;
            while (!release) QThread::msleep(5);
        };
        QStringList stillEnabled{"none seen"};
        QTimer poll;
        poll.setInterval(10);
        connect(&poll, &QTimer::timeout, &window, [&] {
            if (!entered || release) return;
            stillEnabled.clear();
            for (const auto& a : media)
                if (cardAction(sd, 0, a.toLatin1())->isEnabled()) stillEnabled << a;
            release = true;
        });
        poll.start();
        whenShown<QFileDialog>([&](QFileDialog* d) {
            d->setDirectory(out.path());
            d->selectFile("Palm TX.frond");
            QMetaObject::invokeMethod(d, "accept");
        });
        auto* list = window.findChild<QListWidget*>("sessionList");
        QCOMPARE(rowTexts(window, "sessionName"), QStringList({"Palm TX"}));
        auto* menu = list->itemWidget(list->item(0))->findChild<QPushButton*>("rowMenu")->menu();
        for (auto* a : menu->actions()) if (a->objectName() == "exportSession") a->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(window.statusBar()->currentMessage().startsWith("Session exported to"), 20000);
        QCOMPARE(stillEnabled, QStringList());
        QVERIFY(QFileInfo(out.filePath("Palm TX/sd.frondsd")).isFile());
        for (const auto& a : media) QVERIFY2(cardAction(sd, 0, a.toLatin1())->isEnabled(), qPrintable(a));
        window.close();
        QSettings().clear();
    }
    void cardsWindow() {
        cardsReset();
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files, out(QDir::currentPath() + "/build/cards-window-XXXXXX");
        const auto core = fakeCardCore(files.path());
        cardsMachine("Palm TX", true);
        const auto spare = cardsMachine("Spare Palm TX", false);
        const auto e2 = cardsMachine("Tungsten E2", false, "palm-e2");
        frondCards::reconcile(cardsSessions());
        QString games;
        QVERIFY(frondCards::makeCard(cardsSessions(), "Games", "sd", 64, {}, &games).isEmpty());
        QVERIFY(frondCards::setLinks(cardsSessions(), games, {spare, e2}).isEmpty());
        const auto kinds = frondCards::kindsTaken();
        if (kinds.contains("ms")) QVERIFY(frondCards::makeCard(cardsSessions(), "Photos", "ms", 16).isEmpty());
        MainWindow window(core);
        window.show();
        auto* cards = window.showCards();
        QVERIFY(cards && cards->isVisible());
        QCOMPARE(window.findChild<QAction*>("cardsAction")->text(), QString("C&ards…"));
        auto* sd = cards->findChild<QListWidget*>("sdCards");
        QCOMPARE(sd->count(), 2);
        QCOMPARE(rowLabel(sd, 0, "cardName"), QString("Games"));
        QCOMPARE(rowLabel(sd, 0, "cardDetail"), QString("64 MB · Free · Linked to Spare Palm TX, Tungsten E2"));
        QCOMPARE(rowLabel(sd, 1, "cardName"), QString("Palm TX card"));
        QCOMPARE(rowLabel(sd, 1, "cardDetail"), QString("32 MB · In Palm TX"));
        // The marker is drawn lit (highlight colour) only on the card that's in a machine.
        auto* litMarker = sd->itemWidget(sd->item(1))->findChild<QFrame*>("cardMarker");
        auto* darkMarker = sd->itemWidget(sd->item(0))->findChild<QFrame*>("cardMarker");
        QVERIFY(litMarker->property("lit").toBool());
        QVERIFY(!darkMarker->property("lit").toBool());
        QTRY_VERIFY(litMarker->height() > 0);
        const auto highlight = litMarker->palette().color(QPalette::Highlight);
        QCOMPARE(litMarker->grab().toImage().pixelColor(1, litMarker->height() / 2).rgb(), highlight.rgb());
        QVERIFY(darkMarker->grab().toImage().pixelColor(1, darkMarker->height() / 2).rgb() != highlight.rgb());
        // A narrow window wraps the linked machines rather than cutting them off.
        cards->resize(260, cards->height());
        QTRY_VERIFY([&] {
            auto* detail = sd->itemWidget(sd->item(0))->findChild<QLabel*>("cardDetail");
            return detail->width() < 260 && detail->heightForWidth(detail->width()) <= detail->height() &&
                   sd->visualItemRect(sd->item(0)).height() >= detail->height();
        }());
        cards->resize(520, cards->height());
        if (kinds.contains("ms")) {
            auto* ms = cards->findChild<QListWidget*>("memorySticks");
            QCOMPARE(ms->count(), 1);
            QCOMPARE(rowLabel(ms, 0, "cardDetail"), QString("16 MB · Not in a machine"));
        }
        screenshot(cards, "cards-window.png");
        // In a stopped machine: Delete greyed, Take out of it there.
        QVERIFY(!cardAction(sd, 1, "deleteCard")->isEnabled());
        QCOMPARE(cardAction(sd, 1, "takeOutCard")->text(), QString("Take out of Palm TX"));
        QVERIFY(!cardAction(sd, 0, "takeOutCard"));
        cardAction(sd, 1, "takeOutCard")->trigger();
        const auto txCard = cardNamed("Palm TX card");
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(cardsLibrary() + '/' + txCard.id + "/sd.frondsd").isFile(), 10000);
        QTRY_COMPARE(rowLabel(sd, 1, "cardDetail"), QString("32 MB · Not in a machine"));
        QVERIFY(frondCards::readSlot(cardsSessions() + "/Palm TX").swap);
        // Rename.
        typeInto("Trip");
        cardAction(sd, 1, "renameCard")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(cardNamed("Trip").id, txCard.id, 5000);
        // Duplicate: a new serial.
        cardAction(sd, 0, "duplicateCard")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!cardNamed("Games 2").id.isEmpty(), 10000);
        QVERIFY(cardNamed("Games 2").identity != cardNamed("Games").identity);
        QTRY_COMPARE(sd->count(), 3);
        // Export image.
        // The dialog suggests the card's name; saved in another folder.
        QString suggested;
        whenShown<QFileDialog>([&](QFileDialog* d) {
            suggested = QFileInfo(d->selectedFiles().value(0)).fileName();
            d->setDirectory(out.path());
            d->selectFile("Games.img");
            QMetaObject::invokeMethod(d, "accept");
        });
        QVERIFY(cardAction(sd, 0, "exportCard")->isEnabled());
        cardAction(sd, 0, "exportCard")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(QFileInfo(out.path() + "/Games.img").size(), qint64(64) << 20, 20000);
        QCOMPARE(suggested, QString("Games.img"));
        // Delete, after the confirmation.
        answerYes();
        cardAction(sd, 1, "deleteCard")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(cardNamed("Games 2").id.isEmpty(), 5000);
        QTRY_COMPARE(sd->count(), 2);
        // New card: every type some device takes, sizes smallest first.
        bool shown = false;
        QStringList types, sizes;
        whenShown<NewCardDialog>([&](NewCardDialog* d) {
            shown = true;
            for (int i = 0; i < d->type->count(); ++i) types << d->type->itemText(i);
            d->type->setCurrentIndex(d->type->findText("SD card"));
            for (int i = 0; i < d->sizes->count(); ++i) sizes << d->sizes->itemText(i);
            QCOMPARE(d->sizes->currentText(), QString("128 MB"));
            d->name->setText("Music");
            screenshot(d, "new-card.png");
            d->accept();
        });
        QTimer::singleShot(0, cards->findChild<QPushButton*>("newCard"), &QPushButton::click);
        QTRY_VERIFY_WITH_TIMEOUT(cardNamed("Music").bytes == qint64(128) << 20, 20000);
        QVERIFY(shown);
        QCOMPARE(types.first(), QString("SD card"));
        QCOMPARE(sizes, QStringList({"32 MB", "64 MB", "128 MB", "256 MB", "512 MB", "1 GB", "2 GB"}));
        if (kinds.contains("ms")) QVERIFY(types.contains("Memory Stick"));
        if (kinds.contains("ms-pro")) QCOMPARE(types.last(), QString("Memory Stick Pro"));
        cards->close();
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 10000);
        QSettings().clear();
    }
    // The running card picker: cards the machine takes, greyed when in another machine or too big;
    // a card put in and taken out again through the core.
    void cardPicker() {
        cardsReset();
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        const auto core = pickerCore(files.path(), "palm-e2");
        cardsMachine("Palm TX", true);
        const auto e2 = cardsMachine("Tungsten", false, "palm-e2");
        frondCards::reconcile(cardsSessions());
        QString games;
        QVERIFY(frondCards::makeCard(cardsSessions(), "Games", "sd", 64, {}, &games).isEmpty());
        sparseCard("Big", "sd", qint64(2048) << 20);
        MainWindow window(core);
        window.show();
        window.openSession(e2 + "/Tungsten.frond", true);
        auto* state = window.findChild<QLabel*>("sdState");
        auto* pick = window.findChild<QPushButton*>("cardPick");
        QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("No card"), 10000);
        showCardPanel(pick); QTRY_VERIFY(pick->isVisible() && pick->isEnabled());
        QCOMPARE(pick->text(), QString("No card"));
        QVERIFY(!state->isVisible());
        QStringList texts, enabled;
        whenShown<CardPickerDialog>([&](CardPickerDialog* d) {
            QCOMPARE(d->windowTitle(), QString("SD card for Tungsten"));
            for (auto* r : d->findChildren<QRadioButton*>("cardChoice")) {
                texts << r->text();
                if (r->isEnabled()) enabled << r->text();
                if (r->property("cardId").toString() == games) r->setChecked(true);
            }
            screenshot(d, "card-picker.png");
            d->accept();
        });
        QTimer::singleShot(0, pick, &QPushButton::click);
        QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Inserted"), 10000);
        QCOMPARE(texts, QStringList({"No card", "Big · 2 GB · Too big for Tungsten E2", "Games · 64 MB", "Palm TX card · 32 MB · In Palm TX"}));
        QCOMPARE(enabled, QStringList({"No card", "Games · 64 MB"}));
        QCOMPARE(pick->text(), QString("Games"));
        QVERIFY(QFileInfo(e2 + "/sd.frondsd").isFile());
        QCOMPARE(frondCards::readSlot(e2).card, games);
        // No card: eject after the confirmation, take out, back to the drawer.
        whenShown<CardPickerDialog>([&](CardPickerDialog* d) {
            for (auto* r : d->findChildren<QRadioButton*>("cardChoice"))
                if (r->property("cardId").toString().isEmpty()) r->setChecked(true);
            QTimer::singleShot(0, d, &QDialog::accept);
            answerYes();
        });
        QTimer::singleShot(0, pick, &QPushButton::click);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(cardsLibrary() + '/' + games + "/sd.frondsd").isFile(), 10000);
        QTRY_COMPARE(pick->text(), QString("No card"));
        QVERIFY(frondCards::cardFileIn(e2).isEmpty());
        QFile trace(files.path() + "/commands"); QVERIFY(trace.open(QIODevice::ReadOnly));
        const auto commands = trace.readAll();
        QVERIFY(commands.indexOf("sd-put-in\t") < commands.indexOf("sd-eject\t"));
        QVERIFY(commands.indexOf("sd-eject\t") < commands.indexOf("sd-take-out\t"));
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 10000);
        QSettings().clear();
    }
    // Link to machines, and a running linked machine's control when its card comes free.
    void cardLink() {
        cardsReset();
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        const auto core = pickerCore(files.path(), "palm-tx");
        const auto holder = cardsMachine("Holder", true), a = cardsMachine("Spare", false), e2 = cardsMachine("Tungsten", false, "palm-e2");
        frondCards::reconcile(cardsSessions());
        QString games;
        QVERIFY(frondCards::makeCard(cardsSessions(), "Games", "sd", 64, {}, &games).isEmpty());
        const auto big = sparseCard("Big", "sd", qint64(2048) << 20);
        {
            LinkDialog dialog(cardsSessions(), games, {}, {});
            QCOMPARE(dialog.windowTitle(), QString("Link Games to machines"));
            QStringList texts;
            for (int i = 0; i < dialog.machines->count(); ++i) texts << dialog.machines->item(i)->text();
            QCOMPARE(texts, QStringList({"Holder · Palm TX · Has Holder card, take it out first", "Spare · Palm TX", "Tungsten · Tungsten E2"}));
            QVERIFY(!(dialog.machines->item(0)->flags() & Qt::ItemIsEnabled));
            dialog.machines->item(1)->setCheckState(Qt::Checked);
            dialog.machines->item(2)->setCheckState(Qt::Checked);
            QVERIFY(frondCards::setLinks(cardsSessions(), games, dialog.linked()).isEmpty());
        }
        QCOMPARE(frondCards::readSlot(a).link, games);
        QCOMPARE(frondCards::readSlot(e2).link, games);
        {
            LinkDialog dialog(cardsSessions(), big, {}, {});
            QCOMPARE(dialog.machines->item(2)->text(), QString("Tungsten · Tungsten E2 · Too big for Tungsten E2"));
            QVERIFY(!(dialog.machines->item(2)->flags() & Qt::ItemIsEnabled));
            QCOMPARE(dialog.machines->item(1)->text(), QString("Spare · Palm TX · Linked to Games"));
            QCOMPARE(dialog.machines->item(1)->checkState(), Qt::Unchecked);
        }
        // Holder holds the card's partner; Spare runs linked to Games, which another machine has.
        {
            frondCards::LibraryLock lock(cardsLibrary());
            QVERIFY(frondCards::toDrawer(cardsLibrary(), holder, cardNamed("Holder card").id).isEmpty());
            QVERIFY(frondCards::toMachine(cardsLibrary(), games, holder).isEmpty());
        }
        MainWindow window(core);
        window.show();
        window.openSession(a + "/Spare.frond", true);
        auto* linked = window.findChild<QLabel*>("linkedState");
        auto* insert = window.findChild<QPushButton*>("linkedInsert");
        QTRY_COMPARE_WITH_TIMEOUT(linked->text(), QString("Games is in Holder"), 10000);
        QVERIFY(!insert->isVisible());
        QVERIFY(frondCards::takeOutStopped(cardsSessions(), games).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(linked->text(), QString("Games is free"), 10000);
        showCardPanel(insert); QTRY_VERIFY(insert->isVisible() && insert->isEnabled());
        screenshot(&window, "card-linked.png");
        insert->click();
        QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLabel*>("sdState")->text(), QString("Inserted"), 10000);
        QVERIFY(QFileInfo(a + "/sd.frondsd").isFile());
        QCOMPARE(window.findChild<QPushButton*>("cardPick")->text(), QString("Games"));
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 10000);
        QSettings().clear();
    }
    // Files… on the running machine's card greys its row menu in Cards and the picker, while it is
    // pending (eject and save) and while Files is open.
    void cardsExchangeGreying() {
        cardsReset();
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        const auto core = pickerCore(files.path(), "palm-tx");
        { QFile slow(files.filePath("slow-save")); QVERIFY(slow.open(QIODevice::WriteOnly)); }
        const auto a = cardsMachine("Palm TX", true);
        frondCards::reconcile(cardsSessions());
        MainWindow window(core);
        window.show();
        window.openSession(a + "/Palm TX.frond", true);
        auto* pick = window.findChild<QPushButton*>("cardPick");
        auto* action = window.findChild<QPushButton*>("sdExchange");
        QTRY_VERIFY_WITH_TIMEOUT(pick->isEnabled() && action->isEnabled(), 10000);
        auto* cards = window.showCards();
        auto* sd = cards->findChild<QListWidget*>("sdCards");
        QCOMPARE(sd->count(), 1);
        // The menu as it opens.
        auto enabled = [&](const char* name) {
            auto* menu = sd->itemWidget(sd->item(0))->findChild<QPushButton*>("cardMenu")->menu();
            emit menu->aboutToShow();
            auto* a = menu->findChild<QAction*>(name);
            return a && a->isEnabled();
        };
        const QList<const char*> names{"renameCard", "linkCard", "takeOutCard"};
        for (const auto* name : names) QVERIFY2(enabled(name), name);
        QStringList failures;
        bool sawPending = false, sawOpen = false;
        auto check = [&](const QString& when) {
            if (pick->isEnabled()) failures << when + ": picker enabled";
            for (const auto* name : names)
                if (enabled(name)) failures << when + ": " + name + " enabled";
            cards->refresh();
            for (const auto* name : names)
                if (enabled(name)) failures << when + ": " + name + " enabled after refresh";
        };
        QTimer poll; poll.setInterval(10);
        connect(&poll, &QTimer::timeout, &window, [&] {
            if (auto* box = window.findChild<QMessageBox*>(); box && box->isVisible() && box->windowTitle().contains("Open the card")) {
                box->button(QMessageBox::Yes)->click();
                return;
            }
            if (auto* dialog = window.findChild<QDialog*>("cardFilesWindow"); dialog && dialog->isVisible()) {
                if (!sawOpen) { sawOpen = true; check("open"); }
                dialog->reject();
                return;
            }
            QFile trace(files.filePath("commands"));
            if (sawPending || !trace.open(QIODevice::ReadOnly)) return;
            const auto commands = trace.readAll();
            if (commands.contains("save\t") && !commands.contains("sd-take-out")) { sawPending = true; check("pending"); }
        });
        poll.start();
        action->click();
        QTRY_VERIFY_WITH_TIMEOUT(sawOpen, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(action->isEnabled(), 10000);
        poll.stop();
        QVERIFY(sawPending);
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join("; ")));
        // Back to normal afterwards.
        QVERIFY(pick->isEnabled());
        for (const auto* name : names) QVERIFY2(enabled(name), name);
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 10000);
        QSettings().clear();
    }
    // Files… in Cards: a free card and a stopped machine's card open in Files and change in place;
    // the running machine's card is greyed In use. A stopped machine's card is marked swapped.
    void cardsFilesGating() {
        cardsReset();
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        const auto core = pickerCore(files.path(), "palm-tx");
        const auto runner = cardsMachine("Runner", true), stopped = cardsMachine("Stopped", true), empty = cardsMachine("Empty", false);
        frondCards::reconcile(cardsSessions());
        QString free;
        QVERIFY(frondCards::makeCard(cardsSessions(), "Free", "sd", 32, {}, &free).isEmpty());
        QFile host(files.filePath("note.txt")); QVERIFY(host.open(QIODevice::WriteOnly)); host.write("note"); host.close();
        MainWindow window(core);
        window.show();
        window.openSession(runner + "/Runner.frond", true);
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("sdExchange")->isEnabled(), 10000);
        auto* cards = window.showCards();
        auto* sd = cards->findChild<QListWidget*>("sdCards");
        auto row = [&](const QString& name) {
            for (int i = 0; i < sd->count(); ++i)
                if (rowLabel(sd, i, "cardName") == name) return i;
            return -1;
        };
        auto filesAction = [&](const QString& name) {
            auto* menu = sd->itemWidget(sd->item(row(name)))->findChild<QPushButton*>("cardMenu")->menu();
            emit menu->aboutToShow();
            return menu->findChild<QAction*>("cardFiles");
        };
        QCOMPARE(sd->count(), 3);
        QCOMPARE(filesAction("Runner card")->text(), QString("Files… · In use"));
        QVERIFY(!filesAction("Runner card")->isEnabled());
        QCOMPARE(filesAction("Stopped card")->text(), QString("Files…"));
        QVERIFY(filesAction("Stopped card")->isEnabled() && filesAction("Free")->isEnabled());
        auto edit = [&](const QString& name, const QString& path) {
            const auto generation = cardGeneration(path);
            bool done = false;
            QTimer poll; poll.setInterval(10);
            connect(&poll, &QTimer::timeout, cards, [&] {
                auto* w = cards->findChild<CardFilesWindow*>("cardFilesWindow");
                if (!w || !w->isVisible() || done) return;
                done = true;
                // Open in Files: the card can't be opened again or moved.
                QVERIFY(frondCardFile::inUse(path));
                w->addPaths({host.fileName()});
                w->doneButton->click();
            });
            poll.start();
            filesAction(name)->trigger();
            QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
            QTRY_VERIFY_WITH_TIMEOUT(!cards->findChild<CardFilesWindow*>("cardFilesWindow"), 5000);
            poll.stop();
            QVERIFY(!frondCardFile::inUse(path));
            QCOMPARE(*frondCardFile::verify(path), generation + 1);
            auto c = frondCardFile::CardFile::open(path, false);
            frondCardFile::FatImage fat(*c);
            QVERIFY(fat.find("/note.txt"));
        };
        edit("Free", cardsLibrary() + '/' + free + "/sd.frondsd");
        QVERIFY(!frondCards::readSlot(stopped).swap);
        edit("Stopped card", stopped + "/sd.frondsd");
        QVERIFY(frondCards::readSlot(stopped).swap);
        // A classic Memory Stick has Files… too.
        {
            sparseCard("Stick", "ms", 4 << 20);
            cards->refresh();
            auto* sticks = cards->findChild<QListWidget*>("memorySticks");
            QCOMPARE(sticks->count(), 1);
            auto* menu = sticks->itemWidget(sticks->item(0))->findChild<QPushButton*>("cardMenu")->menu();
            emit menu->aboutToShow();
            QVERIFY(menu->findChild<QAction*>("cardFiles") && menu->findChild<QAction*>("cardFiles")->isEnabled());
            QVERIFY(menu->findChild<QAction*>("renameCard"));
        }
        // A card open in Files can't be taken out or deleted.
        {
            auto c = frondCardFile::CardFile::open(cardsLibrary() + '/' + free + "/sd.frondsd", true);
            {
                frondCards::LibraryLock lock(cardsLibrary());
                QCOMPARE(frondCards::toMachine(cardsLibrary(), free, empty), QString("The card is open in Files. Close it first."));
            }
            QCOMPARE(frondCards::deleteCard(cardsSessions(), free), QString("The card is open in Files. Close it first."));
        }
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 10000);
        QSettings().clear();
    }
    // Pairing by an IPv6 loopback address.
    void linkPairingIpv6() {
        QTcpServer probe;
        if (!probe.listen(QHostAddress::LocalHostIPv6, 0)) QSKIP("No IPv6 loopback here");
        probe.close();
        QTemporaryDir dir(QDir::currentPath() + "/build/lk6-XXXXXX");
        TestHub ha(dir.path(), "a", freePort()), hb(dir.path(), "b", freePort());
        HubUser a(ha), b(hb);
        a.client.send({{"op", "rename"}, {"name", "Six"}});
        b.client.setSticky("watch", {{"op", "watch"}});
        QTRY_VERIFY_WITH_TIMEOUT(!a.last("hello").isEmpty() && !b.last("hello").isEmpty(), 10000);
        b.got.clear(); a.got.clear();
        a.client.send({{"op", "pair"}, {"address", "[::1]:" + QString::number(b.client.hello().value("port").toInt())}});
        QTRY_VERIFY_WITH_TIMEOUT(!a.last("pair-peer").isEmpty() && !b.last("pair-request").isEmpty(), 10000);
        a.client.send({{"op", "pair-code"}, {"code", b.last("pair-request").value("code").toString().remove(' ')}});
        QTRY_VERIFY_WITH_TIMEOUT(a.last("pair-result").value("result") == "paired" && b.last("pair-request-end").value("result") == "paired", 10000);
        QVERIFY(!storedPeer(ha, b.client.hello().value("id").toString()).isEmpty());
        QCOMPARE(storedPeer(ha, b.client.hello().value("id").toString()).value("address").toString(), QString("[::1]:%1").arg(b.client.hello().value("port").toInt()));
        killHub(a); killHub(b);
    }
    // The Licences text lists every desktop component with its notice.
    void licencesNotices() {
        const QString text = thirdPartyNotices();
        for (const char* name : {"argparse", "Monocypher", "Dynarmic", "Zydis", "xbyak", "BlueZ", "Permission is hereby granted"})
            QVERIFY2(text.contains(name), name);
        QVERIFY(!text.contains("SDL2_image"));
        QVERIFY(text.size() > 10000);
    }
};
int main(int argc, char** argv) {
    // Isolate both settings and automatic session files from the real user's desktop.
    QDir().mkpath("build");
    QTemporaryDir directory(QDir::currentPath() + "/build/gui-workflow-XXXXXX");
    QDir().mkpath(directory.path() + "/data-real");
    if (!QFile::link(directory.path() + "/data-real", directory.path() + "/data")) return 1;
    qputenv("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    qputenv("XDG_DATA_HOME", (directory.path() + "/data").toUtf8());
    // The link hub a window starts runs here too, on its own ports.
    // A unix socket path is limited to 107 characters, so the runtime folder is a short one under
    // the system temp folder.
    QTemporaryDir runtime(QDir::tempPath() + "/fr-XXXXXX");
    QFile::setPermissions(runtime.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
    qputenv("FROND_LINK_PORT", "0"); qputenv("FROND_LINK_DISCOVERY_PORT", "0");
    qunsetenv("FROND_LINK_SOCKET");
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setOrganizationName("FrondTest"); app.setApplicationName("FrondTest");
    app.setQuitOnLastWindowClosed(false);
    qputenv("FROND_LINK_HUB", (QCoreApplication::applicationDirPath() + "/frond-desktop").toUtf8());
    qputenv("FROND_BTVIRT", fakeRadio().toUtf8());
    qputenv("FROND_BT_PHONE", fakePhone().toUtf8());
    // Card tool operations run on the real ARM core even where a window's core is a fake one.
    qputenv("FROND_CARD_TOOL", cardsCore().toUtf8());
    WorkflowTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "desktop-workflow.moc"
