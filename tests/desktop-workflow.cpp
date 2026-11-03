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
#include "Dialogs.h"
#include "FirmwareStore.h"
#include "SessionStore.h"
#include "AudioQualification.h"
#include <QMessageBox>
#include <QMenuBar>
#include <QTimer>
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

static QByteArray digest(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return hash.result();
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
            QVERIFY(!arguments.contains("--fast-audio"));
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
            mode->setCurrentIndex(mode->findData("tcp"));
            emit mode->activated(mode->currentIndex());
            QVERIFY(port->isEnabled());
            port->setValue(5099);
            emit port->editingFinished();
            QCOMPARE(QSettings().value("serialMode").toString(), QString("tcp"));
            QCOMPARE(QSettings().value("serialPort").toInt(), 5099);
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
            QCOMPARE(QSettings().value("serialMode").toString(), QString("pty"));
            QCOMPARE(QSettings().value("serialPort").toInt(), 5099);
            QTRY_VERIFY_WITH_TIMEOUT(mode->isEnabled(), 5000);
            mode->setCurrentIndex(mode->findData("pty"));
            emit mode->activated(mode->currentIndex());
            QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Serial: /dev/pts/9, connected"), 5000);
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
            QVERIFY(state->text().isEmpty());
        }
        MainWindow next(core.fileName());
        QCOMPARE(next.findChild<QComboBox*>("serialMode")->currentData().toString(), QString("pty"));
        QCOMPARE(next.findChild<QSpinBox*>("serialPort")->value(), 5099);
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
            mode->setCurrentIndex(mode->findData("tcp"));
            emit mode->activated(mode->currentIndex());
            QVERIFY(port->isEnabled());
            port->setValue(5098);
            emit port->editingFinished();
            QCOMPARE(QSettings().value("usbMode").toString(), QString("tcp"));
            QCOMPARE(QSettings().value("usbPort").toInt(), 5098);
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
            QCOMPARE(QSettings().value("usbMode").toString(), QString("pty"));
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
            QCOMPARE(QSettings().value("usbMode").toString(), QString("pty"));
            QCOMPARE(QSettings().value("usbPort").toInt(), 5098);
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
            QVERIFY(state->text().isEmpty());
        }
        MainWindow next(core.fileName());
        QCOMPARE(next.findChild<QComboBox*>("usbMode")->currentData().toString(), QString("pty"));
        QCOMPARE(next.findChild<QSpinBox*>("usbPort")->value(), 5098);
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
        QVERIFY(!args.contains("--fast-audio"));
        QVERIFY(read(".args").contains("--window-scale\n1.75"));
        QVERIFY(args.contains("--smooth-lcd") && args.contains("--no-sound"));
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
        QVERIFY(!window.findChild<QComboBox*>("serialMode")->isEnabled());
        QVERIFY(window.findChild<QComboBox*>("serialMode")->toolTip().contains("Not available on the Tungsten E2 yet"));
        QVERIFY(!window.findChild<QSpinBox*>("serialPort")->isEnabled());
        QTRY_VERIFY(window.findChild<QComboBox*>("usbMode")->isEnabled());
        QVERIFY(!read(".commands").contains("serial-state"));
        QTRY_VERIFY_WITH_TIMEOUT(read(".commands").contains("usb-state"), 5000);
        QVERIFY(read(".args").contains("--usb\npty"));
        QTRY_VERIFY(window.findChild<QLabel*>("usbState")->text().contains("Tungsten E2 enumerated"));
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
        MainWindow third(core.fileName()); third.show();
        QCOMPARE(third.findChild<QComboBox*>("serialMode")->currentData().toString(), QString("tcp"));
        QCOMPARE(third.findChild<QComboBox*>("usbMode")->currentData().toString(), QString("pty"));
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
                QVERIFY(!firmware->machineSettings->fastAudio->isEnabled());
                QCOMPARE(firmware->machineSettings->fastAudio->toolTip(), QString("Not available on the Zire 72 yet"));
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
        // No NAND, low-latency audio or serial port; USB and infrared settings carry over.
        QVERIFY(!args.contains("--nand") && !args.contains("--fast-audio") && !args.contains("--serial"));
        QVERIFY(read(".args").contains("--usb\npty") && read(".args").contains("--irda\npty"));
        QCOMPARE(window.windowTitle(), QString("Zire 72"));
        QTRY_VERIFY_WITH_TIMEOUT(read(".commands").contains("network-state"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(read(".commands").contains("usb-state"), 5000);
        QVERIFY(!read(".commands").contains("serial-state"));
        // Network, serial and low-latency audio grey out with the shared wording; the rest stay.
        for (const char* name : {"networkMode", "serialMode"}) {
            auto* box = window.findChild<QComboBox*>(name);
            QVERIFY(!box->isEnabled());
            QCOMPARE(box->toolTip(), QString("Not available on the Zire 72 yet"));
        }
        QVERIFY(!window.findChild<QSpinBox*>("serialPort")->isEnabled());

        QTRY_VERIFY(window.findChild<QComboBox*>("usbMode")->isEnabled());
        QVERIFY(window.findChild<QComboBox*>("irdaMode")->isEnabled());
        QVERIFY(window.findChild<QCheckBox*>("bluetoothEnabled")->isEnabled());
        QVERIFY(!window.findChild<QCheckBox*>("keyboardAttached")->isEnabled());  // infrared beaming has the port
        QTRY_VERIFY(window.findChild<QLabel*>("usbState")->text().contains("Zire 72 enumerated"));
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
        MainWindow next(core.fileName());
        QCOMPARE(next.findChild<QComboBox*>("irdaMode")->currentData().toString(),QString("tcp"));
        QCOMPARE(next.findChild<QSpinBox*>("irdaPort")->value(),5097);
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
            auto* enabled=window.findChild<QCheckBox*>("bluetoothEnabled");
            auto* path=window.findChild<QLineEdit*>("bluetoothSocket");
            auto* state=window.findChild<QLabel*>("bluetoothState");
            QVERIFY(enabled && path && state); QVERIFY(!enabled->isChecked()); QVERIFY(!path->isEnabled());
            enabled->click(); path->setText(socketPath); emit path->editingFinished();
            QCOMPARE(QSettings().value("bluetoothSocket").toString(),socketPath);
            window.openSession(source.fileName(),true);
            QTRY_VERIFY(enabled->isEnabled() && state->text().contains("connected"));
            QFile args(source.fileName()+".args"); QVERIFY(args.open(QIODevice::ReadOnly));
            QVERIFY(args.readAll().contains("--bluetooth\n"+socketPath.toUtf8()));
            enabled->click(); QTRY_VERIFY(enabled->isEnabled() && state->text().isEmpty());
            QVERIFY(!path->isEnabled()); enabled->click();
            QTRY_VERIFY(enabled->isEnabled() && state->text().contains("connected"));
            QString shown; QTimer dismiss; dismiss.setInterval(20);
            connect(&dismiss,&QTimer::timeout,&window,[&]{
                if(auto* box=window.findChild<QMessageBox*>();box && box->isVisible()) {shown=box->text();box->done(QMessageBox::Ok);}
            }); dismiss.start();
            path->setText(socketPath+" missing"); emit path->editingFinished();
            QTRY_VERIFY(shown.contains("Controller socket unavailable"));
            QCOMPARE(QSettings().value("bluetoothSocket").toString(),socketPath);
            QTRY_VERIFY(enabled->isEnabled()); enabled->click();
            QTRY_VERIFY(enabled->isEnabled() && state->text().isEmpty());
            QCOMPARE(QSettings().value("bluetoothSocket").toString(),socketPath);
            path->setText(socketPath); enabled->click();
            QTRY_VERIFY(enabled->isEnabled() && state->text().contains("connected"));
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(),5000);
        }
        MainWindow next(core.fileName());
        QVERIFY(next.findChild<QCheckBox*>("bluetoothEnabled")->isChecked());
        QCOMPARE(next.findChild<QLineEdit*>("bluetoothSocket")->text(),socketPath);
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
    void sdExchange_data() {
        QTest::addColumn<QString>("mode");
        for (const auto* mode : {"cancel-start", "cancel-edit", "apply", "save-fail", "export-fail", "insert-fail", "eject-fail", "already-ejected", "close-edit", "close-start"})
            QTest::newRow(mode) << QString(mode);
    }
    void sdExchange() {
        QFETCH(QString, mode);
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        const auto source = files.filePath("input.frond");
        QFile session(source); QVERIFY(session.open(QIODevice::WriteOnly)); session.write("private original session"); session.close();
        const auto original = digest(source);
        QFile card(source + ".img"); QVERIFY(card.open(QIODevice::WriteOnly));
        const auto image = frondSd::create(32);
        QCOMPARE(card.write(reinterpret_cast<const char*>(image.data()), image.size()), qint64(image.size())); card.close();
        const auto originalCard = digest(card.fileName());
        QFile modeFile(source + ".mode"); QVERIFY(modeFile.open(QIODevice::WriteOnly)); modeFile.write(mode.toUtf8()); modeFile.close();
        QFile core(files.filePath("core")); QVERIFY(core.open(QIODevice::WriteOnly));
        core.write(R"PY(#!/usr/bin/python3
import socket,sys,pathlib,shutil
source=pathlib.Path(sys.argv[1])
mode=pathlib.Path(str(source)+'.mode').read_text()
trace=pathlib.Path(str(source)+'.commands')
s=socket.socket(socket.AF_UNIX)
s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
state='ejected' if mode=='already-ejected' else 'inserted'
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t')
    cmd=cmd.decode(); arg=bytes.fromhex(arg.decode()).decode()
    with trace.open('a') as f: f.write(cmd+'\t'+arg+'\n')
    ok=True; message='done'
    if cmd=='device-state': message='palm-tx'
    elif cmd=='sd-state': message=state
    elif cmd=='network-state': message='disabled'
    elif cmd=='keyboard-state': message='detached'
    elif cmd=='sd-eject':
        if mode=='eject-fail': ok=False
        else: state='ejected'
    elif cmd=='save':
        if mode=='save-fail' and 'sd-exchange' in arg: ok=False
        else:
            p=pathlib.Path(arg)
            if p.suffix!='.frond':
                p.mkdir(parents=True, exist_ok=True)
                p=p/'session.frond'
            p.write_text('recovery '+state)
    elif cmd=='sd-export':
        assert state=='ejected'
        if mode=='export-fail': ok=False
        else: shutil.copyfile(str(source)+'.img',arg)
    elif cmd=='sd-insert':
        assert state=='ejected' and pathlib.Path(arg).is_file()
        if mode=='insert-fail': ok=False
        else: state='inserted'
    elif cmd=='quit': pass
    else: raise AssertionError(cmd)
    if not ok: message='Injected '+cmd+' failure'
    s.sendall(i+b'\t'+(b'ok' if ok else b'error')+b'\tresult\t'+message.encode().hex().encode()+b'\n')
    if cmd=='quit': break
)PY");
        core.close(); QVERIFY(core.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        MainWindow window(core.fileName()); window.show(); window.openSession(source, true);
        auto* action = window.findChild<QPushButton*>("sdExchange"); QVERIFY(action);
        QTRY_VERIFY_WITH_TIMEOUT(action->isEnabled(), 5000);
        bool sawEditor = false, sawError = false;
        QTimer poll; poll.setInterval(10);
        connect(&poll, &QTimer::timeout, &window, [&] {
            if (auto* box = window.findChild<QMessageBox*>()) {
                if (box->windowTitle().contains("Close Palm")) {
                    if (mode == "close-start") window.close();
                    box->button(mode == "cancel-start" ? QMessageBox::Cancel : QMessageBox::Yes)->click();
                }
                else { sawError = true; box->button(QMessageBox::Ok)->click(); }
            }
            if (auto* dialog = window.findChild<QDialog*>("sdExchangeDialog")) {
                if (!dialog->isVisible()) return;
                sawEditor = true;
                QVERIFY(!action->isEnabled());
                for (const auto* name : {"sdInsert", "sdInsertImage", "sdEject", "sdNew"}) QVERIFY(!window.findChild<QPushButton*>(name)->isEnabled());
                if (mode == "cancel-edit") dialog->reject();
                else if (mode == "close-edit") window.close();
                else dialog->accept();
            }
        });
        poll.start(); action->click();
        QTRY_VERIFY_WITH_TIMEOUT((mode == "close-edit" || mode == "close-start") ? !window.isVisible() : action->isEnabled(), 10000);
        poll.stop();
        const auto state = window.findChild<QLabel*>("sdState")->text();
        QCOMPARE(state.contains("Inserted"), mode == "apply" || mode == "already-ejected" || mode == "cancel-start" || mode == "eject-fail");
        QCOMPARE(sawEditor, mode == "cancel-edit" || mode == "apply" || mode == "insert-fail" || mode == "already-ejected" || mode == "close-edit");
        QCOMPARE(sawError, mode.endsWith("fail"));
        QFile trace(source + ".commands"); QVERIFY(trace.open(QIODevice::ReadOnly));
        const auto commands = QString::fromUtf8(trace.readAll()).split('\n', Qt::SkipEmptyParts);
        QStringList exchangeCommands; QString backup;
        for (const auto& line : commands) {
            const auto command = line.section('\t', 0, 0);
            if (command == "save" && backup.isEmpty()) backup = line.section('\t', 1);
            if (command == "save" || command == "quit" || command.startsWith("sd-")) exchangeCommands << command;
        }
        QStringList expected{"sd-state"};
        if (mode != "cancel-start" && mode != "eject-fail" && mode != "close-start") {
            if (mode != "already-ejected") expected << "sd-eject";
            expected << "save";
            if (mode != "save-fail") {
                expected << "sd-export";
                if (mode == "apply" || mode == "insert-fail" || mode == "already-ejected") expected << "sd-insert";
                if (mode == "close-edit") expected << "save" << "quit";
                const auto savedPath = QFileInfo(backup).isDir() ? backup + "/session.frond" : backup;
                QFile saved(savedPath); QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), QByteArray("recovery ejected"));
                const auto permissions = QFileInfo(QFileInfo(backup).absolutePath()).permissions();
                QVERIFY(!(permissions & (QFile::ReadGroup | QFile::ReadOther)));
            } else QVERIFY(!QFileInfo::exists(backup));
        }
        if (mode == "eject-fail") expected << "sd-eject";
        if (mode == "close-start") expected << "save" << "quit";
        QCOMPARE(exchangeCommands, expected);
        QCOMPARE(digest(source), original); QCOMPARE(digest(card.fileName()), originalCard);
        if (window.isVisible()) {
            QVERIFY(!window.close());
            QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        }
        QCOMPARE(digest(source), original);
    }
    // Sessions in the managed folder, written the way the core does (magic, version, device name).
    static QByteArray sessionBytes(quint32 version, const QByteArray& device) {
        QByteArray bytes;
        for (quint32 value : {0x19800819u, version})
            for (int i = 0; i < 4; i++) bytes.append(char(value >> (8 * i)));
        return bytes + device.leftJustified(32, '\0', true) + "body";
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
    message={b'keyboard-state':b'detached',b'network-state':b'disabled',b'sd-state':b'none',b'device-state':b'palm-tx'}.get(cmd,b'done')
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
        QVERIFY(window.findChild<QWidget*>("running")->isVisible());
        QCOMPARE(window.windowTitle(), QString("Trip"));
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        // The core's save went into the same session, and it is the one that resumes.
        QCOMPARE(QSettings().value("lastSession").toString(), QFileInfo(managedSession("Trip")).canonicalFilePath());
        QDir(managedFolder()).removeRecursively();
        QSettings().clear();
    }
    // The running page has the Android Menu sections, and the menu bar only File and Help.
    void hardReset() {
        QSettings().clear(); QSettings().setValue("resume", false);
        QTemporaryDir files;
        QFile source(files.path() + "/hard.frond");
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write("session"); source.close();
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
        MainWindow window(core.fileName()); window.show(); window.openSession(source.fileName(), true);
        auto* hard = window.findChild<QPushButton*>("hardReset");
        QTRY_VERIFY_WITH_TIMEOUT(hard->isEnabled(), 5000);
        QVERIFY(hard->isVisibleTo(&window));
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
        hard->click();
        QTRY_COMPARE_WITH_TIMEOUT(commands(), QStringList({"save", "save", "hard-reset"}), 3000);
        QTRY_VERIFY(hard->isEnabled());
        answer = "Erase"; hard->click();
        QTRY_COMPARE_WITH_TIMEOUT(commands(), QStringList({"save", "save", "hard-reset", "hard-reset"}), 3000);
        poll.stop();
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
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
        QTRY_VERIFY_WITH_TIMEOUT(hard->isVisible() && hard->isEnabled(), 5000);
        window.findChild<QPushButton*>("save")->click();
        QTRY_COMPARE_WITH_TIMEOUT(commands(), QStringList({"save", "save"}), 3000);
        QTest::qWait(300);
        QCOMPARE(commands(), QStringList({"save", "save"}));
        poll.stop();
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
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
        QStringList items;
        for (auto* action : window.menuBar()->actions().first()->menu()->actions()) if (!action->isSeparator()) items << action->text();
        QCOMPARE(items, QStringList({"&Import session…", "&Firmware…", "&Settings…", "&Quit"}));
        auto* list = window.findChild<QListWidget*>("sessionList");
        emit list->itemActivated(list->item(0));
        auto* sd = window.findChild<QLabel*>("sdState");
        QTRY_COMPARE_WITH_TIMEOUT(sd->text(), QString("Inserted"), 5000);
        QStringList headings;
        for (auto* label : window.findChild<QWidget*>("running")->findChildren<QLabel*>())
            if (QStringList({"Applications", "SD card", "Device"}).contains(label->text())) headings << label->text();
        QCOMPARE(headings, QStringList({"Applications", "SD card", "Device"}));
        const auto shown = [&](const char* name) { return window.findChild<QPushButton*>(name)->isVisibleTo(&window); };
        QVERIFY(shown("sdExchange") && shown("sdEject") && !shown("sdInsert") && !shown("sdNew") && !shown("sdRetry"));
        QTRY_VERIFY(window.findChild<QPushButton*>("installApp")->isEnabled());
        // The E2 has no network or serial port; its USB, keyboard and Bluetooth stay.
        QVERIFY(!window.findChild<QComboBox*>("networkMode")->isEnabled());
        QVERIFY(!window.findChild<QComboBox*>("serialMode")->isEnabled());
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
        QTRY_VERIFY(window.findChild<QPushButton*>("sdEject")->isEnabled());
        window.findChild<QPushButton*>("sdEject")->click();
        QTRY_COMPARE_WITH_TIMEOUT(sd->text(), QString("Ejected"), 5000);
        QTRY_VERIFY(shown("sdInsert") && shown("sdExchange") && shown("sdExport") && shown("sdNew") && shown("sdInsertImage") && !shown("sdEject"));
        answer.stop();
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        QDir(managedFolder()).removeRecursively();
        QSettings().clear();
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
    elif cmd=='ms-create':
        assert arg=='128' and card!='inserted';card='inserted'
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
        auto* create = window.findChild<QPushButton*>("sdNew");
        auto* protection = window.findChild<QCheckBox*>("cardReadOnly");
        QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("No card"), 5000);
        QVERIFY(create->isVisible() && create->isEnabled());
        QCOMPARE(window.findChild<QLabel*>("storageHeading")->text(), QString("Memory Stick"));
        QVERIFY(!window.findChild<QPushButton*>("sdExchange")->isVisible());
        create->click();
        QTRY_COMPARE_WITH_TIMEOUT(state->text(), QString("Inserted"), 5000);
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
        QVERIFY(commands.contains("ms-create\t128\n"));
        QVERIFY(commands.contains("ms-read-only\ton\n"));
        QVERIFY(commands.contains("ms-reinsert\t\n"));
        QVERIFY(!commands.contains("sd-"));
        QDir(managedFolder()).removeRecursively(); QSettings().clear();
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
            MainWindow next(core.fileName()); next.show(); next.openSession(two, true);
            auto* sound = next.findChild<QCheckBox*>("sound"); QTRY_VERIFY(sound->isEnabled()); QVERIFY(sound->isChecked());
            QVERIFY(read(".args").contains("--window-scale\n1"));
            QVERIFY(!read(".args").contains("--no-sound") && !read(".args").contains("--smooth-lcd") && !read(".args").contains("--fast-audio"));
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
        for (bool keyboard : {false, true}) for (bool jog : {false, true}) {
            frondSessions::Preferences prefs;
            prefs.showKeyboard = keyboard; prefs.showJogControls = jog; prefs.microphone = true;
            QVERIFY(frondSessions::writePreferences(path, prefs).isEmpty());
            QFile::remove(core.fileName() + ".args");
            MainWindow window(core.fileName()); window.show(); window.openSession(path, true);
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("stop")->isEnabled(), 5000);
            QFile args(core.fileName() + ".args"); QVERIFY(args.open(QIODevice::ReadOnly));
            const auto arguments = args.readAll().split('\n');
            QCOMPARE(arguments.contains("--hide-keyboard"), !keyboard);
            QCOMPARE(arguments.contains("--hide-jog-controls"), !jog);
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
            QVERIFY(frondSessions::readPreferences(path, &prefs));
            QCOMPARE(prefs.showKeyboard, keyboard); QCOMPARE(prefs.showJogControls, jog);
            QVERIFY(prefs.microphone);
            QCOMPARE(digest(path), guest);
        }
        QDir(managedFolder()).removeRecursively(); QSettings().clear();
    }
    void machineControlVisibilitySettings() {
        frondSessions::Preferences original{7, true, false, true, true, false, true};
        MachineSettings controls(original, false);
        controls.setDevice(frondFirmware::deviceFromId("sony-clie-tg50"));
        QVERIFY(!controls.showKeyboard->isHidden());
        QVERIFY(!controls.showJogControls->isHidden());
        QVERIFY(!controls.showKeyboard->isChecked());
        QVERIFY(controls.showJogControls->isChecked());
        for (bool keyboard : {false, true}) for (bool jog : {false, true}) {
            controls.showKeyboard->setChecked(keyboard); controls.showJogControls->setChecked(jog);
            const auto saved = controls.values();
            QCOMPARE(saved.showKeyboard, keyboard); QCOMPARE(saved.showJogControls, jog);
            QCOMPARE(saved.scale, original.scale);
            QVERIFY(saved.smooth && !saved.audio && saved.fastAudio && saved.microphone);
        }
        controls.setDevice(frondFirmware::Device::tx);
        QVERIFY(controls.showKeyboard->isHidden());
        QVERIFY(controls.showJogControls->isHidden());
        QVERIFY(controls.values().microphone);
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
                QVERIFY(!dialog->controls->fastAudio->isEnabled());
                QCOMPARE(dialog->controls->fastAudio->toolTip(), QString("Requires qualified firmware"));
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
        auto* enabled = window.findChild<QCheckBox*>("bluetoothEnabled");
        auto* path = window.findChild<QLineEdit*>("bluetoothSocket");
        auto* state = window.findChild<QLabel*>("bluetoothState");
        auto* stop = window.findChild<QPushButton*>("stop");
        QTRY_VERIFY_WITH_TIMEOUT(stop->isEnabled() && enabled->isEnabled(), 20000);
        QVERIFY(!enabled->isChecked()); QVERIFY(!path->isEnabled());
        enabled->click(); QTRY_VERIFY_WITH_TIMEOUT(state->text().contains("provider connected"), 5000);
        QString shown; QTimer dismiss; dismiss.setInterval(20);
        connect(&dismiss, &QTimer::timeout, &window, [&] {
            if (auto* box = window.findChild<QMessageBox*>(); box && box->isVisible()) { shown = box->text(); box->done(QMessageBox::Ok); }
        }); dismiss.start();
        path->setText("relative"); emit path->editingFinished();
        QTRY_VERIFY_WITH_TIMEOUT(!shown.isEmpty(), 5000);
        QCOMPARE(QSettings().value("bluetoothSocket").toString(), endpoint);
        path->setText(endpoint); enabled->click();
        QTRY_VERIFY_WITH_TIMEOUT(enabled->isEnabled() && state->text().isEmpty(), 5000);
        enabled->click(); QTRY_VERIFY_WITH_TIMEOUT(state->text().contains("provider connected"), 5000);
        stop->click(); QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("addDevice")->isEnabled(), 15000);
        QVERIFY(!enabled->isChecked());
        const auto saved = QSettings().value("lastSession").toString(); QVERIFY(QFileInfo::exists(saved));
        window.openSession(saved, true);
        QTRY_VERIFY_WITH_TIMEOUT(stop->isEnabled() && enabled->isEnabled(), 20000); QVERIFY(!enabled->isChecked());
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
                QVERIFY(!dialog->machineSettings->fastAudio->isEnabled());
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
                QVERIFY(!dialog->controls->fastAudio->isEnabled());
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
            QCOMPARE(window.findChild<QPushButton*>("sdNew")->isVisible(), frondFirmware::profile(frondFirmware::deviceFromId("sony-clie-tg50")).frontend.memoryStick);
            QVERIFY(!window.findChild<QPushButton*>("sdExchange")->isVisible());
            QVERIFY(!window.findChild<QComboBox*>("usbMode")->isEnabled());
            QVERIFY(window.findChild<QCheckBox*>("bluetoothEnabled")->isEnabled());
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
};
int main(int argc, char** argv) {
    // Isolate both settings and automatic session files from the real user's desktop.
    QDir().mkpath("build");
    QTemporaryDir directory(QDir::currentPath() + "/build/gui-workflow-XXXXXX");
    QDir().mkpath(directory.path() + "/data-real");
    if (!QFile::link(directory.path() + "/data-real", directory.path() + "/data")) return 1;
    qputenv("XDG_CONFIG_HOME", (directory.path() + "/config").toUtf8());
    qputenv("XDG_DATA_HOME", (directory.path() + "/data").toUtf8());
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setOrganizationName("FrondTest"); app.setApplicationName("FrondTest");
    app.setQuitOnLastWindowClosed(false);
    WorkflowTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "desktop-workflow.moc"
