#include <QtTest>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QMessageBox>
#include <QTimer>
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
    void persistence() {
        const auto core = qEnvironmentVariable("FROND_TEST_CORE");
        const auto source = qEnvironmentVariable("FROND_TEST_SESSION");
        QVERIFY(!core.isEmpty() && !source.isEmpty());
        const auto before = digest(source);
        QVERIFY(!before.isEmpty());
        QString saved, cardState, networkMode;
        {
            MainWindow window(core); window.show(); window.openSession(source, true);
            auto* stop = window.findChild<QPushButton*>("stop");
            QTRY_VERIFY_WITH_TIMEOUT(stop->isEnabled(), 30000);
            const auto screenshot = qEnvironmentVariable("FROND_TEST_SCREENSHOT");
            if (!screenshot.isEmpty()) QVERIFY(window.grab().save(screenshot));
            cardState = window.findChild<QLabel*>("sdState")->text();
            networkMode = window.findChild<QComboBox*>("networkMode")->currentData().toString();
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
