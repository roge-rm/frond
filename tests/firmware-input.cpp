#include <QtTest>
#include <QTemporaryDir>
#include "FirmwareInput.h"

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
};
QTEST_GUILESS_MAIN(FirmwareInputTest)
#include "firmware-input.moc"
