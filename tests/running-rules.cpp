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
        // Storage shows on every machine: Internal at least.
        QVERIFY(buttons(iiix, s).card && buttons(iiix, s).cardEnabled);
        s.ready = false;
        QVERIFY(!buttons(tx, s).powerEnabled && !buttons(tx, s).soundEnabled);
        s.keyboardAttached = true;
        QVERIFY(buttons(tx, s).keyboard);
    }
    void databaseFilesAreNamedByKind() {
        QCOMPARE(databaseFile({"Memos-PMem", "appl", "memo", true}), QString("Memos-PMem.prc"));
        QCOMPARE(databaseFile({"MemoDB", "DATA", "memo", false}), QString("MemoDB.pdb"));
        QCOMPARE(databaseFile({"a/b:c", "DATA", "x", false}), QString("a_b_c.pdb"));
        QCOMPARE(databaseFile({"..", "DATA", "x", false}), QString("Database.pdb"));
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
        QCOMPARE(subtitle(traitsFor(named("tapwave-zodiac-2")), true, "Tapwave Zodiac 2"), QString("Fast"));
        QCOMPARE(subtitle(traitsFor(named("tapwave-zodiac-2")), false, "Tapwave Zodiac 2"), QString("Exact"));
    }
    // The Zodiac's unfinished hardware is listed and greyed, and it has no hard reset yet.
    void zodiacRows() {
        const auto t = traitsFor(named("tapwave-zodiac-2"));
        State s; s.ready = true; s.network = "off";
        QVERIFY(!t.runs68k && !t.guestServices && t.cardSlot && t.powerKey);
        for (Id id : {Id::Keyboard, Id::Microphone, Id::Network, Id::HardReset}) QVERIFY(!row(t, s, id).enabled);
        for (Id id : {Id::Reset, Id::Save, Id::SaveStop, Id::Screenshot}) QVERIFY(row(t, s, id).enabled);
        QVERIFY(row(traitsFor(named("palm-tx")), s, Id::HardReset).enabled);
    }
    // The panel's Hard reset button goes off with the other running controls, on every device.
    void hardResetButtonFollowsTheControls() {
        for (auto* id : devices()) {
            const auto t = traitsFor(named(id));
            QVERIFY2(!hardResetEnabled(false, t), id);
            QCOMPARE(hardResetEnabled(true, t), t.guestServices);
        }
        QVERIFY(hardResetEnabled(true, traitsFor(named("palm-tx"))));
        QVERIFY(!hardResetEnabled(true, traitsFor(named("tapwave-zodiac-2"))));
    }
    void machineStopIsTheFailureText() {
        QCOMPARE(machineStopText("device: tapwave-zodiac-2\nzodiac: the machine stopped: x, after 5 instructions\n"
                                 "Tapwave Zodiac 2: the machine stopped: unsupported-peripheral (side-effect) at pc 200885fc\n"),
                 QString("The Tapwave Zodiac 2 stopped: unsupported-peripheral (side-effect) at pc 200885fc."));
        QVERIFY(machineStopText("device: palm-tx\nsave: the device state snapshot failed\n").isEmpty());
        QVERIFY(machineStopText(": the machine stopped: \n").isEmpty());
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
    void databaseListParsing() {
        auto hex = [](const char* text) { return QString::fromLatin1(QByteArray(text).toHex()); };
        const auto text = hex("appl") + " " + hex("memo") + " 00000001 " + hex("Memos") + "\n" +
                          hex("DATA") + " " + hex("memo") + " 00000000 " + hex("Memo Data") + "\n" +
                          hex("appl") + " " + hex("abcd") + " 00000001 " + hex("alpha") + "\n" +
                          "bad line\n" + hex("appl") + " " + hex("zzzz") + " 0001 " + hex("odd") + "x\n";
        const auto all = parseDatabases(text);
        QCOMPARE(all.size(), size_t(3));
        QCOMPARE(all[0].name, QString("alpha"));
        QCOMPARE(all[1].name, QString("Memo Data"));
        const auto apps = applications(all);
        QCOMPARE(apps.size(), size_t(2));
        const auto data = dataOf(all, apps[1]);
        QCOMPARE(data.size(), size_t(1));
        QCOMPARE(data[0].name, QString("Memo Data"));
        QVERIFY(dataOf(all, apps[0]).empty());
    }
    void shortcutNames() {
        QCOMPARE(*shortcutFor("ctrl+s"), Shortcut::Save);
        QCOMPARE(*shortcutFor("ctrl+shift+s"), Shortcut::SaveCopy);
        QCOMPARE(*shortcutFor("ctrl+3"), Shortcut::Display);
        QCOMPARE(*shortcutFor("f12"), Shortcut::Screenshot);
        QVERIFY(!shortcutFor("ctrl+z") && !shortcutFor("") && !shortcutFor("CTRL+S"));
        QStringList names, keys;
        for (const auto& key : shortcutKeys()) { QVERIFY(!names.contains(key.name) && !keys.contains(key.keys)); names << key.name; keys << key.keys; }
        // The core's name is the Qt sequence in lower case.
        for (const auto& key : shortcutKeys()) QCOMPARE(QString(key.keys).toLower(), QString(key.name));
    }
    // Rows that show a key beside them; every key that has a row is found, and Install, Quit and
    // the panels' keys without a row are not.
    void rowKeys() {
        QCOMPARE(*rowFor(Shortcut::Save), Id::Save);
        QCOMPARE(*rowFor(Shortcut::HideBar), Id::HideBar);
        QCOMPARE(*rowFor(Shortcut::FullScreen), Id::FullScreen);
        QVERIFY(!rowFor(Shortcut::Install) && !rowFor(Shortcut::Quit) && !rowFor(Shortcut::Sound) && !rowFor(Shortcut::Card));
    }
    void embedRules() {
        QVERIFY(embedPossible("xcb", false));
        QVERIFY(!embedPossible("offscreen", false) && !embedPossible("wayland", false) && !embedPossible("", false));
        QVERIFY(embedPossible("offscreen", true));
        QCOMPARE(parseWindowId("x11 12345"), quint64(12345));
        QCOMPARE(parseWindowId("x11 12345\n"), quint64(12345));
        QCOMPARE(parseWindowId("x11 140724472224432"), quint64(140724472224432ULL));
        QCOMPARE(parseWindowId("unsupported"), quint64(0));
        QCOMPARE(parseWindowId("x11"), quint64(0));
        QCOMPARE(parseWindowId("x11 abc"), quint64(0));
        QCOMPARE(parseWindowId("wayland 5"), quint64(0));
        QCOMPARE(parseWindowId(""), quint64(0));
    }
    void fullScreenRules() {
        Traits t; State s; s.ready = true;
        QVERIFY(!row(t, s, Id::FullScreen).enabled);
        QCOMPARE(row(t, s, Id::FullScreen).label, QString("Full screen"));
        s.embedded = true;
        QVERIFY(row(t, s, Id::FullScreen).enabled);
        s.fullScreen = true;
        QCOMPARE(row(t, s, Id::FullScreen).label, QString("Leave full screen"));
        QCOMPARE(fullScreenHideMs, 3000);
        // The bar stays while the pointer is on it, a menu from it is open or a panel is.
        QVERIFY(!barStaysInFullScreen(false, false, false));
        QVERIFY(barStaysInFullScreen(true, false, false));
        QVERIFY(barStaysInFullScreen(false, true, false));
        QVERIFY(barStaysInFullScreen(false, false, true));
        // The panel goes over the right edge, under the bar when that shows.
        QCOMPARE(overlayPanel({1280, 800}, 36), QRect(1280 - panelWidth, 36, panelWidth, 764));
        QCOMPARE(overlayPanel({1280, 800}, 0), QRect(1280 - panelWidth, 0, panelWidth, 800));
        QCOMPARE(overlayPanel({200, 20}, 36).height(), 0);
    }
    void keyTargets() {
        // A press on the Palm gives it the keys, a press on a Frond widget takes them, anything
        // else (the bar, the menu bar, another application) leaves the choice.
        QVERIFY(palmWantsKeys(false, Press::OnPalm));
        QVERIFY(!palmWantsKeys(true, Press::OnFrond));
        QVERIFY(palmWantsKeys(true, Press::None));
        QVERIFY(!palmWantsKeys(false, Press::None));
        // Typing in a panel field with the pointer over the Palm: the window manager put the focus
        // on Frond's top-level, which holds the Palm, so it moves to the key window.
        QCOMPARE(keyTarget(FocusAt::HoldsPalm, false, false), KeyTarget::Frond);
        QCOMPARE(keyTarget(FocusAt::Frond, false, false), KeyTarget::Keep);
        QCOMPARE(keyTarget(FocusAt::Palm, false, false), KeyTarget::Frond);
        // Back from another application (Alt-Tab): the top-level gets the focus, and it goes to
        // whichever had the keys before, the Palm or the panel.
        QCOMPARE(keyTarget(FocusAt::HoldsPalm, true, false), KeyTarget::Palm);
        QCOMPARE(keyTarget(FocusAt::Palm, true, false), KeyTarget::Keep);
        QCOMPARE(keyTarget(FocusAt::Frond, true, false), KeyTarget::Palm);
        // Click on the Palm after coming back: the press sets the choice and the focus follows.
        QCOMPARE(keyTarget(FocusAt::HoldsPalm, palmWantsKeys(false, Press::OnPalm), false), KeyTarget::Palm);
        // Nothing moves while another application or a dialog has the focus, or a menu is open.
        QCOMPARE(keyTarget(FocusAt::Elsewhere, true, false), KeyTarget::Keep);
        QCOMPARE(keyTarget(FocusAt::Elsewhere, false, false), KeyTarget::Keep);
        QCOMPARE(keyTarget(FocusAt::HoldsPalm, true, true), KeyTarget::Keep);
        QCOMPARE(keyTarget(FocusAt::HoldsPalm, false, true), KeyTarget::Keep);
    }
    void topEdgeReveal() {
        // The hidden bar comes back with the pointer on the top two rows of the full-screen area.
        const QRect area(0, 0, 1280, 800);
        QVERIFY(revealBar({640, 0}, area));
        QVERIFY(revealBar({0, 1}, area));
        QVERIFY(!revealBar({640, 2}, area));
        QVERIFY(!revealBar({640, 400}, area));
        // A second monitor to the right: only its own top edge counts.
        const QRect right(1280, 0, 1920, 1080);
        QVERIFY(revealBar({1300, 0}, right));
        QVERIFY(!revealBar({100, 0}, right));
        QVERIFY(!revealBar({1300, -1}, right));
    }
    // A panel opens at the right of the Palm, so the Palm keeps its width; Size scales the Palm.
    void windowAndPanelSizes() {
        QCOMPARE(windowWidth(320, false), 320);
        QCOMPARE(windowWidth(320, true), 320 + panelWidth);
        QCOMPARE(scaledPalm({320, 564}, 4, 8), QSize(640, 1128));
        QCOMPARE(scaledPalm({320, 564}, 8, 4), QSize(160, 282));
        QCOMPARE(scaledPalm({320, 564}, 4, 5), QSize(400, 705));
        QCOMPARE(scaledPalm({320, 564}, 0, 5), QSize(320, 564));
    }
};
QTEST_APPLESS_MAIN(RunningRulesTest)
#include "running-rules.moc"
