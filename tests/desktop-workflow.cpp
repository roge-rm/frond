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
#include <QRadioButton>
#include <QMessageBox>
#include <QTimer>
#include <QAction>
#include <QDialog>
#include "SdImage.h"
#include <QSettings>
#include <QSpinBox>
#include <QSlider>
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
        {
            MainWindow window(core.fileName()); window.show();
            auto* option = window.findChild<QCheckBox*>("fastAudio"); QVERIFY(option);
            QVERIFY(!option->isChecked()); option->setChecked(true);
            auto* scale = window.findChild<QSlider*>("windowScale"); QVERIFY(scale);
            QCOMPARE(scale->minimum(), 4); QCOMPARE(scale->maximum(), 8);
            scale->setValue(7);
            auto* smooth = window.findChild<QCheckBox*>("smoothLcd"); QVERIFY(smooth);
            QVERIFY(!smooth->isChecked()); smooth->setChecked(true);
            window.openSession(source.fileName(), true);
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("stop")->isEnabled(), 5000);
            QVERIFY(!option->isEnabled());
            QFile args(source.fileName()+".args"); QVERIFY(args.open(QIODevice::ReadOnly));
            const auto arguments = args.readAll();
            QVERIFY(arguments.contains("--fast-audio"));
            QVERIFY(arguments.contains("--window-scale\n1.75"));
            QVERIFY(arguments.contains("--smooth-lcd"));
            QVERIFY(!scale->isEnabled()); QVERIFY(!smooth->isEnabled());
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        }
        MainWindow next(core.fileName());
        QVERIFY(next.findChild<QCheckBox*>("fastAudio")->isChecked());
        QCOMPARE(next.findChild<QSlider*>("windowScale")->value(), 7);
        QVERIFY(next.findChild<QCheckBox*>("smoothLcd")->isChecked());
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
    message={b'device-state':b'tungsten-e2',b'keyboard-state':b'detached',b'network-state':b'disabled',b'sd-state':b'none',b'usb-state':b'pty /dev/pts/9 connected running'}.get(cmd,b'done')
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
        MainWindow window(core.fileName()); window.show();
        auto* device = window.findChild<QComboBox*>("device"); QVERIFY(device);
        auto* start = window.findChild<QPushButton*>("start");
        auto* stop = window.findChild<QPushButton*>("stop");
        // A TX choice with an E2 NOR is refused and nothing starts.
        window.findChild<QLineEdit*>("nor")->setText(norPath);
        window.findChild<QLineEdit*>("nand")->setText({});
        for (auto* radio : window.findChildren<QRadioButton*>()) if (radio->text().startsWith("Start")) radio->setChecked(true);
        device->setCurrentIndex(device->findText("Palm TX"));
        // The launcher greys the unsupported settings once the device is settled, before Start.
        const QStringList gated{"serialMode"};
        auto gatedWidget = [&](const QString& name) { return window.findChild<QWidget*>(name); };
        for (const auto& name : gated) QVERIFY2(gatedWidget(name)->isEnabled(), qPrintable(name));
        for (const char* mode : {"Detect from the NOR image", "Tungsten E2"}) {
            device->setCurrentIndex(device->findText(mode));
            for (const auto& name : gated) {
                QVERIFY2(!gatedWidget(name)->isEnabled(), qPrintable(name));
                QVERIFY2(gatedWidget(name)->toolTip().contains("Not available on the Tungsten E2 yet"), qPrintable(name));
            }
            QVERIFY(!window.findChild<QSpinBox*>("serialPort")->isEnabled());
            QVERIFY(!window.findChild<QSpinBox*>("usbPort")->isEnabled());
        }
        // The saved values stay while they are greyed.
        QCOMPARE(window.findChild<QComboBox*>("serialMode")->currentData().toString(), QString("tcp"));
        QCOMPARE(window.findChild<QComboBox*>("usbMode")->currentData().toString(), QString("pty"));
        QVERIFY(window.findChild<QCheckBox*>("fastAudio")->isChecked());
        device->setCurrentIndex(device->findText("Palm TX"));
        for (const auto& name : gated) {
            QVERIFY2(gatedWidget(name)->isEnabled(), qPrintable(name));
            QVERIFY2(!gatedWidget(name)->toolTip().contains("Not available"), qPrintable(name));
        }
        QVERIFY(window.findChild<QSpinBox*>("serialPort")->isEnabled());
        QString shown;
        QTimer dialogPoll;
        dialogPoll.setInterval(50);
        connect(&dialogPoll, &QTimer::timeout, &window, [&] {
            if (auto* box = window.findChild<QMessageBox*>(); box && box->isVisible()) { shown = box->text(); box->done(QMessageBox::Ok); }
        });
        dialogPoll.start();
        start->click();
        QTRY_VERIFY_WITH_TIMEOUT(shown.contains("Tungsten E2"), 5000);
        QVERIFY(read(".args").isEmpty());
        // Detecting the device from the NOR starts the E2: no TX flag, and no NAND when it's empty.
        device->setCurrentIndex(0);
        start->click();
        QTRY_VERIFY_WITH_TIMEOUT(stop->isEnabled(), 5000);
        dialogPoll.stop();
        const auto args = read(".args").split('\n');
        QCOMPARE(args.value(0), norPath);
        QVERIFY(!args.contains("--experimental-tx") && !args.contains("--nand"));
        QVERIFY(args.contains("--fast-audio"));
        // Leave automatic sizing to the core, which knows the restored device and display.
        QVERIFY(!args.contains("--window-scale") && !args.contains("--smooth-lcd"));
        QVERIFY(!QSettings().contains("windowScale"));
        QTRY_VERIFY_WITH_TIMEOUT(read(".commands").contains("sd-state"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(read(".commands").contains("network-state"), 5000);
        // TX-only network/serial controls stay off; the E2 keyboard and USB are usable.
        QVERIFY(!window.findChild<QComboBox*>("networkMode")->isEnabled());
        QVERIFY(window.findChild<QCheckBox*>("keyboardAttached")->isEnabled());
        window.findChild<QCheckBox*>("keyboardAttached")->click();
        QTRY_VERIFY_WITH_TIMEOUT(read(".commands").contains("keyboard-attach"), 5000);
        QTRY_VERIFY(window.findChild<QCheckBox*>("keyboardAttached")->isChecked());
        QVERIFY(!window.findChild<QComboBox*>("serialMode")->isEnabled());
        QTRY_VERIFY(window.findChild<QComboBox*>("usbMode")->isEnabled());
        QVERIFY(!read(".commands").contains("serial-state"));
        QTRY_VERIFY_WITH_TIMEOUT(read(".commands").contains("usb-state"), 5000);
        QVERIFY(read(".args").contains("--usb\npty"));
        QTRY_VERIFY(window.findChild<QLabel*>("usbState")->text().contains("Tungsten E2 enumerated"));
        QVERIFY(!window.findChild<QComboBox*>("usbMode")->toolTip().contains("Not available"));
        // A NAND file is passed on when given.
        window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        MainWindow second(core.fileName()); second.show();
        for (auto* radio : second.findChildren<QRadioButton*>()) if (radio->text().startsWith("Start")) radio->setChecked(true);
        second.findChild<QLineEdit*>("nor")->setText(norPath);
        second.findChild<QLineEdit*>("nand")->setText(nandPath);
        auto* secondDevice = second.findChild<QComboBox*>("device");
        secondDevice->setCurrentIndex(secondDevice->findText("Tungsten E2"));
        second.findChild<QPushButton*>("start")->click();
        QTRY_VERIFY_WITH_TIMEOUT(second.findChild<QPushButton*>("stop")->isEnabled(), 5000);
        QVERIFY(read(".args").contains("--nand\n" + nandPath));
        QVERIFY(!read(".args").contains("--experimental-tx"));
        second.close(); QTRY_VERIFY_WITH_TIMEOUT(!second.isVisible(), 5000);
        // The serial, USB and fast audio settings made before the E2 session are still saved.
        QCOMPARE(QSettings().value("serialMode").toString(), QString("tcp"));
        QCOMPARE(QSettings().value("usbMode").toString(), QString("pty"));
        QVERIFY(QSettings().value("fastAudio").toBool());
        MainWindow third(core.fileName()); third.show();
        QCOMPARE(third.findChild<QComboBox*>("serialMode")->currentData().toString(), QString("tcp"));
        QCOMPARE(third.findChild<QComboBox*>("usbMode")->currentData().toString(), QString("pty"));
        QVERIFY(third.findChild<QCheckBox*>("fastAudio")->isChecked());
        QSettings().clear();
    }
    void infraredSetting_data() {
        QTest::addColumn<QByteArray>("deviceId");
        QTest::newRow("tx") << QByteArray("palm-tx");
        QTest::newRow("e2") << QByteArray("tungsten-e2");
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
        QTest::newRow("e2") << QByteArray("tungsten-e2");
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
        else: pathlib.Path(arg).write_text('recovery '+state)
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
        auto* action = window.findChild<QAction*>("sdExchange"); QVERIFY(action);
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
                for (auto* a : window.findChildren<QAction*>())
                    if (a->text() == "&Reinsert" || a->text() == "Insert &Image…") QVERIFY(!a->isEnabled());
                if (mode == "cancel-edit") dialog->reject();
                else if (mode == "close-edit") window.close();
                else dialog->accept();
            }
        });
        poll.start(); action->trigger();
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
                QFile saved(backup); QVERIFY(saved.open(QIODevice::ReadOnly)); QCOMPARE(saved.readAll(), QByteArray("recovery ejected"));
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
