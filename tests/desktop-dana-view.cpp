#include <QtTest>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSlider>
#include "DanaView.h"
#include "DanaViewControls.h"
#include "DanaViewController.h"
#include "DesktopClient.h"
#include "Dialogs.h"
#include "SessionStore.h"

static QString state(const frondDana::Preferences& prefs, const QString& host = "density=1 physical=on") {
    auto result = frondDana::argument(prefs);
    result = result.left(result.indexOf(" density="));
    return "dana-view: " + result + " resolved=" + (prefs.keyboard == "auto" ? "hidden" : prefs.keyboard) + ' ' + host;
}
class DanaViewTest : public QObject {
    Q_OBJECT
 private slots:
    void states() {
        frondDana::Preferences original{"full", "1.5", "pan", 543.125, 159.5}, result;
        double density = 1;
        QVERIFY(frondDana::parseState(state(original, "density=2 physical=off output-scale-x=2 output-scale-y=2 page=fn"), &result, &density));
        QCOMPARE(density, 2.0);
        QVERIFY(result == original);
        for (const auto& malformed : {QString(), state(original).mid(10), state(original) + " pan-x=3",
             state(original).replace("543.125", "nan"), state(original).replace("159.5", "161"),
             state(original).replace("543.125", "-1"), state(original).replace("zoom=1.5", "zoom=4"),
             state(original).replace("density=1", "density=0"), state(original).replace("physical=on", "physical=maybe"),
             state(original).replace("resolved=full", "resolved=auto"), state(original) + " unknown=1", state(original) + " page=missing"}) {
            result = original;
            QVERIFY2(!frondDana::parseState(malformed, &result), qPrintable(malformed));
            QVERIFY(result == original);
        }
        QVERIFY(frondDana::argument(original, 2).contains("density=2 physical=on"));
    }
    void preferences() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto path = dir.filePath("machine.frond");
        frondSessions::Preferences prefs;
        prefs.danaView = {"compact", "fit", "pan", 400, 35.5}; prefs.scale = 7; prefs.smooth = true;
        QVERIFY(frondSessions::writePreferences(path, prefs).isEmpty());
        frondSessions::Preferences loaded;
        QVERIFY(frondSessions::readPreferences(path, &loaded));
        QVERIFY(loaded.danaView == prefs.danaView); QCOMPARE(loaded.scale, 7); QVERIFY(loaded.smooth);
        QFile file(frondSessions::preferencesPath(path)); QVERIFY(file.open(QIODevice::ReadOnly));
        auto json = QJsonDocument::fromJson(file.readAll()).object(); file.close();
        const auto view = json.value("danaView").toObject();
        QVERIFY(!view.contains("density") && !view.contains("physical"));
        auto write = [&] {
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write(QJsonDocument(json).toJson()); file.close();
        };
        json.remove("danaView"); write();
        loaded.danaView = prefs.danaView;
        QVERIFY(frondSessions::readPreferences(path, &loaded)); QVERIFY(loaded.danaView == frondDana::Preferences{});
        auto malformed = view; malformed.insert("zoom", "4"); json.insert("danaView", malformed); write();
        QVERIFY(frondSessions::readPreferences(path, &loaded)); QVERIFY(loaded.danaView == frondDana::Preferences{});
        QCOMPARE(loaded.scale, 7); QVERIFY(loaded.smooth);
    }
    void controls() {
        DanaViewControls controls;
        QSignalSpy changed(&controls, &DanaViewControls::changed);
        frondDana::Preferences prefs{"full", "3", "pan", 560, 160};
        controls.setValues(prefs); QCOMPARE(changed.count(), 0); QVERIFY(controls.values() == prefs);
        controls.resize(250, 400); controls.resize(800, 200); QVERIFY(controls.values() == prefs);
        auto* keyboard = controls.findChild<QComboBox*>("danaKeyboard"); QVERIFY(keyboard);
        keyboard->setCurrentIndex(keyboard->findData("hidden")); QCOMPARE(changed.count(), 1);
        QCOMPARE(controls.values().zoom, QString("3")); QCOMPARE(controls.values().panX, 560.0);
        frondSessions::Preferences session; session.scale = 7; session.danaView = prefs;
        MachineSettings machine(session, false); machine.setDevice(frondFirmware::Device::tx); machine.show();
        QVERIFY(!machine.danaView->isVisible()); QVERIFY(machine.windowScale->isVisible());
        auto values = machine.values(); QCOMPARE(values.scale, 7); QVERIFY(values.danaView == prefs);
        for (const auto& profile : deviceProfiles5) {
            if (QString::fromLatin1(profile.name) != "alphasmart-dana") continue;
            machine.setDevice(static_cast<frondFirmware::Device>(profile.deviceType));
            QVERIFY(machine.danaView->isVisible()); QVERIFY(!machine.windowScale->isVisible());
            auto* selection = machine.danaView->findChild<QComboBox*>("danaKeyboard");
            selection->setCurrentIndex(selection->findData("compact"));
            values = machine.values(); QCOMPARE(values.danaView.keyboard, QString("compact")); QCOMPARE(values.scale, 7);
            QCOMPARE(values.danaView.zoom, prefs.zoom);
        }
        prefs.panX = 123.123456789; controls.setValues(prefs);
        keyboard->setCurrentIndex(keyboard->findData("compact")); QCOMPARE(controls.values().panX, prefs.panX);
    }
    void runtime() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QFile fake(dir.filePath("core.py")); QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write(R"PY(import socket,sys,time
s=socket.socket(socket.AF_UNIX);s.connect(sys.argv[sys.argv.index('--desktop-socket')+1])
p=dict(keyboard='hidden',zoom='2',pointer='touch',**{'pan-x':'0','pan-y':'0', 'density':'1','physical':'on'})
def state():
    return ('dana-view: '+' '.join(k+'='+v for k,v in p.items())+' resolved='+('hidden' if p['keyboard']=='auto' else p['keyboard'])).encode()
def event(text): s.sendall(b'0\tevent\tdana-view\t'+text.hex().encode()+b'\n')
s.sendall(b'0\tevent\tready\t\n')
for line in s.makefile('rb'):
    i,cmd,arg=line.rstrip(b'\n').split(b'\t');arg=bytes.fromhex(arg.decode()).decode();text=b'done';status=b'ok'
    with open(sys.argv[0]+'.commands','ab') as f: f.write(cmd+b' '+arg.encode()+b'\n')
    if cmd==b'dana-view':
        next=dict(x.split('=') for x in arg.split())
        if next['keyboard']=='compact': status=b'error';text=b'refused'
        else: p=next;event(state());text=state()
    elif cmd==b'dana-view-state': text=state()
    elif cmd==b'wait': time.sleep(.08)
    elif cmd==b'gesture': p['pan-x']='128';p['zoom']='3';event(state())
    elif cmd==b'bad': event(b'dana-view: zoom=broken')
    s.sendall(i+b'\t'+status+b'\tview\t'+text.hex().encode()+b'\n')
    if cmd==b'quit': break
)PY"); fake.close();
        DesktopClient client; DanaViewController controller(client);
        QSignalSpy ready(&client, &DesktopClient::started), replies(&client, &DesktopClient::result), stopped(&client, &DesktopClient::finished);
        QSignalSpy errors(&controller, &DanaViewController::error), socketErrors(&client, &DesktopClient::error);
        QList<frondDana::Preferences> saved;
        bool saveFails = false;
        controller.save = [&](const frondDana::Preferences& prefs) {
            if (saveFails) return QString("Couldn't save the machine settings.");
            saved.append(prefs); return QString();
        };
        controller.configure(true, {}, 1);
        QVERIFY(client.start("/usr/bin/python3", {fake.fileName()})); QTRY_COMPARE(ready.count(), 1);
        QTRY_COMPARE(replies.count(), 1); QCOMPARE(saved.size(), 0);
        QVERIFY(client.request("wait"));
        frondDana::Preferences first{"full", "1.5", "pan", 12, 2}; controller.choose(first);
        auto second = first; second.zoom = "fit"; second.panX = 45; controller.choose(second);
        QTRY_COMPARE(saved.size(), 1); QVERIFY(saved.last() == second); QVERIFY(controller.values() == second);
        QTRY_VERIFY(client.available());
        QFile trace(fake.fileName() + ".commands"); QVERIFY(trace.open(QIODevice::ReadOnly));
        auto commands = trace.readAll(); trace.close(); QCOMPARE(commands.count("dana-view "), 1);
        QVERIFY(client.request("gesture")); QTRY_COMPARE(saved.size(), 2); QCOMPARE(controller.values().panX, 128.0);
        QTRY_VERIFY(client.available()); QVERIFY(trace.open(QIODevice::ReadOnly));
        QCOMPARE(trace.readAll().count("dana-view "), 1); trace.close();
        const auto accepted = controller.values();
        auto refused = accepted; refused.keyboard = "compact"; controller.choose(refused);
        QTRY_COMPARE(errors.count(), 1); QVERIFY(controller.values() == accepted); QCOMPARE(saved.size(), 2);
        QTRY_VERIFY(client.available());
        saveFails = true;
        auto unsaved = accepted; unsaved.panX = 99; controller.choose(unsaved);
        QTRY_COMPARE(errors.count(), 2); QTRY_VERIFY(client.available());
        QTRY_VERIFY(([&] { if (!trace.open(QIODevice::ReadOnly)) return false; const auto count = trace.readAll().count("dana-view "); trace.close(); return count == 4; })());
        QVERIFY(controller.values() == accepted); QCOMPARE(saved.size(), 2); saveFails = false;
        QTRY_VERIFY(client.available()); QVERIFY(client.request("bad")); QTRY_COMPARE(errors.count(), 3);
        QVERIFY(controller.values() == accepted); QCOMPARE(saved.size(), 2);
        QTRY_VERIFY(client.available());
        controller.setDensity(2); QTRY_VERIFY(client.available());
        QTRY_VERIFY(([&] { if (!trace.open(QIODevice::ReadOnly)) return false; auto bytes = trace.readAll(); trace.close(); return bytes.contains("density=2 physical=on"); })());
        QCOMPARE(saved.size(), 2);
        QTRY_VERIFY(client.available());
        auto last = accepted; last.pointer = "touch"; controller.choose(last);
        QVERIFY(controller.finishChanges()); QTRY_COMPARE(saved.size(), 3);
        QTRY_VERIFY(client.available()); QVERIFY(!controller.finishChanges());
        // Keep a newer choice made while a setter is already in flight.
        auto inflight = last; inflight.zoom = "1"; controller.choose(inflight);
        QVERIFY(controller.finishChanges());
        controller.setDensity(3);
        auto newer = inflight; newer.zoom = "1.5"; controller.choose(newer);
        QTRY_COMPARE(saved.size(), 5); QVERIFY(saved.at(3) == inflight); QVERIFY(saved.last() == newer);
        QTRY_VERIFY(client.available());
        QVERIFY(trace.open(QIODevice::ReadOnly)); commands = trace.readAll(); trace.close();
        QVERIFY(commands.contains("zoom=1.5 pan-x=128 pan-y=2 pointer=touch density=3 physical=on"));
        // A new choice during failed-save recovery follows the rollback.
        saveFails = true;
        auto afterFailure = newer; afterFailure.zoom = "3";
        const auto errorConnection = connect(&controller, &DanaViewController::error, &controller, [&](const QString&) {
            saveFails = false; controller.choose(afterFailure);
        });
        auto failing = newer; failing.zoom = "fit"; controller.choose(failing);
        QTRY_COMPARE(saved.size(), 6); QVERIFY(saved.last() == afterFailure); QVERIFY(controller.values() == afterFailure);
        disconnect(errorConnection);
        QTRY_VERIFY(client.available()); controller.suspend(); QVERIFY(client.request("quit")); QTRY_COMPARE(stopped.count(), 1);
        QCOMPARE(socketErrors.count(), 0);
        // Unrelated machines do not query or dispatch Dana preferences.
        replies.clear(); controller.configure(false, second, 1);
        QVERIFY(client.start("/usr/bin/python3", {fake.fileName()})); QTRY_COMPARE(ready.count(), 2);
        QVERIFY(client.request("wait")); QTRY_COMPARE(replies.count(), 1);
        QCOMPARE(replies.first().at(0).toString(), QString("wait"));
        QVERIFY(client.request("quit")); QTRY_COMPARE(stopped.count(), 2);
    }
};
QTEST_MAIN(DanaViewTest)
#include "desktop-dana-view.moc"
