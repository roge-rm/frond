#include <QtTest>
#include "RunningRules.h"

using namespace runningRules;
using frondFirmware::Device;

static Device named(const char* id) {
    const auto device = frondFirmware::deviceFromId(id);
    return device;
}
static std::vector<Id> ids(const Traits& t, const State& s = {}) {
    std::vector<Id> out;
    for (const auto& row : rows(t, s)) out.push_back(row.id);
    return out;
}
static bool has(const std::vector<Id>& list, Id id) { return std::find(list.begin(), list.end(), id) != list.end(); }
static Row row(const Traits& t, const State& s, Id id) {
    for (const auto& r : rows(t, s)) if (r.id == id) return r;
    return {};
}

class RunningRulesTest : public QObject {
    Q_OBJECT
    // The four devices the tests cover.
    static QList<const char*> devices() { return {"palm-tx", "sony-clie-tg50", "palm-iiix", "palm-e2"}; }
 private slots:
    void devicesAreKnown() {
        for (auto* id : devices()) QVERIFY2(named(id) != Device::automatic, id);
    }
    void everyDeviceGetsTheFixedRowsInOrder() {
        const std::vector<Id> fixed{Id::Display, Id::Connections, Id::Screenshot, Id::HideBar, Id::FullScreen, Id::Save,
                                    Id::SaveCopy, Id::Log, Id::Reset, Id::HardReset, Id::SaveStop};
        for (auto* id : devices()) {
            const auto list = ids(traitsFor(named(id)));
            std::vector<Id> found;
            for (auto i : list) if (has(fixed, i)) found.push_back(i);
            QVERIFY2(found == fixed, id);
        }
    }
    void holdShowsOnlyWhereTheDeviceHasIt() {
        QVERIFY(has(ids(traitsFor(named("sony-clie-tg50"))), Id::Hold));
        for (auto* id : {"palm-tx", "palm-iiix", "palm-e2"}) QVERIFY2(!has(ids(traitsFor(named(id))), Id::Hold), id);
    }
    void holdFollowsTheCoreAndBusy() {
        const auto t = traitsFor(named("sony-clie-tg50"));
        State s; s.ready = true;
        QVERIFY(!row(t, s, Id::Hold).enabled);
        s.hold = true;
        QVERIFY(row(t, s, Id::Hold).enabled && *row(t, s, Id::Hold).on);
        s.hold = false;
        QVERIFY(row(t, s, Id::Hold).enabled && !*row(t, s, Id::Hold).on);
        s.ready = false;
        QVERIFY(!row(t, s, Id::Hold).enabled);
    }
    void a68kDeviceListsOnlyItsHardware() {
        const auto t = traitsFor(named("palm-iiix"));
        QVERIFY(t.runs68k);
        const auto list = ids(t);
        QCOMPARE(has(list, Id::Keyboard), t.keyboard);
        QCOMPARE(has(list, Id::Microphone), t.microphone);
        QCOMPARE(has(list, Id::Network), t.network);
    }
    void otherDevicesGreyOutWhatIsMissing() {
        for (auto* id : {"palm-tx", "palm-e2"}) {
            const auto t = traitsFor(named(id));
            QVERIFY(!t.runs68k);
            State s; s.ready = true; s.network = "disabled";
            QCOMPARE(row(t, s, Id::Keyboard).enabled, t.keyboard);
            QCOMPARE(row(t, s, Id::Microphone).enabled, t.microphone);
            QCOMPARE(row(t, s, Id::Network).enabled, t.network);
            QVERIFY(has(ids(t), Id::Network));
        }
    }
    void keyboardNeedsTheCoreAndFreeInfraredPort() {
        const auto t = traitsFor(named("palm-tx"));
        QVERIFY(t.keyboard);
        State s;
        QVERIFY(!row(t, s, Id::Keyboard).enabled);
        s.ready = true;
        QVERIFY(row(t, s, Id::Keyboard).enabled);
        s.keyboardAttached = true;
        QVERIFY(*row(t, s, Id::Keyboard).on);
        s.beamOff = false;
        QVERIFY(!row(t, s, Id::Keyboard).enabled);
    }
    void networkShowsItsModeOnceKnown() {
        const auto t = traitsFor(named("palm-tx"));
        State s; s.ready = true;
        QVERIFY(!row(t, s, Id::Network).enabled);
        s.network = "internet";
        QCOMPARE(row(t, s, Id::Network).value, QString("internet"));
        QVERIFY(row(t, s, Id::Network).enabled);
    }
    void microphoneStates() {
        QCOMPARE(microphoneValue("on"), QString("On"));
        QCOMPARE(microphoneValue("pending"), QString("On"));
        QCOMPARE(microphoneValue("off"), QString("Off"));
        QCOMPARE(microphoneValue("off Permission denied"), QString("Permission denied"));
        const auto t = traitsFor(named("palm-tx"));
        State s; s.ready = true; s.microphone = "off Permission denied";
        QVERIFY(!*row(t, s, Id::Microphone).on);
        QCOMPARE(row(t, s, Id::Microphone).value, QString("Permission denied"));
    }
    void onlyHardResetIsRedAndGroupsOnlyGrow() {
        for (auto* id : devices()) {
            const auto list = rows(traitsFor(named(id)), {});
            int destructive = 0, group = 0;
            for (const auto& r : list) {
                if (r.destructive) { destructive++; QVERIFY(r.id == Id::HardReset); }
                QVERIFY(r.group >= group);
                group = r.group;
            }
            QCOMPARE(destructive, 1);
        }
    }
    void saveAndStopFollowsTheCore() {
        const auto t = traitsFor(named("palm-tx"));
        State s;
        QVERIFY(!row(t, s, Id::SaveStop).enabled);
        s.ready = true;
        QVERIFY(row(t, s, Id::SaveStop).enabled);
        // The display and log rows only scroll or open a window, so they work while the core is busy.
        s.ready = false;
        QVERIFY(row(t, s, Id::Display).enabled && row(t, s, Id::Log).enabled && !row(t, s, Id::Screenshot).enabled);
    }
    void hideBarSaysWhatItDoes() {
        const auto t = traitsFor(named("palm-tx"));
        State s;
        QCOMPARE(row(t, s, Id::HideBar).label, QString("Hide bar"));
        s.barShown = false;
        QCOMPARE(row(t, s, Id::HideBar).label, QString("Show bar"));
    }
    void powerAndCardFollowTheDevice() {
        const auto tx = traitsFor(named("palm-tx")), iiix = traitsFor(named("palm-iiix"));
        State s; s.ready = true;
        QVERIFY(buttons(tx, s).power && buttons(tx, s).powerEnabled);
        QVERIFY(buttons(tx, s).card);
        QVERIFY(!buttons(iiix, s).card && !buttons(iiix, s).cardEnabled);
        s.ready = false;
        QVERIFY(!buttons(tx, s).powerEnabled && !buttons(tx, s).soundEnabled);
        s.keyboardAttached = true;
        QVERIFY(buttons(tx, s).keyboard);
    }
    void cardIconStates() {
        QCOMPARE(cardIcon(false, true, true, true), CardIcon::None);
        QCOMPARE(cardIcon(true, false, true, true), CardIcon::Out);
        QCOMPARE(cardIcon(true, true, false, false), CardIcon::Out);
        QCOMPARE(cardIcon(true, true, true, true), CardIcon::In);
        QCOMPARE(cardIcon(true, true, true, false), CardIcon::Ejected);
    }
    void subtitleIsTheEngineOrTheDevice() {
        QCOMPARE(subtitle(traitsFor(named("palm-tx")), false, "Palm TX"), QString("Exact"));
        QCOMPARE(subtitle(traitsFor(named("palm-tx")), true, "Palm TX"), QString("Fast"));
        QCOMPARE(subtitle(traitsFor(named("palm-iiix")), true, "Palm IIIx"), QString("Palm IIIx"));
    }
    void screenshotNamesAreFileSafe() {
        QCOMPARE(screenshotName("Palm TX", "20261107-133000"), QString("Frond Palm TX 20261107-133000.png"));
        QCOMPARE(screenshotName("a/b", "1"), QString("Frond a_b 1.png"));
        QCOMPARE(screenshotName(" ", "1"), QString("Frond Palm 1.png"));
    }
    void replayLines() {
        QCOMPARE(snapshotReplay("/a b/c\"d.png"), QString("0 snapshot \"/a b/c\\\"d.png\"\n"));
        QCOMPARE(powerReplay(), QString("0 key-down power\n80 key-up power\n"));
    }
};
QTEST_APPLESS_MAIN(RunningRulesTest)
#include "running-rules.moc"
