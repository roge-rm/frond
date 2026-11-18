// The Battery setting's text and the Linux power_supply reader.
#include <QDir>
#include <QFile>
#include <QTest>
#include "HostBattery.h"

using namespace frondBattery;

class BatteryUi : public QObject {
    Q_OBJECT
    static void write(const QString& path, const QString& text) {
        QDir().mkpath(QFileInfo(path).path());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(text.toUtf8() + "\n");
    }
 private slots:
    void specText() {
        QCOMPARE(spec("max", 40, false, {}), QString("max battery"));
        QCOMPARE(spec("max", 40, true, {}), QString("max charging"));
        QCOMPARE(spec("level", 40, true, {}), QString("level 40 charging"));
        QCOMPARE(spec("level", 140, false, {}), QString("level 100 battery"));
        QCOMPARE(spec("level", -3, false, {}), QString("level 0 battery"));
        // Host mode takes the host's values and ignores the machine's own switch.
        QCOMPARE(spec("host", 40, true, {63, false}), QString("host 63 battery"));
        QCOMPARE(spec("host", 40, false, {63, true}), QString("host 63 charging"));
        QCOMPARE(spec("other", 40, false, {}), QString("max battery"));
        QVERIFY(validMode("host"));
        QVERIFY(!validMode("other"));
    }
    void sysfsLaptop() {
        const auto root = QDir::currentPath() + "/battery-ui-test/laptop";
        QDir(root).removeRecursively();
        write(root + "/AC/type", "Mains");
        write(root + "/AC/online", "0");
        write(root + "/BAT0/type", "Battery");
        write(root + "/BAT0/capacity", "57");
        write(root + "/BAT0/status", "Discharging");
        auto state = parsePowerSupply(root);
        QCOMPARE(state.percent, 57);
        QVERIFY(!state.charging);
        write(root + "/AC/online", "1");
        write(root + "/BAT0/status", "Not charging");
        state = parsePowerSupply(root);
        QCOMPARE(state.percent, 57);
        QVERIFY(state.charging);
        write(root + "/AC/online", "0");
        write(root + "/BAT0/status", "Charging");
        QVERIFY(parsePowerSupply(root).charging);
    }
    void sysfsSkipsDeviceBattery() {
        const auto root = QDir::currentPath() + "/battery-ui-test/mouse";
        QDir(root).removeRecursively();
        write(root + "/hid-1/type", "Battery");
        write(root + "/hid-1/scope", "Device");
        write(root + "/hid-1/capacity", "20");
        write(root + "/BAT1/type", "Battery");
        write(root + "/BAT1/capacity", "88");
        write(root + "/BAT1/status", "Discharging");
        QCOMPARE(parsePowerSupply(root).percent, 88);
    }
    void sysfsNoBattery() {
        const auto root = QDir::currentPath() + "/battery-ui-test/desktop";
        QDir(root).removeRecursively();
        write(root + "/AC/type", "Mains");
        write(root + "/AC/online", "1");
        auto state = parsePowerSupply(root);
        QCOMPARE(state.percent, 100);
        QVERIFY(state.charging);
        state = parsePowerSupply(QDir::currentPath() + "/battery-ui-test/missing");
        QCOMPARE(state.percent, 100);
        QVERIFY(state.charging);
    }
    void cleanupTestCase() { QDir(QDir::currentPath() + "/battery-ui-test").removeRecursively(); }
};
QTEST_APPLESS_MAIN(BatteryUi)
#include "battery-ui.moc"
