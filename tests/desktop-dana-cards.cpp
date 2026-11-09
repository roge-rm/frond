#include <QtTest>
#include <QTemporaryDir>
#include "SessionStore.h"
#include "DanaCardsDialog.h"

class DanaCardsTest : public QObject {
    Q_OBJECT
    const QByteArray key="12345678000000000000000000000000", second="23456789000000000000000000000000";
    QByteArray manifest(bool portable=false) {
        if (portable) return "frond-portable-sd 3\n0 sd sd0.frondsd "+key+" 305419896 33554432 1 0 123 sd\n2 sd sd2.frondsd "+second+" 591751049 33554432 0 1 456 mmc\n";
        return "frond-linked-sd 3\n0 sd \"/cards/a.frondsd\" "+key+" 305419896 33554432 1 0 1 2 3 sd 9\n2 sd \"/cards/b.frondsd\" "+second+" 591751049 33554432 0 1 1 4 5 mmc 7\n";
    }
    void write(const QString& path, const QByteArray& manifest,bool portable=false,const QByteArray& name="alphasmart-dana") {
        QByteArray machine(80,'\0');
        auto put=[](QByteArray& bytes,int at,quint32 value) { for (int n=0;n<4;++n) bytes[at+n]=char(value>>(8*n)); };
        put(machine,0,frondSessions::magic); put(machine,4,8); machine.replace(8,14,name);
        QByteArray descriptor(12,'\0'); put(descriptor,0,manifest.size()); put(descriptor,4,frondDanaSession::hash(machine)); put(descriptor,8,frondDanaSession::hash(manifest));
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(portable?"FRONDPORTSD1\n":"FRONDLINKSD1\n"); file.write(descriptor); file.write(manifest); file.write(machine);
    }
 private slots:
    void inventory() {
        const auto* dana=deviceProfile5(deviceTypeDana);
        QVERIFY(dana->frontend.available);
        const auto device=frondFirmware::deviceFromId("alphasmart-dana");
        QVERIFY(device!=frondFirmware::Device::automatic);
        QVERIFY(dana->capabilities.keyboard); QVERIFY(!frondFirmware::infraredKeyboard(device)); QVERIFY(dana->capabilities.infrared);
        QVERIFY(frondFirmware::infraredKeyboard(frondFirmware::Device::tx));
        for (auto slot : {DeviceCardSlot5::sd0,DeviceCardSlot5::sd2}) {
            QCOMPARE(frondDanaCards::sizes(*dana,slot,DeviceCardFamily5::sdsc),QList<int>({32,64,128,256,512}));
            QCOMPARE(frondDanaCards::sizes(*dana,slot,DeviceCardFamily5::legacyMmc),QList<int>({32,64,128,256}));
        }
    }
    void wirelessSessionHeader() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto path=dir.path()+"/machine.frond";
        write(path,manifest(),false,"alphasmart-dana-wireless");
        QVERIFY(frondDanaSession::inspect(path).valid);
        const auto header=frondSessions::readHeader(path);
        QCOMPARE(header.device,QString("alphasmart-dana-wireless"));
        QVERIFY(frondSessions::openable(header));
        write(path,manifest(),false,"alphasmart-dana-other");
        QVERIFY(!frondDanaSession::inspect(path).valid);
    }
    void state() {
        const auto card=frondDanaCards::state("ejected "+key+" protected io-error mmc");
        QVERIFY(card.known && card.present && !card.inserted && card.protectedCard && card.failed); QCOMPARE(card.protocol,QString("mmc"));
        QVERIFY(frondDanaCards::state("none").known);
        for (auto text : {"inserted", "ejected bad protected", "inserted 12345678000000000000000000000000 writable mmc mmc"}) QVERIFY(!frondDanaCards::state(text).known);
    }
    void wrappedReopenAndTransferGuards() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto path=dir.path()+"/machine.frond";
        for (bool portable : {false,true}) {
            write(path,manifest(portable),portable);
            const auto inspection=frondDanaSession::inspect(path);
            QVERIFY(inspection.valid); QCOMPARE(inspection.portable,portable); QCOMPARE(inspection.version,8u);
            QVERIFY(frondSessions::openable(frondSessions::readHeader(path)));
            QVERIFY(!frondSessions::importFile(dir.path()+"/sessions",path).isEmpty());
            frondSessions::Info info; info.path=path; info.saved=true;
            QVERIFY(!frondSessions::exportFile(info,dir.path()+"/copy").isEmpty());
            QVERIFY(!frondSessions::duplicate(dir.path()+"/sessions",info).isEmpty());
            QVERIFY(!QFileInfo::exists(dir.path()+"/copy"));
        }
    }
    void malformedWrappers() {
        QTemporaryDir dir; const auto path=dir.path()+"/machine.frond";
        for (const auto& text : {manifest().replace("sd 3","sd 2"),manifest().replace("2 sd","0 sd"),manifest().replace(second,key),
                                 manifest().replace("33554432","1073741824"),manifest().replace(" mmc "," unknown ")}) {
            write(path,text); QVERIFY(!frondDanaSession::inspect(path).valid); QVERIFY(!frondSessions::openable(frondSessions::readHeader(path)));
        }
        write(path,manifest()); QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.seek(file.size()-1)); file.write("x"); file.close();
        QVERIFY(!frondDanaSession::inspect(path).valid);
    }
    void independentSocketCommandsAndUncertainExport() {
        QTemporaryDir dir; QFile fake(dir.filePath("core.py")); QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write(R"PY(import socket,sys
s=socket.socket(socket.AF_UNIX);s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
s.sendall(b'0\tevent\tready\t\n')
expected=[b'sd2-state',b'sd-state',b'sd2-insert',b'sd2-eject',b'sd2-take-out',b'export-portable',b'quit']
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t');assert cmd==expected.pop(0)
    message=b'none';ok=b'ok';code=b'card'
    if cmd==b'sd2-state':message=b'ejected 23456789000000000000000000000000 protected mmc'
    if cmd==b'export-portable':
        assert bytes.fromhex(arg.decode())==b'/new folder'
        ok=b'error';code=b'durability-uncertain';message=b'Output published, durability uncertain'
    s.sendall(i+b'\t'+ok+b'\t'+code+b'\t'+message.hex().encode()+b'\n')
    if cmd==b'quit':break
assert not expected
)PY"); fake.close();
        DesktopClient client; QSignalSpy ready(&client,&DesktopClient::started),finished(&client,&DesktopClient::finished);
        QVERIFY(client.start("/usr/bin/python3",{fake.fileName()})); QTRY_COMPARE(ready.count(),1);
        const auto second=DanaCardsDialog::requestWithClient(client,"sd2-state");
        QVERIFY(second.ok); QCOMPARE(frondDanaCards::state(second.message).protocol,QString("mmc"));
        const auto first=DanaCardsDialog::requestWithClient(client,"sd-state"); QVERIFY(first.ok); QCOMPARE(first.message,QString("none"));
        for (const auto* op : {"sd2-insert","sd2-eject","sd2-take-out"}) QVERIFY(DanaCardsDialog::requestWithClient(client,op).ok);
        const auto copy=DanaCardsDialog::requestWithClient(client,"export-portable","/new folder"); QVERIFY(!copy.ok); QVERIFY(copy.message.contains("uncertain"));
        QVERIFY(client.request("quit")); QTRY_COMPARE(finished.count(),1);
    }
};
QTEST_MAIN(DanaCardsTest)
#include "desktop-dana-cards.moc"
