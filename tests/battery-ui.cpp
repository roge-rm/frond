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
    void noCharging() {
        // A machine that can't charge is always told battery, in every mode.
        QCOMPARE(spec("host", 0, true, {63, true}, false), QString("host 63 battery"));
        QCOMPARE(spec("max", 0, true, {}, false), QString("max battery"));
        QCOMPARE(spec("level", 20, true, {}, false), QString("level 20 battery"));
        QCOMPARE(spec("host", 0, false, {63, true}, true), QString("host 63 charging"));
    }
    void windowsFlags() {
        QVERIFY(windowsCharging(1, 0));
        QVERIFY(!windowsCharging(0, 8));
        QVERIFY(!windowsCharging(255, 1));
        QVERIFY(windowsCharging(255, 8));
    }
    void sysfsTwoBatteries() {
        const auto root = QDir::currentPath() + "/battery-ui-test/two";
        QDir(root).removeRecursively();
        write(root + "/BAT0/type", "Battery");
        write(root + "/BAT0/energy_now", "10000000");
        write(root + "/BAT0/energy_full", "40000000");
        write(root + "/BAT0/capacity", "25");
        write(root + "/BAT0/status", "Discharging");
        write(root + "/BAT1/type", "Battery");
        write(root + "/BAT1/charge_now", "3000000");
        write(root + "/BAT1/charge_full", "3000000");
        write(root + "/BAT1/status", "Discharging");
        // Energy and charge are summed as numbers, so the big battery weighs most: (10 + 3) / (40 + 3).
        QCOMPARE(parsePowerSupply(root).percent, 30);
    }
    void sysfsCapacityOnlyAverage() {
        const auto root = QDir::currentPath() + "/battery-ui-test/capacities";
        QDir(root).removeRecursively();
        write(root + "/BAT0/type", "Battery");
        write(root + "/BAT0/capacity", "40");
        write(root + "/BAT1/type", "Battery");
        write(root + "/BAT1/capacity", "60");
        const auto state = parsePowerSupply(root);
        QCOMPARE(state.percent, 50);
        QVERIFY(!state.charging);
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
