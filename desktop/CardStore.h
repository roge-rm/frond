// The card library: cards/<id>/card.json for every card, the card file there while it is in the
// drawer, and <machine>/slot.json for the card a machine's slot holds or is linked to.
// Include SessionStore.h, which pulls this in once its own helpers are declared.
#include "SessionStore.h"
#include "FolderCard.h"
#include "CardStorage.h"
#ifndef FROND_CARD_STORE_H
#define FROND_CARD_STORE_H

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QLockFile>
#include <QProcess>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStorageInfo>
#include <QTimer>
#include <QUuid>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

namespace frondCards {

// Renames a card file or folder in place. QFile::rename copies and deletes across file systems
// and a move here must never copy, so this fails instead (EXDEV), as it does when `to` exists.
inline bool moveFile(const QString& from, const QString& to) {
    if (QFileInfo::exists(to) || QFileInfo(to).isSymLink()) return false;
    return std::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0;
}

inline const QStringList cardFiles{QStringLiteral("sd.frondsd"), QStringLiteral("ms.frondms"), QStringLiteral("ms.frondmsp")};
inline QString kindOfFile(const QString& file) {
    return file == "sd.frondsd" ? QStringLiteral("sd") : file == "ms.frondms" ? QStringLiteral("ms") : QStringLiteral("ms-pro");
}
inline QString fileOfKind(const QString& kind) {
    return kind == "sd" ? QStringLiteral("sd.frondsd") : kind == "ms" ? QStringLiteral("ms.frondms") : QStringLiteral("ms.frondmsp");
}
// Descriptor kinds in a .frond: 1 SD, 2 classic stick, 3 Pro.
inline quint32 descriptorKindOf(const QString& file) { return file == "sd.frondsd" ? 1 : file == "ms.frondms" ? 2 : 3; }

// <data>/cards beside <data>/sessions.
inline QString libraryFor(const QString& sessions) { return QFileInfo(sessions).absolutePath() + "/cards"; }
inline QString sessionsOf(const QString& machineDir) { return QFileInfo(machineDir).absolutePath(); }
inline QString entryDir(const QString& library, const QString& id) { return library + '/' + id; }

// Free space on the file system holding `path`. Tests replace it.
inline std::function<qint64(const QString&)>& freeSpace() {
    static std::function<qint64(const QString&)> f = [](const QString& path) { return QStorageInfo(path).bytesAvailable(); };
    return f;
}
constexpr qint64 spareBytes = 16 << 20;  // frondSdCandidate::spareBytes

struct Card {
    QString id, name, kind, created, machine;  // machine: the holder's folder name, a hint
    qint64 bytes{0};                            // logical size
    QByteArray identity;                        // 32 bytes
    bool writeProtected{false};
};
inline bool readCard(const QString& dir, Card* card) {
    QFile file(dir + "/card.json");
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto o = QJsonDocument::fromJson(file.readAll()).object();
    if (o.value("version").toInt() != 1 || o.value("id").toString() != QFileInfo(dir).fileName()) return false;
    const auto kind = o.value("kind").toString();
    if (kind != "sd" && kind != "ms" && kind != "ms-pro") return false;
    *card = {o.value("id").toString(), o.value("name").toString(), kind, o.value("created").toString(),
             o.value("machine").toString(), qint64(o.value("bytes").toDouble()),
             QByteArray::fromHex(o.value("identity").toString().toLatin1()), o.value("writeProtected").toBool()};
    card->identity = card->identity.leftJustified(32, '\0', true);
    return true;
}
inline bool writeCard(const QString& dir, const Card& card) {
    QJsonObject o{{"version", 1}, {"id", card.id}, {"name", card.name}, {"kind", card.kind}, {"bytes", double(card.bytes)},
                  {"created", card.created}, {"identity", QString::fromLatin1(card.identity.leftJustified(32, '\0', true).toHex())},
                  {"writeProtected", card.writeProtected},
                  {"machine", card.machine.isEmpty() ? QJsonValue() : QJsonValue(card.machine)}};
    QSaveFile file(dir + "/card.json");
    const auto bytes = QJsonDocument(o).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

struct Slot {
    bool present{false};
    QString card, link, name;  // ids, empty for null
    bool swap{false};          // the slot changed since the last save
};
inline Slot readSlot(const QString& machineDir) {
    Slot slot;
    QFile file(machineDir + "/slot.json");
    if (!file.open(QIODevice::ReadOnly)) return slot;
    const auto o = QJsonDocument::fromJson(file.readAll()).object();
    if (o.value("version").toInt() != 1) return slot;
    slot = {true, o.value("card").toString(), o.value("link").toString(), o.value("name").toString(), o.value("swap").toBool()};
    return slot;
}
inline bool writeSlot(const QString& machineDir, const Slot& slot) {
    auto value = [](const QString& s) { return s.isEmpty() ? QJsonValue() : QJsonValue(s); };
    QJsonObject o{{"version", 1}, {"card", value(slot.card)}, {"link", value(slot.link)}, {"name", value(slot.name)}, {"swap", slot.swap}};
    QSaveFile file(machineDir + "/slot.json");
    const auto bytes = QJsonDocument(o).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

inline QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
inline QString now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODate); }
// A fresh card serial, 32 hex characters as the core makes them.
inline QByteArray newIdentity() {
    QByteArray raw(16, Qt::Uninitialized);
    for (auto& c : raw) c = char(QRandomGenerator::global()->bounded(256));
    return raw.toHex();
}

// The card file in a machine folder, or empty.
inline QString cardFileIn(const QString& dir) {
    for (const auto& name : cardFiles)
        if (QFileInfo(dir + '/' + name).isFile()) return name;
    return {};
}

struct Descriptor {
    quint32 kind{0};
    bool inserted{false}, writeProtected{false};
    QByteArray identity;
    quint64 logical{0}, physical{0}, generation{0};
};
// The .frond's version (0 if unreadable) and its media descriptors (v7 and v8).
inline QList<Descriptor> descriptorsOf(const QString& frond, quint32* version = nullptr) {
    using frondSessions::le32;
    using frondSessions::le64;
    QList<Descriptor> result;
    if (version) *version = 0;
    QFile file(frond);
    if (!file.open(QIODevice::ReadOnly)) return result;
    const auto head = file.read(8 + 32 + 52);
    if (head.size() < 8 || le32(head, 0) != frondSessions::magic) return result;
    const auto v = le32(head, 4);
    if (version) *version = v;
    if ((v != 7 && v != 8) || head.size() != 92) return result;
    const int size = v == 7 ? 92 : 100;
    const auto count = le32(head, 88);
    for (quint32 i = 0; i < count && i < 4; ++i) {
        const auto d = file.read(size);
        if (d.size() != size) return {};
        result.append({le32(d, 0), le32(d, 36) != 0, le32(d, 40) != 0, d.mid(44, 32), le64(d, 76), le64(d, 84),
                       size == 100 ? le64(d, 92) : 0});
    }
    return result;
}
inline const Descriptor* describedCard(const QList<Descriptor>& cards, const QString& file) {
    for (const auto& d : cards)
        if (d.kind == descriptorKindOf(file)) return &d;
    return nullptr;
}

// The SD or Pro v2 generation, or the classic stick's change counter, in a card file's header.
inline bool cardHeader(const QString& path, quint64* counter, qint64* logical) {
    using frondSessions::le32;
    using frondSessions::le64;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto h = file.read(64);
    if (h.size() != 64) return false;
    if (h.left(8) == QByteArray("FRONDSD\0", 8)) {
        if (counter) *counter = le32(h, 8) == 2 ? le64(h, 28) : 0;
        if (logical) *logical = qint64(le64(h, 16));
        return true;
    }
    if (h.left(8) == QByteArray("FRONDMS\0", 8)) {
        if (counter) *counter = le64(h, 32);
        // Each MB is 2048 pages of 512 bytes plus 9.
        if (logical) *logical = qint64(le32(h, 12)) * le32(h, 16) * 512;
        return true;
    }
    return false;
}

inline bool hasJournals(const QString& dir, const QString& file) {
    for (const auto& name : QDir(dir).entryList(QDir::Files | QDir::Hidden | QDir::System))
        if (name.startsWith(file + ".journal-")) return true;
    return false;
}
// The card file is complete on its own: no journals beside it, and the counter the .frond's last
// save recorded (a card put in since then, swap set, has nothing to match).
inline bool settled(const QString& dir, const QString& file, bool swap) {
    if (file.isEmpty() || !QFileInfo(dir + '/' + file).isFile() || hasJournals(dir, file)) return false;
    if (swap) return true;
    const auto cards = descriptorsOf(frondSessions::sessionFrondIn(dir));
    const auto* d = describedCard(cards, file);
    if (!d) return true;
    quint64 counter = 0;
    return cardHeader(dir + '/' + file, &counter, nullptr) && counter == d->generation;
}

// A card file nobody has open in Files…, with any change a crash cut off there finished.
inline bool ready(const QString& path) { return frondCardFile::ready(path); }
inline QString inFilesMessage() { return frondCardFile::inFilesMessage(); }
// Empty when ready(), else why not: open in Files, or a leftover change that can't be finished.
inline QString notReady(const QString& path) { return frondCardFile::notReady(path); }

class LibraryLock {
 public:
    explicit LibraryLock(const QString& library, int timeout = 10000) : lock(library + "/.lock") {
        lock.setStaleLockTime(0);
        ok = QDir().mkpath(library) && lock.tryLock(timeout);
    }
    bool ok{false};
 private:
    QLockFile lock;
};
inline QString busyMessage() { return QStringLiteral("The card library is busy. Try again."); }

struct Entry {
    Card card;
    QString dir;
    bool inDrawer{false};
};
inline QList<Entry> entries(const QString& library) {
    QList<Entry> result;
    for (const auto& info : QDir(library).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        Entry e;
        e.dir = info.absoluteFilePath();
        if (!readCard(e.dir, &e.card)) continue;
        e.inDrawer = QFileInfo(e.dir + '/' + fileOfKind(e.card.kind)).isFile();
        result.append(e);
    }
    return result;
}
// "Games", then "Games 2" and so on, among the library's names.
inline QString freeCardName(const QString& library, const QString& base, const QString& except = {}) {
    QStringList names;
    for (const auto& e : entries(library))
        if (e.card.id != except) names << e.card.name;
    if (!names.contains(base)) return base;
    int n = 2;
    while (names.contains(base + ' ' + QString::number(n))) n++;
    return base + ' ' + QString::number(n);
}

// Whether a library card already has this serial.
inline bool identityUsed(const QString& library, const QByteArray& identity, const QString& except = {}) {
    const auto wanted = identity.leftJustified(32, '\0', true);
    for (const auto& e : entries(library))
        if (e.card.id != except && e.card.identity == wanted) return true;
    return false;
}

// A machine folder whose lock is held for the length of an operation; `held` when the caller
// already holds it.
struct Machine {
    QString dir, name, frond;
    std::unique_ptr<QLockFile> lock;
    bool locked{false};
};

// A new entry for the card file in `dir`. Kind and size from the file, identity and write
// protection from the descriptor when it describes this card. With `freshIdentity` (a copy of a
// card another entry has) the card gets a new serial and the slot swap, so the core loads it as
// a different card: two cards with one serial would look unchanged to each other's machines.
inline bool moveIn(const QString& library, const QString& dir, const QString& file, Slot slot, const QString& name, Card* made = nullptr,
                   bool freshIdentity = false) {
    Card card;
    card.id = newId();
    card.kind = kindOfFile(file);
    card.created = now();
    card.machine = QFileInfo(dir).fileName();
    const auto cards = descriptorsOf(frondSessions::sessionFrondIn(dir));
    const auto* d = describedCard(cards, file);
    qint64 logical = 0;
    cardHeader(dir + '/' + file, nullptr, &logical);
    card.bytes = d ? qint64(d->logical) : logical;
    // A serial another card has (two machines duplicated from each other) is replaced too.
    if (!freshIdentity && d && !slot.swap && identityUsed(library, d->identity)) freshIdentity = true;
    if (freshIdentity) slot.swap = true;
    card.identity = d && !slot.swap ? d->identity : newIdentity();
    card.writeProtected = d && d->writeProtected;
    card.name = freeCardName(library, name.isEmpty() ? QFileInfo(dir).fileName() + " card" : name);
    const auto to = entryDir(library, card.id);
    if (!QDir().mkpath(to) || !writeCard(to, card)) {
        QDir(to).removeRecursively();
        return false;
    }
    slot.card = card.id;
    slot.name = card.name;
    if (!writeSlot(dir, slot)) {
        QDir(to).removeRecursively();
        return false;
    }
    if (made) *made = card;
    return true;
}

// Card file from a machine folder into the drawer. The slot gets swap first, so a crash at any
// point loads with --card-swap; a cut-off move is fixed by reconcile rule 4. Library lock and
// machine lock held.
inline QString toDrawer(const QString& library, const QString& dir, const QString& id) {
    const auto file = cardFileIn(dir);
    Card card;
    if (file.isEmpty() || !readCard(entryDir(library, id), &card)) return QStringLiteral("Couldn't move the card.");
    if (const auto why = notReady(dir + '/' + file); !why.isEmpty()) return why;
    auto slot = readSlot(dir);
    slot.present = true; slot.swap = true; slot.card = id;
    if (!writeSlot(dir, slot)) return QStringLiteral("Couldn't move the card.");
    if (!moveFile(dir + '/' + file, entryDir(library, id) + '/' + file))
        return QStringLiteral("Couldn't move the card into Cards. Cards and sessions have to be on the same disk.");
    slot.card.clear();
    writeSlot(dir, slot);
    card.machine.clear();
    writeCard(entryDir(library, id), card);
    return {};
}
// A drawer card into an empty slot. Library lock and machine lock held.
inline QString toMachine(const QString& library, const QString& id, const QString& dir) {
    Card card;
    if (!readCard(entryDir(library, id), &card)) return QStringLiteral("Couldn't find that card.");
    const auto file = fileOfKind(card.kind);
    if (!cardFileIn(dir).isEmpty()) return QStringLiteral("Take the machine's card out first.");
    if (!QFileInfo(entryDir(library, id) + '/' + file).isFile()) return QStringLiteral("That card is in another machine.");
    if (const auto why = notReady(entryDir(library, id) + '/' + file); !why.isEmpty()) return why;
    auto slot = readSlot(dir);
    slot.present = true; slot.swap = true; slot.card = id; slot.name = card.name;
    if (!writeSlot(dir, slot)) return QStringLiteral("Couldn't move the card.");
    if (!moveFile(entryDir(library, id) + '/' + file, dir + '/' + file)) {
        slot.card.clear();
        writeSlot(dir, slot);
        return QStringLiteral("Couldn't move the card into the machine. Cards and sessions have to be on the same disk.");
    }
    card.machine = QFileInfo(dir).fileName();
    writeCard(entryDir(library, id), card);
    return {};
}

// Hidden copy folders in the library: cards/.new-<id>, locked by cards/.new-<id>.lock while a copy
// fills them.
inline void removeStaleNew(const QString& library) {
    for (const auto& entry : QDir(library).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (!entry.fileName().startsWith(".new-")) continue;
        QLockFile lock(entry.absoluteFilePath() + ".lock");
        lock.setStaleLockTime(0);
        if (!lock.tryLock()) continue;
        QDateTime newest = entry.lastModified();
        for (const auto& child : QDir(entry.absoluteFilePath()).entryInfoList(QDir::Files | QDir::Hidden))
            newest = std::max(newest, child.lastModified());
        if (newest.secsTo(QDateTime::currentDateTime()) > 600) QDir(entry.absoluteFilePath()).removeRecursively();
        lock.unlock();
        if (!QFileInfo::exists(entry.absoluteFilePath())) QFile::remove(entry.absoluteFilePath() + ".lock");
    }
}

// A copy being filled in cards/.new-<id>. Removed with its lock unless taken().
class NewCopy {
 public:
    explicit NewCopy(const QString& library) : id(newId()), dir(library + "/.new-" + id), lock(dir + ".lock") {
        lock.setStaleLockTime(0);
        ok = QDir().mkpath(library) && lock.tryLock() && QDir().mkpath(dir);
    }
    ~NewCopy() {
        if (!kept) QDir(dir).removeRecursively();
        lock.unlock();
        QFile::remove(dir + ".lock");
    }
    void taken() { kept = true; }
    const QString id, dir;
    bool ok{false};
 private:
    QLockFile lock;
    bool kept{false};
};

// A settled copy of the card file `file` in machine folder `dir` (or a drawer folder) at `to`.
// An unfinished save is settled as the copy is made. Checks free space first. Empty on success.
inline QString settledCopy(const QString& dir, const QString& file, const QString& to, const frondSessions::CopyProgress& progress) {
    const QString failed = QStringLiteral("Couldn't copy the card.");
    auto source = dir + '/' + file;
    QString journal;
    if (hasJournals(dir, file) || !settled(dir, file, false)) {
        const auto settle = frondSessions::sdSettleOf(dir, frondSessions::sessionFrondIn(dir));
        if (settle.sibling == file && !settle.full.isEmpty()) source = settle.full;
        else if (settle.sibling == file && !settle.journal.isEmpty()) journal = settle.journal;
        else if (hasJournals(dir, file)) return QStringLiteral("unsettled");
    }
    const qint64 total = QFileInfo(source).size();
    const auto free = freeSpace()(QFileInfo(to).absolutePath());
    if (free >= 0 && free < total + spareBytes)
        return QStringLiteral("Not enough free space for the card. It needs %1 MB.").arg((total + spareBytes) / 1000000 + 1);
    qint64 done = 0;
    bool cancelled = false;
    const frondSessions::CopyProgress watch = [&](qint64 n, qint64 all) {
        if (progress && !progress(n, all)) cancelled = true;
        return !cancelled;
    };
    if (!frondSessions::copyChunked(source, to, &done, total, watch)) {
        QFile::remove(to);
        return cancelled ? QStringLiteral("cancelled") : failed;
    }
    if (!journal.isEmpty() && !frondSessions::applySdJournalCopy(journal, to)) {
        QFile::remove(to);
        return QStringLiteral("unsettled");
    }
    return {};
}

// Reconcile: makes card.json, slot.json and the machine hints match where the card files are.
// `held` is a machine folder the caller already holds locked. Machines locked elsewhere are
// skipped. Library lock held.
inline void reconcileLocked(const QString& sessions, const QString& held = {}) {
    const auto library = libraryFor(sessions);
    const auto heldCanon = held.isEmpty() ? QString() : QFileInfo(held).canonicalFilePath();
    std::vector<Machine> machines;
    QStringList skippedClaims;  // ids named by machines we can't touch
    for (const auto& info : QDir(sessions).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (frondSessions::ignoredName(info.fileName())) continue;
        Machine m;
        m.dir = info.absoluteFilePath();
        m.name = info.fileName();
        m.frond = frondSessions::sessionFrondIn(m.dir);
        if (!QFileInfo(m.frond).isFile()) continue;  // not saved yet
        const bool isHeld = !heldCanon.isEmpty() && info.canonicalFilePath() == heldCanon;
        if (!isHeld) {
            m.lock = std::make_unique<QLockFile>(m.frond + ".lock");
            m.lock->setStaleLockTime(0);
            if (!frondSessions::tryLockSession(*m.lock, m.frond)) {
                const auto slot = readSlot(m.dir);
                skippedClaims << slot.card << slot.link;
                continue;
            }
        }
        m.locked = true;
        machines.push_back(std::move(m));
    }
    // Files… changes a crash cut off, in the machines held and the drawer. Cards open in Files now
    // are left alone.
    for (const auto& m : machines)
        if (const auto file = cardFileIn(m.dir); !file.isEmpty()) frondCardFile::settle(m.dir + '/' + file);
    for (const auto& e : entries(library))
        if (e.inDrawer) frondCardFile::settle(e.dir + '/' + fileOfKind(e.card.kind));
    auto known = entries(library);
    auto find = [&](const QString& id) -> Entry* {
        for (auto& e : known)
            if (e.card.id == id) return &e;
        return nullptr;
    };
    // Rules 1 to 4 per machine. Machines the hint names go first, so they keep their card (rule 3).
    std::stable_sort(machines.begin(), machines.end(), [&](const Machine& a, const Machine& b) {
        auto hinted = [&](const Machine& m) {
            const auto slot = readSlot(m.dir);
            const auto* e = find(slot.card);
            return e && e->card.machine == m.name;
        };
        return hinted(a) && !hinted(b);
    });
    QStringList claimed, knownIn;  // knownIn: machines whose slot names the library card they hold
    for (auto& m : machines) {
        quint32 version = 0;
        descriptorsOf(m.frond, &version);
        if (version < 7) continue;  // a v6 card is inside the .frond until its first run
        auto slot = readSlot(m.dir);
        const auto file = cardFileIn(m.dir);
        auto* e = find(slot.card);
        if (!file.isEmpty()) {
            if (e && e->card.kind == kindOfFile(file) && !e->inDrawer && !claimed.contains(e->card.id)) {
                claimed << e->card.id;  // rule 1
                if (slot.card == e->card.id) knownIn << m.dir;
                if (e->card.machine != m.name) {
                    e->card.machine = m.name;
                    writeCard(e->dir, e->card);
                }
                continue;
            }
            // Rule 2, or rule 3 for a second holder: a new entry, named after the first one's, with
            // its own serial. The card it holds isn't its link any more.
            Slot fresh = slot;
            fresh.present = true;
            fresh.link.clear();
            Card made;
            if (moveIn(library, m.dir, file, fresh, e ? e->card.name : slot.name, &made, e != nullptr)) {
                claimed << made.id;
                known = entries(library);
            }
            continue;
        }
        // No card file. Rule 4 (a cut-off move: the drawer wins) or a card that is elsewhere now.
        if (!slot.card.isEmpty()) {
            slot.card.clear();
            slot.swap = true;
            writeSlot(m.dir, slot);
        }
    }
    // Rule 5: an entry with no file anywhere goes, with links to it. Not while a machine we
    // skipped may hold it.
    for (const auto& e : known) {
        if (e.inDrawer || claimed.contains(e.card.id) || skippedClaims.contains(e.card.id)) continue;
        // The hint names a saved machine we couldn't lock: it may hold the card.
        const auto hinted = sessions + '/' + e.card.machine;
        if (!e.card.machine.isEmpty() && QFileInfo(frondSessions::sessionFrondIn(hinted)).isFile() &&
            std::none_of(machines.cbegin(), machines.cend(), [&](const Machine& m) { return m.name == e.card.machine; }))
            continue;
        // A machine that isn't saved yet, holding the card it was given while running.
        if (!e.card.machine.isEmpty() && !QFileInfo(frondSessions::sessionFrondIn(hinted)).isFile() &&
            QFileInfo(hinted + '/' + fileOfKind(e.card.kind)).isFile() && readSlot(hinted).card == e.card.id)
            continue;
        QDir(e.dir).removeRecursively();
        for (auto& m : machines) {
            auto slot = readSlot(m.dir);
            if (slot.link == e.card.id) {
                slot.link.clear();
                writeSlot(m.dir, slot);
            }
        }
    }
    known = entries(library);
    // Rule 6: a linked card left in a stopped machine by a crash goes back to the drawer once settled.
    for (auto& m : machines) {
        const auto slot = readSlot(m.dir);
        const auto file = cardFileIn(m.dir);
        if (file.isEmpty() || slot.link.isEmpty() || slot.card != slot.link) continue;
        if (settled(m.dir, file, slot.swap)) toDrawer(library, m.dir, slot.card);
    }
    // Rule 7: a take-out the launcher didn't finish, as when Frond died and the core saved after it.
    // The slot names the library card, the save has no card there and the slot hasn't changed
    // since, so the card goes to the drawer.
    for (auto& m : machines) {
        const auto slot = readSlot(m.dir);
        const auto file = cardFileIn(m.dir);
        if (!knownIn.contains(m.dir) || file.isEmpty() || slot.card.isEmpty() || slot.swap) continue;
        quint32 version = 0;
        const auto cards = descriptorsOf(m.frond, &version);
        if (version < 7 || describedCard(cards, file) || !settled(m.dir, file, false)) continue;
        toDrawer(library, m.dir, slot.card);
    }
    // Hints of drawer cards.
    for (auto& e : entries(library))
        if (e.inDrawer && !e.card.machine.isEmpty()) {
            e.card.machine.clear();
            writeCard(e.dir, e.card);
        }
}
inline QString reconcile(const QString& sessions, const QString& held = {}) {
    LibraryLock lock(libraryFor(sessions));
    if (!lock.ok) return busyMessage();
    reconcileLocked(sessions, held);
    return {};
}
// At start-up: stale copies go, then reconcile.
inline void startUp(const QString& sessions) {
    if (!QFileInfo(sessions).isDir()) return;
    const auto library = libraryFor(sessions);
    removeStaleNew(library);
    reconcile(sessions);
}

// Card flags for the core, from slot.json and card.json. Never for a 68k machine.
inline QStringList launchArguments(const QString& sessions, const QString& dir) {
    const auto slot = readSlot(dir);
    if (!slot.swap) return {};
    QStringList args{QStringLiteral("--card-swap")};
    const auto file = cardFileIn(dir);
    Card card;
    if (!file.isEmpty() && !slot.card.isEmpty() && readCard(entryDir(libraryFor(sessions), slot.card), &card)) {
        args << QStringLiteral("--card-identity") << QString::fromLatin1(card.identity.toHex());
        if (card.writeProtected) args << QStringLiteral("--card-protected");
    }
    return args;
}

// Machine start, machine lock held: reconcile, then a linked machine with an empty slot takes
// its card if it is in the drawer. Returns the flags for the core; `heldBy` gets the holder's
// name when another machine has the linked card.
inline QStringList takeAtStart(const QString& sessions, const QString& dir, QString* heldBy = nullptr) {
    const auto library = libraryFor(sessions);
    // The machine lock is held, so its card can be settled even if the library is busy.
    if (const auto file = cardFileIn(dir); !file.isEmpty()) frondCardFile::settle(dir + '/' + file);
    LibraryLock lock(library);
    if (lock.ok) {
        reconcileLocked(sessions, dir);
        const auto slot = readSlot(dir);
        if (!slot.link.isEmpty() && cardFileIn(dir).isEmpty()) {
            Card card;
            const bool known = readCard(entryDir(library, slot.link), &card);
            if (known && QFileInfo(entryDir(library, slot.link) + '/' + fileOfKind(card.kind)).isFile())
                toMachine(library, slot.link, dir);
            else if (known && heldBy)
                *heldBy = card.machine;
        }
    }
    return launchArguments(sessions, dir);
}

// After the final save of a linked holder, machine lock held: the card goes to the drawer.
inline QString releaseAtClose(const QString& sessions, const QString& dir) {
    const auto slot = readSlot(dir);
    const auto file = cardFileIn(dir);
    if (slot.link.isEmpty() || slot.card != slot.link || file.isEmpty()) return {};
    if (!settled(dir, file, false)) return QStringLiteral("The card stays in the machine: its last save isn't finished.");
    const auto library = libraryFor(sessions);
    LibraryLock lock(library);
    if (!lock.ok) return busyMessage();
    return toDrawer(library, dir, slot.card);
}

// After a save that succeeded, machine lock held.
inline void clearSwap(const QString& dir) {
    auto slot = readSlot(dir);
    if (!slot.present || !slot.swap) return;
    slot.swap = false;
    writeSlot(dir, slot);
}

// The SD card inside a v6 .frond (the stream after the header and its 32-byte table), written raw
// to `to`, and its serial as the core gives it at extraction (MD5 of the payload, hex). `none` is
// set for a file with no card or no readable table, which has nothing to keep.
inline QString extractV6Card(const QString& frond, const QString& to, bool* none, QByteArray* identity,
                             const frondSessions::CopyProgress& progress = {}) {
    using frondSessions::le32;
    *none = false;
    QFile in(frond);
    if (!in.open(QIODevice::ReadOnly)) return QStringLiteral("Couldn't read the session.");
    const auto head = in.read(40 + 32);
    if (head.size() != 72 || le32(head, 0) != frondSessions::magic || le32(head, 4) != 6) { *none = true; return {}; }
    quint64 skip = 0;
    for (int i = 2; i < 7; ++i) skip += le32(head, 40 + 4 * i);
    const quint64 sd = le32(head, 40 + 28);
    if (!sd) { *none = true; return {}; }
    const QString unreadable = QStringLiteral("Couldn't read the card inside this session.");
    if (sd % 512 || sd > (128u << 20)) return unreadable;
    QFile out(to);
    if (!out.open(QIODevice::WriteOnly)) return QStringLiteral("Couldn't copy the card.");
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return unreadable;
    QCryptographicHash md5(QCryptographicHash::Md5);
    QByteArray input, output(1 << 20, Qt::Uninitialized);
    quint64 skipped = 0, written = 0;
    bool ended = false;
    QString error;
    while (written < sd && error.isEmpty()) {
        if (z.avail_in == 0) {
            input = in.read(1 << 20);
            if (input.isEmpty()) { error = unreadable; break; }
            z.next_in = reinterpret_cast<Bytef*>(input.data());
            z.avail_in = uInt(input.size());
        }
        // Only as much as is left of the sections to skip, then of the card.
        const quint64 want = skipped < skip ? std::min<quint64>(skip - skipped, output.size()) : std::min<quint64>(sd - written, output.size());
        z.next_out = reinterpret_cast<Bytef*>(output.data());
        z.avail_out = uInt(want);
        const int r = inflate(&z, Z_NO_FLUSH);
        const quint64 got = want - z.avail_out;
        if (r == Z_STREAM_END) ended = true;
        else if (r != Z_OK && r != Z_BUF_ERROR) { error = unreadable; break; }
        if (skipped < skip) skipped += got;
        else if (got) {
            md5.addData(QByteArrayView(output.constData(), qsizetype(got)));
            if (out.write(output.constData(), qint64(got)) != qint64(got)) { error = QStringLiteral("Couldn't copy the card."); break; }
            written += got;
            if (progress && !progress(qint64(written), qint64(sd))) error = QStringLiteral("cancelled");
        }
        if (ended && written < sd) error = unreadable;
        if (!got && r == Z_BUF_ERROR && z.avail_in) error = unreadable;
    }
    inflateEnd(&z);
    if (error.isEmpty() && !out.flush()) error = QStringLiteral("Couldn't copy the card.");
    out.close();
    if (!error.isEmpty()) { QFile::remove(to); return error; }
    *identity = md5.result().toHex();
    return {};
}

// The ARM core, which is also the card tool (`frond-core --card ...`); the window sets it.
// FROND_CARD_TOOL replaces it (the tests use it).
inline QString& defaultCore() {
    static QString core;
    return core;
}
inline QString cardTool(const QString& core = {}) {
    const auto env = qEnvironmentVariable("FROND_CARD_TOOL");
    return !env.isEmpty() ? env : !core.isEmpty() ? core : defaultCore();
}
struct ToolResult {
    bool ok{false};
    QString code, message;
};
// Runs one card tool operation, passing its progress on; `progress` returning false cancels it
// (SIGTERM, the tool removes its output). Blocks, so run it off the UI thread.
inline ToolResult runCardTool(const QString& tool, const QStringList& args, const frondSessions::CopyProgress& progress = {}) {
    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(tool, QStringList{QStringLiteral("--card")} + args);
    if (!p.waitForStarted(10000)) return {false, QStringLiteral("start"), QStringLiteral("Couldn't run the card tool.")};
    QByteArray pending;
    QString last;
    bool cancelled = false;
    qint64 done = 0, total = 0;
    auto take = [&] {
        pending += p.readAllStandardOutput();
        for (int at = pending.indexOf('\n'); at >= 0; at = pending.indexOf('\n')) {
            const auto line = QString::fromUtf8(pending.left(at));
            pending.remove(0, at + 1);
            if (line.startsWith("progress ")) {
                const auto parts = line.mid(9).split('/');
                if (parts.size() == 2) { done = parts[0].toLongLong(); total = parts[1].toLongLong(); }
            } else if (!line.isEmpty()) last = line;
        }
    };
    while (p.state() != QProcess::NotRunning) {
        p.waitForReadyRead(100);
        take();
        if (!cancelled && progress && !progress(done, total)) {
            cancelled = true;
            p.terminate();
        }
    }
    p.waitForFinished(-1);
    take();
    if (last.startsWith("ok ")) return {true, QStringLiteral("ok"), last.mid(3)};
    if (cancelled) return {false, QStringLiteral("cancelled"), QStringLiteral("Cancelled.")};
    if (last.startsWith("error ")) {
        const auto rest = last.mid(6);
        const auto space = rest.indexOf(' ');
        return {false, rest.left(space), space < 0 ? QStringLiteral("The card tool failed.") : rest.mid(space + 1)};
    }
    return {false, QStringLiteral("failed"), QStringLiteral("The card tool failed.")};
}

// Delete: the machine's card goes back to the drawer. Settled, a rename; unsettled, a settled
// copy into the drawer; a v6 machine's card is taken out of its .frond with the card tool.
// Machine lock held. Empty on success, then the folder can go.
inline QString keepV6CardOf(const QString& sessions, const QString& dir, const QString& tool, const frondSessions::CopyProgress& progress);
inline QString keepCardOf(const QString& sessions, const QString& dir, const frondSessions::CopyProgress& progress = {},
                          const QString& cancelled = QStringLiteral("Delete cancelled."), const QString& tool = {}) {
    const auto library = libraryFor(sessions);
    quint32 version = 0;
    descriptorsOf(frondSessions::sessionFrondIn(dir), &version);
    if (version == 6) return keepV6CardOf(sessions, dir, tool, progress);
    {
        LibraryLock lock(library);
        if (!lock.ok) return busyMessage();
        // A save that got as far as its full card copy: settle it the way a load does, so the card
        // is moved in rather than lost.
        if (cardFileIn(dir).isEmpty()) {
            const auto settle = frondSessions::sdSettleOf(dir, frondSessions::sessionFrondIn(dir));
            if (!settle.full.isEmpty() && !moveFile(settle.full, dir + '/' + settle.sibling))
                return QStringLiteral("Couldn't move the card into Cards.");
        }
        reconcileLocked(sessions, dir);
        const auto slot = readSlot(dir);
        const auto file = cardFileIn(dir);
        if (file.isEmpty()) return {};
        if (slot.card.isEmpty()) return QStringLiteral("Couldn't move the card into Cards.");
        if (const auto why = notReady(dir + '/' + file); !why.isEmpty()) return why;
        if (settled(dir, file, slot.swap)) return toDrawer(library, dir, slot.card);
    }
    // Copied outside the lock, then renamed in under it.
    const auto slot = readSlot(dir);
    const auto file = cardFileIn(dir);
    NewCopy copy(library);
    if (!copy.ok) return QStringLiteral("Couldn't move the card into Cards.");
    const auto error = settledCopy(dir, file, copy.dir + '/' + file, progress);
    if (error == "unsettled") return QStringLiteral("Open %1 once to finish its last save.").arg(QFileInfo(dir).fileName());
    if (error == "cancelled") return cancelled;
    if (!error.isEmpty()) return error;
    LibraryLock lock(library);
    if (!lock.ok) return busyMessage();
    Card card;
    if (!readCard(entryDir(library, slot.card), &card)) return QStringLiteral("Couldn't move the card into Cards.");
    if (!moveFile(copy.dir + '/' + file, entryDir(library, slot.card) + '/' + file))
        return QStringLiteral("Couldn't move the card into Cards.");
    QFile::remove(dir + '/' + file);
    for (const auto& name : QDir(dir).entryList(QDir::Files | QDir::Hidden | QDir::System))
        if (name.startsWith(file + ".journal-")) QFile::remove(dir + '/' + name);
    card.machine.clear();
    writeCard(entryDir(library, slot.card), card);
    return {};
}

// A v6 machine's card is inside its .frond: it goes into the drawer as a new card, extracted raw
// and made into a settled card file by the card tool, before the machine can be deleted.
inline QString keepV6CardOf(const QString& sessions, const QString& dir, const QString& tool, const frondSessions::CopyProgress& progress) {
    const auto library = libraryFor(sessions);
    NewCopy copy(library);
    if (!copy.ok) return QStringLiteral("Couldn't move the card into Cards.");
    const auto raw = copy.dir + "/card.img";
    bool none = false;
    QByteArray identity;
    // Raw copy first, then the card file: twice the card's size at most, 128 MB each.
    const auto free = freeSpace()(library);
    if (free >= 0 && free < 2 * (qint64(128) << 20) + spareBytes) {
        QFile frond(frondSessions::sessionFrondIn(dir));
        if (frond.open(QIODevice::ReadOnly)) {
            const auto head = frond.read(72);
            if (head.size() == 72 && frondSessions::le32(head, 68) &&
                free < 2 * qint64(frondSessions::le32(head, 68)) + spareBytes)
                return QStringLiteral("Not enough free space for the card. It needs %1 MB.")
                    .arg((2 * qint64(frondSessions::le32(head, 68)) + spareBytes) / 1000000 + 1);
        }
    }
    auto half = [&](qint64 base) {
        return frondSessions::CopyProgress([&, base](qint64 done, qint64 total) {
            return !progress || progress(base + done / 2, total);
        });
    };
    auto error = extractV6Card(frondSessions::sessionFrondIn(dir), raw, &none, &identity, half(0));
    if (error == "cancelled") return QStringLiteral("Delete cancelled.");
    if (!error.isEmpty()) return error + QStringLiteral(" It wasn't deleted.");
    if (none) return {};
    const qint64 rawBytes = QFileInfo(raw).size();
    const auto result = runCardTool(cardTool(tool), {QStringLiteral("import"), raw, QStringLiteral("sd"), copy.dir + "/sd.frondsd"},
                                    half(rawBytes / 2));
    QFile::remove(raw);
    if (!result.ok && result.code == "cancelled") return QStringLiteral("Delete cancelled.");
    qint64 logical = 0;
    if (!result.ok || !cardHeader(copy.dir + "/sd.frondsd", nullptr, &logical) || logical != rawBytes)
        return QStringLiteral("Couldn't move the card into Cards: %1 It wasn't deleted.").arg(result.ok ? QStringLiteral("the copy didn't check out.") : result.message);
    LibraryLock lock(library);
    if (!lock.ok) return busyMessage();
    // Two v6 machines duplicated from each other hold the same card bytes, so the same serial.
    if (identityUsed(library, identity)) identity = newIdentity();
    Card card{copy.id, freeCardName(library, QFileInfo(dir).fileName() + " card"), QStringLiteral("sd"), now(), {}, logical, identity, false};
    if (!writeCard(copy.dir, card) || !QDir().rename(copy.dir, entryDir(library, copy.id)))
        return QStringLiteral("Couldn't move the card into Cards.");
    copy.taken();
    return {};
}

// Whether a v6 .frond has a card inside.
inline bool v6HasCard(const QString& frond) {
    QFile f(frond);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const auto head = f.read(72);
    return head.size() == 72 && frondSessions::le32(head, 4) == 6 && frondSessions::le32(head, 68) != 0;
}

// ---- Card types and sizes, from the device profiles ----

// "sd", "ms" (classic Memory Stick) and "ms-pro"; sizes are MiB for SD and Pro, MB for a classic stick.
inline QString kindName(const QString& kind) {
    return kind == "sd" ? QStringLiteral("SD card") : kind == "ms" ? QStringLiteral("Memory Stick") : QStringLiteral("Memory Stick Pro");
}
inline QString toolKind(const QString& kind) { return kind == "ms-pro" ? QStringLiteral("mspro") : kind; }
inline QString sizeLabel(int size) { return size >= 1024 ? QStringLiteral("%1 GB").arg((size + 512) / 1024) : QStringLiteral("%1 MB").arg(size); }
// A card's size in the units above, from its logical bytes. A classic stick's bytes are a little
// off its nominal MB, so it snaps to the nearest size there is.
inline int sizeOf(const QString& kind, qint64 bytes) {
    const int mib = int((bytes + (1 << 19)) >> 20);
    if (kind != "ms") return mib;
    int best = 4;
    for (int mb : {4, 8, 16, 32, 64, 128})
        if (std::abs(mb - mib) < std::abs(best - mib)) best = mb;
    return best;
}
// "32 MB", or "Pro · 1 GB" for a Memory Stick Pro.
inline QString cardSizeText(const Card& card) {
    const auto size = sizeLabel(sizeOf(card.kind, card.bytes));
    return card.kind == "ms-pro" ? QStringLiteral("Pro · ") + size : size;
}
inline bool deviceTakesKind(frondFirmware::Device device, const QString& kind) {
    if (frondFirmware::runs68k(device)) return false;
    const auto& p = frondFirmware::profile(device);
    const auto slot = deviceProfileStorageSlot5(p);
    if (kind == "sd") return slot == DeviceStorageSlot5::sd && p.frontend.sdSlot;
    if (slot != DeviceStorageSlot5::memoryStick || !p.frontend.memoryStick) return false;
    return kind == "ms" ? p.msSizes != 0 : p.msProMaxMib >= 256;
}
// Whether the device takes a card of this kind and size; `tooBig` when only the size is wrong.
inline bool deviceTakes(frondFirmware::Device device, const QString& kind, int size, bool* tooBig = nullptr) {
    if (tooBig) *tooBig = false;
    if (!deviceTakesKind(device, kind)) return false;
    const auto& p = frondFirmware::profile(device);
    bool fits, big;
    if (kind == "sd") {
        const auto bytes = quint64(size) << 20;
        fits = bytes >= p.sdMin && bytes <= p.sdMax;
        big = bytes > p.sdMax;
    } else if (kind == "ms") {
        fits = p.msSizes & uint32_t(size);
        big = !fits && uint32_t(size) > p.msSizes;
    } else {
        fits = size >= 256 && uint32_t(size) <= p.msProMaxMib;
        big = uint32_t(size) > p.msProMaxMib;
    }
    if (tooBig) *tooBig = !fits && big;
    return fits;
}
// Sizes some device takes, smallest first.
inline QList<int> sizesFor(const QString& kind) {
    QList<int> all = kind == "sd" ? QList<int>{32, 64, 128, 256, 512, 1024, 1886} : kind == "ms" ? QList<int>{4, 8, 16, 32, 64, 128}
                                                                                                 : QList<int>{256, 512, 1024, 1886};
    QList<int> result;
    for (int size : all)
        if (std::any_of(frondFirmware::devices.cbegin(), frondFirmware::devices.cend(),
                        [&](frondFirmware::Device d) { return deviceTakes(d, kind, size); }))
            result << size;
    return result;
}
// The kinds some device takes: SD, then classic sticks, then Pro.
inline QStringList kindsTaken() {
    QStringList result;
    for (const auto* kind : {"sd", "ms", "ms-pro"})
        if (!sizesFor(QLatin1String(kind)).isEmpty()) result << QLatin1String(kind);
    return result;
}

// ---- What the Cards window shows ----

struct MachineSlot {
    QString dir, name;
    frondFirmware::Device device{frondFirmware::Device::automatic};
    Slot slot;
    QString cardFile;
    bool busy{false};  // open in another Frond (or another window)
};
// Saved ARM machines with a card slot. `held` is the caller's own machine, which isn't busy.
inline QList<MachineSlot> machineSlots(const QString& sessions, const QString& held = {}) {
    QList<MachineSlot> result;
    const auto heldCanon = held.isEmpty() ? QString() : QFileInfo(held).canonicalFilePath();
    for (const auto& info : QDir(sessions).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase)) {
        if (frondSessions::ignoredName(info.fileName())) continue;
        MachineSlot m;
        m.dir = info.absoluteFilePath();
        m.name = info.fileName();
        const auto frond = frondSessions::sessionFrondIn(m.dir);
        if (!QFileInfo(frond).isFile()) continue;
        const auto header = frondSessions::readHeader(frond);
        if (!frondSessions::openable(header)) continue;
        m.device = frondFirmware::deviceFromId(header.device);
        if (!deviceTakesKind(m.device, "sd") && !deviceTakesKind(m.device, "ms") && !deviceTakesKind(m.device, "ms-pro")) continue;
        m.slot = readSlot(m.dir);
        m.cardFile = cardFileIn(m.dir);
        if (heldCanon.isEmpty() || info.canonicalFilePath() != heldCanon) {
            QLockFile lock(frond + ".lock");
            lock.setStaleLockTime(0);
            m.busy = !frondSessions::tryLockSession(lock, frond);
        }
        result.append(m);
    }
    return result;
}

struct Row {
    Card card;
    bool inDrawer{false};
    QString holder, holderDir;  // the machine the card is in
    bool holderBusy{false};
    QStringList links;          // machines linked to it
};
inline QList<Row> rows(const QString& sessions, const QString& held = {}) {
    const auto library = libraryFor(sessions);
    const auto machines = machineSlots(sessions, held);
    QList<Row> result;
    for (const auto& e : entries(library)) {
        Row row{e.card, e.inDrawer, {}, {}, false, {}};
        for (const auto& m : machines) {
            if (m.slot.link == e.card.id) row.links << m.name;
            if (!e.inDrawer && m.slot.card == e.card.id && m.cardFile == fileOfKind(e.card.kind)) {
                row.holder = m.name; row.holderDir = m.dir; row.holderBusy = m.busy;
            }
        }
        // A machine that isn't saved yet (and so isn't listed) still holds it.
        if (!e.inDrawer && row.holder.isEmpty() && !e.card.machine.isEmpty()) {
            row.holder = e.card.machine;
            row.holderDir = sessions + '/' + e.card.machine;
            QLockFile lock(frondSessions::sessionFrondIn(row.holderDir) + ".lock");
            lock.setStaleLockTime(0);
            const bool heldHere = !held.isEmpty() && QFileInfo(held).canonicalFilePath() == QFileInfo(row.holderDir).canonicalFilePath();
            row.holderBusy = !heldHere && QFileInfo(frondSessions::sessionFrondIn(row.holderDir)).isFile() &&
                             !frondSessions::tryLockSession(lock, frondSessions::sessionFrondIn(row.holderDir));
        }
        result.append(row);
    }
    std::sort(result.begin(), result.end(), [](const Row& a, const Row& b) {
        return QString::localeAwareCompare(a.card.name, b.card.name) < 0;
    });
    return result;
}
// "In Palm TX", "Free" or "Not in a machine", then the machines a linked card goes with.
inline QString statusText(const Row& row) {
    QString text = !row.holder.isEmpty() ? QStringLiteral("In %1").arg(row.holder)
                   : !row.links.isEmpty() ? QStringLiteral("Free") : QStringLiteral("Not in a machine");
    if (!row.links.isEmpty()) text += QStringLiteral(" · Linked to ") + row.links.join(QStringLiteral(", "));
    return text;
}

// ---- Card operations. Each takes the library lock for its changes; copies run outside it. ----

// A copy filled in `copy` becomes the card `card` (its id is the copy's).
inline QString adoptNew(const QString& library, NewCopy& copy, Card card) {
    LibraryLock lock(library);
    if (!lock.ok) return busyMessage();
    card.id = copy.id;
    if (!writeCard(copy.dir, card) || !QDir().rename(copy.dir, entryDir(library, copy.id)))
        return QStringLiteral("Couldn't add the card.");
    copy.taken();
    return {};
}
inline QString toolError(const ToolResult& r) { return r.code == "cancelled" ? QStringLiteral("Cancelled.") : r.message; }

// A new formatted card in the drawer. `size` as in sizesFor().
inline QString makeCard(const QString& sessions, const QString& name, const QString& kind, int size,
                        const frondSessions::CopyProgress& progress = {}, QString* id = nullptr) {
    const auto library = libraryFor(sessions);
    if (name.trimmed().isEmpty()) return QStringLiteral("Give the card a name.");
    if (freeCardName(library, name.trimmed()) != name.trimmed()) return QStringLiteral("There's already a card called %1.").arg(name.trimmed());
    NewCopy copy(library);
    if (!copy.ok) return QStringLiteral("Couldn't add the card.");
    const auto file = copy.dir + '/' + fileOfKind(kind);
    const auto r = runCardTool(cardTool(), {QStringLiteral("make"), toolKind(kind), QString::number(size), file}, progress);
    if (!r.ok) return toolError(r);
    qint64 logical = 0;
    if (!cardHeader(file, nullptr, &logical)) return QStringLiteral("Couldn't add the card.");
    if (const auto error = adoptNew(library, copy, {{}, name.trimmed(), kind, now(), {}, logical, newIdentity(), false}); !error.isEmpty())
        return error;
    if (id) *id = copy.id;
    return {};
}
// The kinds an image could be: a .frondms header is a classic stick, a raw image one of SD or Pro
// by size. Empty when no device takes it.
inline QStringList imageKinds(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const auto head = f.read(8);
    if (head == QByteArray("FRONDMS\0", 8)) return {QStringLiteral("ms")};
    if (head == QByteArray("FRONDSD\0", 8)) return {};
    const qint64 bytes = f.size();
    if (!bytes || bytes % 512) return {};
    QStringList kinds;
    for (const auto* kind : {"sd", "ms-pro"}) {
        const QString k = QLatin1String(kind);
        if (k == "ms-pro" && (bytes % (1 << 20) || !QList<int>{256, 512, 1024, 1886}.contains(int(bytes >> 20)))) continue;
        if (k == "sd" && (bytes < (qint64(32) << 20) || bytes > (qint64(1886) << 20))) continue;
        if (std::any_of(frondFirmware::devices.cbegin(), frondFirmware::devices.cend(), [&](frondFirmware::Device d) {
                if (!deviceTakesKind(d, k)) return false;
                if (k == "sd") {
                    const auto& p = frondFirmware::profile(d);
                    return quint64(bytes) >= p.sdMin && quint64(bytes) <= p.sdMax;
                }
                return deviceTakes(d, k, int(bytes >> 20));
            }))
            kinds << k;
    }
    return kinds;
}
// An image into the drawer as a new card named after the file.
inline QString importCard(const QString& sessions, const QString& image, const QString& kind,
                          const frondSessions::CopyProgress& progress = {}, QString* id = nullptr) {
    const auto library = libraryFor(sessions);
    NewCopy copy(library);
    if (!copy.ok) return QStringLiteral("Couldn't add the card.");
    const auto file = copy.dir + '/' + fileOfKind(kind);
    const auto r = runCardTool(cardTool(), {QStringLiteral("import"), image, toolKind(kind), file}, progress);
    if (!r.ok) return toolError(r);
    qint64 logical = 0;
    if (!cardHeader(file, nullptr, &logical)) return QStringLiteral("Couldn't add the card.");
    auto base = QFileInfo(image).completeBaseName().trimmed();
    if (base.isEmpty()) base = kindName(kind);
    Card card{{}, {}, kind, now(), {}, logical, newIdentity(), false};
    {
        LibraryLock lock(library);
        if (!lock.ok) return busyMessage();
        card.name = freeCardName(library, base);
    }
    if (const auto error = adoptNew(library, copy, card); !error.isEmpty()) return error;
    if (id) *id = copy.id;
    return {};
}

// The types and sizes a folder can become: SD and Memory Stick Pro (a classic stick has its own
// file system, which only the Palm writes). Sizes as in sizesFor().
inline QStringList folderKinds() {
    QStringList result;
    for (const auto& kind : kindsTaken())
        if (kind != "ms") result << kind;
    return result;
}

// A host folder copied once onto a new card: a blank FAT16 image, the folder written in and read
// back with mtools, then the card tool makes the card file from it, all inside cards/.new-<id> so a refusal, an
// error or Cancel leaves nothing. Named `name`. Refused when the folder doesn't fit a card of `size`.
inline QString importFolderCard(const QString& sessions, const QString& folder, const QString& name, const QString& kind, int size,
                                const frondSessions::CopyProgress& progress = {}, QString* id = nullptr) {
    const auto library = libraryFor(sessions);
    const auto wanted = name.trimmed();
    if (wanted.isEmpty()) return QStringLiteral("Give the card a name.");
    if (freeCardName(library, wanted) != wanted) return QStringLiteral("There's already a card called %1.").arg(wanted);
    if (!folderKinds().contains(kind) || !sizesFor(kind).contains(size)) return QStringLiteral("Choose an SD card or Memory Stick Pro size.");
    if (!QFileInfo(folder).isDir()) return QStringLiteral("Couldn't read the folder.");
    if (!frondFolderCard::mtoolsAvailable()) return QStringLiteral("Install mtools to import a folder (Debian: sudo apt install mtools).");
    // Phases of the progress bar: copy 0 to 45%, check 45 to 50%, the card file 50 to 100%.
    constexpr qint64 whole = 1000;
    auto report = [&](qint64 done) { return !progress || progress(done, whole); };
    const auto plan = frondFolderCard::measure(folder, [&] { return !report(0); });
    if (plan.cancelled) return QStringLiteral("Cancelled.");
    if (!plan.problem.isEmpty()) return plan.problem;
    if (!frondFolderCard::fits(plan, unsigned(size)))
        return QStringLiteral("The folder doesn't fit on a %1 card.").arg(sizeLabel(size));
    const qint64 cardBytes = qint64(size) << 20;
    const auto free = freeSpace()(library);
    if (free >= 0 && free < 2 * cardBytes + plan.bytes + spareBytes)
        return QStringLiteral("Not enough free space for the card. It needs %1 MB.").arg((2 * cardBytes + plan.bytes + spareBytes) / 1000000 + 1);
    NewCopy copy(library);
    if (!copy.ok) return QStringLiteral("Couldn't add the card.");
    const auto raw = copy.dir + "/.work.img", back = copy.dir + "/.back";
    if (!frondFolderCard::blankImage(raw, unsigned(size))) return QStringLiteral("Couldn't make the card.");
    if (auto error = frondFolderCard::copyFolder(raw, folder, back, plan, [&](qint64 d, qint64 t) { return report(t ? d * 450 / t : 0); });
        !error.isEmpty())
        return error;
    if (!report(450)) return QStringLiteral("Cancelled.");
    if (!frondFolderCard::sameTree(folder, back)) return QStringLiteral("The card copy didn't match the folder.");
    QDir(back).removeRecursively();
    const auto file = copy.dir + '/' + fileOfKind(kind);
    const auto r = runCardTool(cardTool(), {QStringLiteral("import"), raw, toolKind(kind), file},
                               [&](qint64 d, qint64 t) { return report(500 + (t ? d * 500 / t : 0)); });
    if (!r.ok) return toolError(r);
    QFile::remove(raw);
    qint64 logical = 0;
    if (!cardHeader(file, nullptr, &logical)) return QStringLiteral("Couldn't add the card.");
    if (const auto error = adoptNew(library, copy, {{}, wanted, kind, now(), {}, logical, newIdentity(), false}); !error.isEmpty())
        return error;
    if (id) *id = copy.id;
    return {};
}

// Where a card's file is now, for a copy or an export: the drawer, or a stopped machine (whose
// lock `hold` then holds). An unfinished save is used as a load would settle it.
inline QString cardSource(const QString& sessions, const QString& id, QString* path, std::unique_ptr<QLockFile>* hold,
                          Card* card = nullptr, QString* holderDir = nullptr) {
    const auto library = libraryFor(sessions);
    Card c;
    if (!readCard(entryDir(library, id), &c)) return QStringLiteral("Couldn't find that card.");
    if (card) *card = c;
    const auto file = fileOfKind(c.kind);
    if (QFileInfo(entryDir(library, id) + '/' + file).isFile()) {
        if (const auto why = notReady(entryDir(library, id) + '/' + file); !why.isEmpty()) return why;
        *path = entryDir(library, id) + '/' + file;
        return {};
    }
    for (const auto& r : rows(sessions))
        if (r.card.id == id && !r.holderDir.isEmpty()) {
            if (r.holderBusy) return QStringLiteral("%1 is running. Stop it first.").arg(r.holder);
            *hold = std::make_unique<QLockFile>(frondSessions::sessionFrondIn(r.holderDir) + ".lock");
            (*hold)->setStaleLockTime(0);
            if (QFileInfo(frondSessions::sessionFrondIn(r.holderDir)).isFile() &&
                !frondSessions::tryLockSession(**hold, frondSessions::sessionFrondIn(r.holderDir)))
                return QStringLiteral("%1 is running. Stop it first.").arg(r.holder);
            if (holderDir) *holderDir = r.holderDir;
            if (const auto why = QFileInfo(r.holderDir + '/' + file).isFile() ? notReady(r.holderDir + '/' + file) : QString();
                !why.isEmpty()) {
                hold->reset();
                return why;
            }
            const auto slot = readSlot(r.holderDir);
            if (settled(r.holderDir, file, slot.swap)) {
                *path = r.holderDir + '/' + file;
                return {};
            }
            const auto settle = frondSessions::sdSettleOf(r.holderDir, frondSessions::sessionFrondIn(r.holderDir));
            if (settle.sibling == file && !settle.full.isEmpty()) {
                *path = settle.full;
                return {};
            }
            return QStringLiteral("Open %1 once to finish its last save.").arg(r.holder);
        }
    return QStringLiteral("Couldn't find that card.");
}
// The card as an image: .img for SD and Pro, .frondms for a classic stick.
inline QString exportCardImage(const QString& sessions, const QString& id, const QString& image, const frondSessions::CopyProgress& progress = {}) {
    QString path;
    std::unique_ptr<QLockFile> hold;
    if (const auto error = cardSource(sessions, id, &path, &hold); !error.isEmpty()) return error;
    const auto r = runCardTool(cardTool(), {QStringLiteral("export"), path, image}, progress);
    return r.ok ? QString() : toolError(r);
}
// A settled copy in the drawer as "Name 2", with its own serial and no links.
inline QString duplicateCard(const QString& sessions, const QString& id, const frondSessions::CopyProgress& progress = {}, QString* newId = nullptr) {
    const auto library = libraryFor(sessions);
    QString path, holderDir;
    std::unique_ptr<QLockFile> hold;
    Card card;
    if (const auto error = cardSource(sessions, id, &path, &hold, &card, &holderDir); !error.isEmpty()) return error;
    NewCopy copy(library);
    if (!copy.ok) return QStringLiteral("Couldn't copy the card.");
    const auto file = fileOfKind(card.kind);
    QString error;
    if (holderDir.isEmpty() || path.endsWith(".full")) {
        // A settled file: copied as it is.
        const qint64 total = QFileInfo(path).size();
        const auto free = freeSpace()(library);
        if (free >= 0 && free < total + spareBytes)
            return QStringLiteral("Not enough free space for the card. It needs %1 MB.").arg((total + spareBytes) / 1000000 + 1);
        qint64 done = 0;
        bool cancelled = false;
        const frondSessions::CopyProgress watch = [&](qint64 n, qint64 all) {
            if (progress && !progress(n, all)) cancelled = true;
            return !cancelled;
        };
        if (!frondSessions::copyChunked(path, copy.dir + '/' + file, &done, total, watch))
            return cancelled ? QStringLiteral("Cancelled.") : QStringLiteral("Couldn't copy the card.");
    } else {
        error = settledCopy(holderDir, file, copy.dir + '/' + file, progress);
        if (error == "cancelled") return QStringLiteral("Cancelled.");
        if (error == "unsettled") return QStringLiteral("Open %1 once to finish its last save.").arg(QFileInfo(holderDir).fileName());
        if (!error.isEmpty()) return error;
    }
    hold.reset();
    Card made = card;
    made.created = now();
    made.machine.clear();
    made.identity = newIdentity();
    {
        LibraryLock lock(library);
        if (!lock.ok) return busyMessage();
        made.name = freeCardName(library, card.name);
    }
    if (const auto e = adoptNew(library, copy, made); !e.isEmpty()) return e;
    if (newId) *newId = copy.id;
    return {};
}
// card.json only, and the holder's slot.json name. Names are unique.
inline QString renameCard(const QString& sessions, const QString& id, const QString& wanted) {
    const auto library = libraryFor(sessions);
    const auto name = wanted.trimmed();
    if (name.isEmpty()) return QStringLiteral("Give the card a name.");
    LibraryLock lock(library);
    if (!lock.ok) return busyMessage();
    Card card;
    if (!readCard(entryDir(library, id), &card)) return QStringLiteral("Couldn't find that card.");
    if (freeCardName(library, name, id) != name) return QStringLiteral("There's already a card called %1.").arg(name);
    card.name = name;
    if (!writeCard(entryDir(library, id), card)) return QStringLiteral("Couldn't rename the card.");
    return {};
}
// Only a card in the drawer. Links to it go.
inline QString deleteCard(const QString& sessions, const QString& id, const QString& held = {}) {
    const auto library = libraryFor(sessions);
    LibraryLock lock(library);
    if (!lock.ok) return busyMessage();
    Card card;
    if (!readCard(entryDir(library, id), &card)) return QStringLiteral("Couldn't find that card.");
    if (!QFileInfo(entryDir(library, id) + '/' + fileOfKind(card.kind)).isFile()) return QStringLiteral("Take the card out of its machine first.");
    if (const auto why = notReady(entryDir(library, id) + '/' + fileOfKind(card.kind)); !why.isEmpty()) return why;
    if (!QDir(entryDir(library, id)).removeRecursively()) return QStringLiteral("Couldn't delete the card.");
    for (const auto& m : machineSlots(sessions, held)) {
        if (m.busy || m.slot.link != id) continue;
        QLockFile mlock(frondSessions::sessionFrondIn(m.dir) + ".lock");
        mlock.setStaleLockTime(0);
        const bool own = !held.isEmpty() && QFileInfo(held).canonicalFilePath() == QFileInfo(m.dir).canonicalFilePath();
        if (!own && !frondSessions::tryLockSession(mlock, frondSessions::sessionFrondIn(m.dir))) continue;
        auto slot = m.slot;
        slot.link.clear();
        writeSlot(m.dir, slot);
    }
    return {};
}
// Take out of a stopped machine: settled, a rename; unsettled, a settled copy, or refused when the
// journal can't be applied. The slot gets `card: null, swap: true`.
inline QString takeOutStopped(const QString& sessions, const QString& id, const frondSessions::CopyProgress& progress = {}) {
    QString dir;
    for (const auto& r : rows(sessions))
        if (r.card.id == id) {
            if (r.inDrawer) return {};
            if (r.holderBusy) return QStringLiteral("%1 is running. Stop it first.").arg(r.holder);
            dir = r.holderDir;
        }
    if (dir.isEmpty()) return QStringLiteral("Couldn't find that card.");
    QLockFile lock(frondSessions::sessionFrondIn(dir) + ".lock");
    lock.setStaleLockTime(0);
    if (QFileInfo(frondSessions::sessionFrondIn(dir)).isFile() && !frondSessions::tryLockSession(lock, frondSessions::sessionFrondIn(dir)))
        return QStringLiteral("%1 is running. Stop it first.").arg(QFileInfo(dir).fileName());
    if (const auto error = keepCardOf(sessions, dir, progress, QStringLiteral("Cancelled.")); !error.isEmpty()) return error;
    auto slot = readSlot(dir);
    if (!slot.card.isEmpty() && cardFileIn(dir).isEmpty()) {
        slot.card.clear();
        slot.swap = true;
        writeSlot(dir, slot);
    }
    return {};
}
// Links `id` to the machines in `linked` and unlinks it from the others. A machine linked to
// another card moves its link (its name goes in `moved`). One holding a different card, or one
// that doesn't take the card, is left alone. `held` is the caller's running machine.
inline QString setLinks(const QString& sessions, const QString& id, const QStringList& linked, const QString& held = {},
                        QStringList* moved = nullptr) {
    const auto library = libraryFor(sessions);
    LibraryLock lock(library);
    if (!lock.ok) return busyMessage();
    Card card;
    if (!readCard(entryDir(library, id), &card)) return QStringLiteral("Couldn't find that card.");
    QStringList wanted;
    for (const auto& dir : linked) wanted << QFileInfo(dir).canonicalFilePath();
    QString error;
    for (const auto& m : machineSlots(sessions, held)) {
        const bool want = wanted.contains(QFileInfo(m.dir).canonicalFilePath());
        if (want == (m.slot.link == id)) continue;
        if (m.busy) { error = QStringLiteral("%1 is running. Stop it first.").arg(m.name); continue; }
        QLockFile mlock(frondSessions::sessionFrondIn(m.dir) + ".lock");
        mlock.setStaleLockTime(0);
        const bool own = !held.isEmpty() && QFileInfo(held).canonicalFilePath() == QFileInfo(m.dir).canonicalFilePath();
        if (!own && !frondSessions::tryLockSession(mlock, frondSessions::sessionFrondIn(m.dir))) { error = QStringLiteral("%1 is running. Stop it first.").arg(m.name); continue; }
        auto slot = m.slot;
        slot.present = true;
        if (want) {
            if (!deviceTakes(m.device, card.kind, sizeOf(card.kind, card.bytes))) continue;
            if (!m.cardFile.isEmpty() && slot.card != id) continue;
            if (!slot.link.isEmpty() && moved) *moved << m.name;
            slot.link = id;
        } else {
            slot.link.clear();
        }
        if (!writeSlot(m.dir, slot)) error = QStringLiteral("Couldn't link %1.").arg(m.name);
    }
    return error;
}

// Save a Copy: the copy carries the running machine's slot.json, as an export does. Its .frond was
// just saved with the card it has, so it needs no swap.
inline QString stageSlot(const QString& machineDir, const QString& copyDir) {
    auto slot = readSlot(machineDir);
    if (!slot.present) return {};
    slot.swap = false;
    if (cardFileIn(copyDir).isEmpty()) slot.card.clear();
    return writeSlot(copyDir, slot) ? QString() : QStringLiteral("Couldn't save the session.");
}

// The name of the card a machine holds or is linked to, for dialogs.
inline QString cardNameOf(const QString& sessions, const QString& dir) {
    const auto slot = readSlot(dir);
    Card card;
    const auto id = slot.card.isEmpty() ? slot.link : slot.card;
    return !id.isEmpty() && readCard(entryDir(libraryFor(sessions), id), &card) ? card.name : QString();
}

// Watches whether a linked card is in the drawer: the library folder, the card's folder and a
// 5 second timer. `changed` gets each change.
class FreeWatcher {
 public:
    FreeWatcher(const QString& library, const QString& id, std::function<void(bool)> changed)
        : library(library), id(id), changed(std::move(changed)) {
        QDir().mkpath(library);
        watcher.addPath(library);
        if (QFileInfo(entryDir(library, id)).isDir()) watcher.addPath(entryDir(library, id));
        QObject::connect(&watcher, &QFileSystemWatcher::directoryChanged, &watcher, [this] { check(); });
        QObject::connect(&timer, &QTimer::timeout, &timer, [this] { check(); });
        timer.start(5000);
        state = isFree();
    }
    bool isFree() const {
        Card card;
        return readCard(entryDir(library, id), &card) && QFileInfo(entryDir(library, id) + '/' + fileOfKind(card.kind)).isFile();
    }
    bool lastState() const { return state; }
    void check() {
        if (QFileInfo(entryDir(library, id)).isDir() && !watcher.directories().contains(entryDir(library, id)))
            watcher.addPath(entryDir(library, id));
        const bool now = isFree();
        if (now == state) return;
        state = now;
        if (changed) changed(now);
    }
 private:
    QString library, id;
    std::function<void(bool)> changed;
    QFileSystemWatcher watcher;
    QTimer timer;
    bool state{false};
};

}  // namespace frondCards
#endif
