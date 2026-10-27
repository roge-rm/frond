#include <QtTest>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QCheckBox>
#include <QMessageBox>
#include <QTimer>
#include <QAction>
#include <QDialog>
#include "SdImage.h"
#include <QSettings>
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
            window.openSession(source.fileName(), true);
            QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("stop")->isEnabled(), 5000);
            QVERIFY(!option->isEnabled());
            QFile args(source.fileName()+".args"); QVERIFY(args.open(QIODevice::ReadOnly));
            QVERIFY(args.readAll().contains("--experimental-fast-audio"));
            window.close(); QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 5000);
        }
        MainWindow next(core.fileName());
        QVERIFY(next.findChild<QCheckBox*>("fastAudio")->isChecked());
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
    if cmd=='sd-state': message=state
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
