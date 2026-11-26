// The desktop's machine and card stores (SessionStore.h, CardStore.h): the input is written as a
// machine's settings.json, a card entry's card.json, a slot file and the .frond itself, then each
// is read back the way the launcher and Cards window do. QtCore only.
#include <QDir>
#include <QFile>

#include "CardStore.h"
#include "Scratch.h"

namespace {
void Put(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(bytes);
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    static unsigned runs = 0;
    if (size > 65536) return 0;
    const auto root = QString::fromStdString(FuzzScratch("qt-session-store", runs)) + "/" + QString::number(runs);
    const QByteArray input(reinterpret_cast<const char*>(bytes), int(size));
    const auto id = QStringLiteral("0b6f2c1e-4a5d-4f3e-9c7b-1a2b3c4d5e6f");
    const auto machine = root + "/Machine", card = root + "/library/" + id;
    QDir().mkpath(machine);
    QDir().mkpath(card);
    const auto frond = machine + "/Machine.frond";
    Put(frond, input);
    Put(frondSessions::preferencesPath(frond), input);
    Put(card + "/card.json", input);
    Put(machine + "/slot.json", input);
    Put(machine + "/slot2.json", input);
    Put(machine + "/sd.frondsd", input);

    frondSessions::Preferences prefs;
    if (frondSessions::readPreferences(frond, &prefs)) frondSessions::writePreferences(frond, prefs);
    const auto header = frondSessions::readHeader(frond);
    frondSessions::openable(header);
    frondSessions::importError(header);
    frondSessions::describe(frond);
    quint32 version = 0;
    const auto cards = frondCards::descriptorsOf(frond, &version);
    frondCards::describedCard(cards, "sd.frondsd");
    frondCards::linkedSession(frond);
    frondCards::linkedRows(frond);
    quint64 counter = 0;
    qint64 logical = 0;
    frondCards::cardHeader(machine + "/sd.frondsd", &counter, &logical);
    frondCards::settled(machine, "sd.frondsd", false);
    frondCards::Card entry;
    if (frondCards::readCard(card, &entry)) {
        frondCards::writeCard(card, entry);
        frondCards::savedSdProtection(cards, "sd.frondsd", entry);
    }
    frondCards::entries(root + "/library");
    for (int n : {0, 1}) {
        const auto slot = frondCards::readSlot(machine, n);
        if (slot.present) frondCards::writeSlot(machine, slot, n);
    }
    QDir(root).removeRecursively();
    return 0;
}
