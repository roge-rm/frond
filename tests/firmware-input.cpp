#include <QtTest>
#include <QTemporaryDir>
#include "FirmwareInput.h"

using frondFirmware::Device;
// Sparse synthetic E2 NOR: card header at 'at' with the Palm company id and the E2 HAL id.
static void e2Nor(QFile& nor, qint64 size, qint64 at, quint32 hal = 0x68737072) {
    nor.resize(0); nor.resize(size);
    auto put = [&](qint64 offset, quint32 value) {
        const char bytes[4] = {char(value), char(value >> 8), char(value >> 16), char(value >> 24)};
        nor.seek(offset); nor.write(bytes, 4);
    };
    put(at + 8, 0xfeedbeef); put(at + 0x80, 0x50616c6d); put(at + 0x84, hal);
    nor.flush();
}

class FirmwareInputTest : public QObject {
    Q_OBJECT
 private slots:
    void txFiles() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto norPath = dir.filePath("synthetic-nor");
        const auto nandPath = dir.filePath("synthetic-nand");
        QVERIFY(!frondFirmware::validateTxFiles(norPath, nandPath).isEmpty());
        QFile nor(norPath), nand(nandPath);
        QVERIFY(nor.open(QIODevice::ReadWrite)); QVERIFY(nand.open(QIODevice::ReadWrite));
        QVERIFY(nor.resize(16u << 20)); QVERIFY(nand.resize(2112u * 64 * 1024));
        // Sparse synthetic files exercise geometry without firmware data.
        QVERIFY(frondFirmware::validateTxFiles(norPath, nandPath).isEmpty());
        QVERIFY(nand.resize(128u << 20)); // NAND without spare bytes.
        QVERIFY(!frondFirmware::validateTxFiles(norPath, nandPath).isEmpty());
        QVERIFY(nand.resize(2112u * 64 * 1024));
        QVERIFY(nor.resize((16u << 20) - 1));
        QVERIFY(!frondFirmware::validateTxFiles(norPath, nandPath).isEmpty());
        QVERIFY(!frondFirmware::validateTxFiles(dir.path(), nandPath).isEmpty());
        QVERIFY(!frondFirmware::validateTxFiles(nandPath, norPath).isEmpty());
    }
    void e2Files() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto norPath = dir.filePath("synthetic-nor"), nandPath = dir.filePath("synthetic-nand");
        QFile nor(norPath), nand(nandPath);
        QVERIFY(nor.open(QIODevice::ReadWrite)); QVERIFY(nand.open(QIODevice::ReadWrite));
        e2Nor(nor, 8 << 20, 0x20000);
        QVERIFY(nand.resize(34603008));
        Device device = Device::automatic;
        // NAND is optional for the E2; when given it must be the raw size.
        QVERIFY(frondFirmware::validateFiles(Device::e2, norPath, nandPath, &device).isEmpty());
        QCOMPARE(device, Device::e2);
        QVERIFY(frondFirmware::validateFiles(Device::e2, norPath, {}).isEmpty());
        QVERIFY(frondFirmware::validateFiles(Device::automatic, norPath, nandPath, &device).isEmpty());
        QCOMPARE(device, Device::e2);
        QCOMPARE(frondFirmware::detectDevice(norPath), Device::e2);
        QVERIFY(nand.resize(34603008 - 528));
        QVERIFY(!frondFirmware::validateFiles(Device::e2, norPath, nandPath).isEmpty());
        QVERIFY(!frondFirmware::validateFiles(Device::e2, norPath, dir.filePath("missing")).isEmpty());
        // Wrong NOR size or header place.
        e2Nor(nor, 16 << 20, 0x20000);
        QVERIFY(!frondFirmware::validateFiles(Device::e2, norPath, {}).isEmpty());
        e2Nor(nor, 8 << 20, 0x40000);
        QVERIFY(frondFirmware::validateFiles(Device::e2, norPath, {}).contains("0x20000"));
        e2Nor(nor, 8 << 20, 0);
        QVERIFY(!frondFirmware::validateFiles(Device::automatic, norPath, {}).isEmpty());
        // No header, or a header for another device.
        QVERIFY(nor.resize(0)); QVERIFY(nor.resize(8 << 20));
        QVERIFY(!frondFirmware::validateFiles(Device::e2, norPath, {}).isEmpty());
        QCOMPARE(frondFirmware::detectDevice(norPath), Device::automatic);
        QVERIFY(!frondFirmware::validateFiles(Device::automatic, norPath, {}).isEmpty());
        e2Nor(nor, 8 << 20, 0x20000, 0x446d4752);
        QVERIFY(!frondFirmware::validateFiles(Device::e2, norPath, {}).isEmpty());
    }
    void noFallbackBetweenDevices() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto e2Path = dir.filePath("e2-nor"), txPath = dir.filePath("tx-nor"), nandPath = dir.filePath("nand");
        QFile e2(e2Path), tx(txPath), nand(nandPath);
        QVERIFY(e2.open(QIODevice::ReadWrite)); QVERIFY(tx.open(QIODevice::ReadWrite)); QVERIFY(nand.open(QIODevice::ReadWrite));
        e2Nor(e2, 8 << 20, 0x20000);
        QVERIFY(tx.resize(16u << 20)); QVERIFY(nand.resize(2112u * 64 * 1024));
        QCOMPARE(frondFirmware::detectDevice(txPath), Device::tx);
        Device device = Device::automatic;
        QVERIFY(frondFirmware::validateFiles(Device::automatic, txPath, nandPath, &device).isEmpty());
        QCOMPARE(device, Device::tx);
        // A TX choice with an E2 NOR, and an E2 choice with a TX NOR, are refused.
        QVERIFY(frondFirmware::validateFiles(Device::tx, e2Path, nandPath).contains("Tungsten E2"));
        QVERIFY(!frondFirmware::validateFiles(Device::e2, txPath, {}).isEmpty());
        // The TX NAND size is not an E2 NAND, and the E2 NAND size is not a TX NAND.
        QVERIFY(!frondFirmware::validateFiles(Device::e2, e2Path, nandPath).isEmpty());
        QVERIFY(nand.resize(34603008));
        QVERIFY(!frondFirmware::validateFiles(Device::tx, txPath, nandPath).isEmpty());
        QVERIFY(!frondFirmware::validateFiles(Device::tx, txPath, {}).isEmpty());
    }
};
QTEST_GUILESS_MAIN(FirmwareInputTest)
#include "firmware-input.moc"
