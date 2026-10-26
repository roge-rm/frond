#include <QtTest>
#include <QFile>
#include "DesktopClient.h"

class ClientTest : public QObject {
    Q_OBJECT
 private slots:
    void framing() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile fake(dir.path() + "/core.py");
        QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write(R"PY(import socket,sys,time
s=socket.socket(socket.AF_UNIX)
s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\trea')
time.sleep(.03)
s.sendall(b'dy\t\n')
f=s.makefile('rb')
for line in f:
    i,command,arg=line.rstrip(b'\n').split(b'\t')
    assert bytes.fromhex(arg.decode()).decode() == ('/tmp/a b\n音.frond' if command == b'save' else '')
    reply=i+b'\tok\tdone\t'+b'Complete'.hex().encode()+b'\n'
    s.sendall(reply[:5]);time.sleep(.03);s.sendall(reply[5:])
    if command==b'quit': break
s.close()
)PY");
        fake.close();
        DesktopClient client;
        QSignalSpy ready(&client, &DesktopClient::started), result(&client, &DesktopClient::result);
        QSignalSpy finished(&client, &DesktopClient::finished), error(&client, &DesktopClient::error);
        QVERIFY(client.start("/usr/bin/python3", {fake.fileName()}));
        QTRY_COMPARE(ready.count(), 1);
        QVERIFY(client.available());
        QVERIFY(client.request("save", QString::fromUtf8("/tmp/a b\n音.frond")));
        QVERIFY(!client.request("reset"));
        QTRY_COMPARE(result.count(), 1);
        QCOMPARE(result.first().at(0).toString(), "save");
        QCOMPARE(result.first().at(1).toBool(), true);
        QCOMPARE(result.first().at(3).toString(), "Complete");
        QVERIFY(client.request("quit"));
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(error.count(), 0);
        QVERIFY(!client.running());
        // A second launch must discard the previous socket and reply buffer.
        QVERIFY(client.start("/usr/bin/python3", {fake.fileName()}));
        QTRY_COMPARE(ready.count(), 2);
        QVERIFY(client.request("quit"));
        QTRY_COMPARE(finished.count(), 2);
        QCOMPARE(error.count(), 0);
    }
    void failedStart() {
        DesktopClient client;
        QSignalSpy error(&client, &DesktopClient::error), finished(&client, &DesktopClient::finished);
        QVERIFY(client.start("/missing/frond-core", {}));
        QTRY_COMPARE(error.count(), 1);
        QTRY_COMPARE(finished.count(), 1);
        QVERIFY(!client.running());
    }
};
QTEST_GUILESS_MAIN(ClientTest)
#include "desktop-client.moc"
