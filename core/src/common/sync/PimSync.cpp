#include "PimSync.h"

#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>

#include "Sha256.h"

namespace frondSync {
namespace {

struct DbName {
    PimKind kind;
    const char* classic;
    const char* classicCreator;
    const char* garnet;
    const char* garnetCreator;
};

constexpr std::array<DbName, 4> names{{
    {PimKind::contacts, "AddressDB", "addr", "ContactsDB-PAdd", "PAdd"},
    {PimKind::calendar, "DatebookDB", "date", "CalendarDB-PDat", "PDat"},
    {PimKind::tasks, "ToDoDB", "todo", "TasksDB-PTod", "PTod"},
    {PimKind::memos, "MemoDB", "memo", "MemosDB-PMem", "PMem"},
}};

std::string Digest(const Bytes& data) { return Sha256Hex(data.data(), data.size()); }

std::string RecordHash(const DlpRecord& record) {
    Bytes bytes{uint8_t(record.flags & recSecret), record.category};
    bytes.insert(bytes.end(), record.data.begin(), record.data.end());
    return Digest(bytes);
}

class ProfileChange {
   public:
    explicit ProfileChange(SyncProfile& profile) : profile(profile) {
        std::string error;
        if (!profile.BeginPimChange(error)) throw std::runtime_error(error);
    }
    ~ProfileChange() { if (active) profile.RollBackPimChange(); }
    void Commit() {
        std::string error;
        if (!profile.CommitPimChange(error)) throw std::runtime_error(error);
        active = false;
    }

   private:
    SyncProfile& profile;
    bool active{true};
};

class Categories {
   public:
    explicit Categories(Bytes data) : block(std::move(data)) {
        if (block.empty()) return;
        if (block.size() < commonSize) throw std::runtime_error("Palm category block is truncated");
        for (size_t i = 0; i < labels.size(); ++i) {
            const auto start = block.begin() + LabelOffset(i);
            labels[i] = PalmPimText(Bytes(start, std::find(start, start + labelLength, 0)));
        }
    }

    std::string Name(uint8_t slot) const { return slot ? labels[slot & 15] : ""; }

    uint8_t Slot(const std::string& name) const {
        if (name.empty() || name == labels[0]) return 0;
        auto position = std::find(labels.begin() + 1, labels.end(), name);
        return position == labels.end() ? uint8_t(labels.size()) : uint8_t(position - labels.begin());
    }

    uint8_t Add(const std::string& name) {
        if (block.empty()) throw std::runtime_error("This Palm database has no category block");
        Bytes bytes;
        if (!EncodePalmPimText(name, bytes) || bytes.size() >= labelLength)
            throw std::runtime_error("Palm category names must fit in 15 bytes");
        const auto free = std::find(labels.begin() + 1, labels.end(), "");
        if (free == labels.end()) throw std::runtime_error("No free category slots on the Palm");
        const size_t slot = size_t(free - labels.begin());
        const auto ids = block.begin() + idOffset;
        unsigned id = 128;  // IDs 128–255 are reserved for desktop-created categories.
        while (id <= 255 && std::find(ids, ids + labels.size(), uint8_t(id)) != ids + labels.size()) ++id;
        if (id > 255) throw std::runtime_error("No free Palm category IDs");
        const auto dest = block.begin() + LabelOffset(slot);
        std::fill_n(dest, labelLength, 0);
        std::copy(bytes.begin(), bytes.end(), dest);
        block[idOffset + slot] = uint8_t(id);
        labels[slot] = name;
        return uint8_t(slot);
    }

    std::string Hash() const {
        // The application's sort options and renamed/dirty flags do not affect the record names.
        return block.empty() ? Digest({}) : Digest(Bytes(block.begin() + 2, block.begin() + idOffset + labels.size()));
    }

    const Bytes& Block() const { return block; }

   private:
    static constexpr size_t labelLength = 16, idOffset = 258, commonSize = 276;
    static size_t LabelOffset(size_t slot) { return 2 + labelLength * slot; }

    Bytes block;
    std::array<std::string, 16> labels;
};

class Conduit {
   public:
    Conduit(DlpClient& dlp, SyncProfile& profile, std::string scope, const PalmPimDatabase& db, PimAction action,
            PimSyncCounts& counts)
        : dlp(dlp), profile(profile), scope(std::move(scope)), database(db), action(action), counts(counts) {}

    void Run(bool fast) {
        Must(dlp.OpenConduit(), "Start conduit");
        Must(dlp.OpenDB(0, 0xf0, database.name, handle), "Open database");
        open = true;
        Bytes app;
        Must(dlp.ReadAppBlock(handle, app), "Read categories");
        categories = Categories(std::move(app));

        std::string error;
        for (auto& link : profile.PimLinks(scope, error)) links.emplace(link.guestId, std::move(link));
        Check(error.empty(), error);
        const auto pending = profile.PendingPimWrites(scope, error);
        Check(error.empty(), error);

        // A failed conduit always retries with a full view, even if this device still has our PC ID.
        const auto previousCategories = profile.DeviceValue(scope, "pim-categories");
        Check(profile.SetDeviceValue(scope, "pim-categories", "", &error), error);
        fast = fast && action == PimAction::synchronize && pending.empty() && previousCategories == categories.Hash();
        if (!fast || !ReadFast()) ReadFull();
        for (const auto& [uid, record] : incoming)
            if (record.flags & recBusy) throw std::runtime_error("A Palm record is busy");

        std::vector<PimRecord> records;
        if (action == PimAction::desktopOverwrites) {
            Check(profile.ReadPimRecords(database.kind, false, records, error), error);
            PreflightOverwrite(records);
        }
        Recover(pending);

        for (const auto& [uid, record] : incoming) {
            auto link = links.find(uid);
            if (link == links.end()) Unlinked(record);
            else Linked(record, link->second);
        }

        for (const auto& [uid, link] : links) {
            if (!present.count(uid)) Missing(link);
        }

        // New guest records must be imported before uploads, including an interrupted upload
        // recovered above. Keep the snapshot of matched record IDs for overwrite semantics.
        if (action != PimAction::desktopOverwrites)
            Check(profile.ReadPimRecords(database.kind, false, records, error), error);
        for (const auto& record : records) {
            if (record.archived || matched.count(record.id)) continue;
            if (action == PimAction::handheldOverwrites) {
                Check(profile.DeletePimRecord(record.id, record.revision, error), error);
                ++counts.deleted;
            } else {
                Push(record, 0);
            }
        }

        for (const auto& [uid, linked] : overwriteDeletes) {
            RemoveGuest(uid);
            if (linked) RemoveLink(uid);
        }
        Must(dlp.CleanUpDatabase(handle), "Remove deleted Palm records");
        Must(dlp.ResetSyncFlags(handle), "Reset Palm sync flags");
        const int closed = dlp.CloseDB(handle);
        open = false;
        Must(closed, "Close database");
        Check(profile.SetDeviceValue(scope, "pim-categories", categories.Hash(), &error), error);
    }

    ~Conduit() {
        if (open && dlp.LinkError().empty()) dlp.CloseDB(handle);
    }

   private:
    void Check(bool success, const std::string& why) {
        if (!success) throw std::runtime_error(why.empty() ? "Could not update sync profile" : why);
    }

    void Must(int rc, const char* what) {
        if (!rc) return;
        throw std::runtime_error(std::string(what) + ": " + (rc < 0 ? dlp.LinkError() : DlpErrorText(uint16_t(rc))));
    }

    bool ReadFast() {
        std::vector<uint32_t> ids;
        int rc = dlp.ReadRecordIds(handle, ids);
        if (rc == dlpErrNotSupp || rc == dlpErrIllegalReq) return false;
        Must(rc, "Read Palm record IDs");
        rc = dlp.ResetRecordIndex(handle);
        if (rc == dlpErrNotSupp || rc == dlpErrIllegalReq) return false;
        Must(rc, "Reset Palm record index");
        for (size_t i = 0; i < 65536; ++i) {
            DlpRecord r;
            rc = dlp.ReadNextModifiedRecord(handle, r);
            if (rc == dlpErrNotFound) break;
            if (rc == dlpErrNotSupp || rc == dlpErrIllegalReq || rc == dlpErrSize) return false;
            Must(rc, "Read modified Palm record");
            if (!r.uid || !incoming.emplace(r.uid, std::move(r)).second)
                throw std::runtime_error("Duplicate Palm record ID");
            if (i == 65535) throw std::runtime_error("Too many Palm records");
        }
        for (const auto uid : ids) {
            if (!links.count(uid) && !incoming.count(uid)) return false;
            if (!uid || !present.insert(uid).second) throw std::runtime_error("Duplicate Palm record ID");
        }
        for (const auto& [uid, record] : incoming) present.insert(uid);
        // Clean records can still have pending desktop edits.
        for (const auto& [uid, link] : links) {
            if (!present.count(uid) || incoming.count(uid)) continue;
            const auto record = profile.FindPimRecord(link.recordId);
            if (!record) throw std::runtime_error("Missing mapped profile record");
            if (record->deleted || record->archived) {
                RemoveGuest(uid);
                RemoveLink(uid);
            } else if (record->revision != link.revision) Push(*record, uid);
            matched.insert(link.recordId);
        }
        return true;
    }

    void ReadFull() {
        present.clear();
        incoming.clear();
        OpenDbInfo info;
        Must(dlp.ReadOpenDBInfo(handle, info), "Count Palm records");
        for (unsigned index = 0; index < info.records; ++index) {
            DlpRecord r;
            const int rc = dlp.ReadPimRecordByIndex(handle, uint16_t(index), r);
            if (rc == dlpErrNotFound || rc == dlpErrDeleted) continue;
            Must(rc, "Read Palm record");
            if (!r.uid || !present.insert(r.uid).second) throw std::runtime_error("Duplicate Palm record ID");
            incoming.emplace(r.uid, std::move(r));
        }
    }

    void Recover(const std::vector<PimPending>& pending) {
        for (const auto& intent : pending) {
            uint32_t found = 0;
            for (const auto& [uid, record] : incoming) {
                if (!links.count(uid) && !(record.flags & (recDeleted | recArchived)) &&
                    RecordHash(record) == intent.guestHash) {
                    if (found) throw std::runtime_error("More than one Palm record matches an interrupted upload");
                    found = uid;
                }
            }
            std::string error;
            ProfileChange change(profile);
            if (found) {
                PimLink link{found, intent.recordId, intent.revision, GuestHash(incoming.at(found))};
                Check(profile.SavePimLink(scope, link, error), error);
                links.emplace(found, std::move(link));
            }
            Check(profile.ClearPendingPimWrite(scope, intent.recordId, error), error);
            change.Commit();
        }
    }

    void RemoveGuest(uint32_t uid) {
        const int rc = dlp.DeleteRecord(handle, uid);
        if (rc != dlpErrNotFound && rc != dlpErrDeleted) Must(rc, "Delete Palm record");
        ++counts.deleted;
    }

    void RemoveLink(uint32_t uid) {
        std::string error;
        Check(profile.RemovePimLink(scope, uid, error), error);
    }

    void SaveIncoming(const DlpRecord& guest, PimRecord local) {
        PimRecord decoded;
        std::string error;
        Check(DecodePalmPim(database.kind, database.format, guest.data, decoded, error), error);
        decoded.category = categories.Name(guest.category);
        decoded.secret = guest.flags & recSecret;
        ApplyPalmPim(decoded, database.format, local);
        local.deleted = false;
        local.archived = guest.flags & recArchived;

        ProfileChange change(profile);
        Check(profile.SavePimRecord(local, error), error);
        Check(profile.SavePimLink(scope, {guest.uid, local.id, local.revision, GuestHash(guest)}, error), error);
        if (local.archived) ++counts.archived;
        change.Commit();
        matched.insert(local.id);
        ++counts.downloaded;
    }

    void Unlinked(const DlpRecord& record) {
        if (record.flags & recDeleted && !(record.flags & recArchived)) return;
        if (action == PimAction::desktopOverwrites) {
            overwriteDeletes.emplace_back(record.uid, false);
            return;
        }
        PimRecord local;
        local.kind = database.kind;
        SaveIncoming(record, local);
    }

    void Linked(const DlpRecord& guest, const PimLink& link) {
        const auto record = profile.FindPimRecord(link.recordId);
        if (!record) throw std::runtime_error("Missing mapped profile record");
        const auto& local = *record;
        if (guest.flags & recArchived && action != PimAction::desktopOverwrites) {
            // Keep a simultaneous local edit active and save a separate archived copy.
            if (local.archived && link.guestHash == GuestHash(guest)) return;
            if (local.revision != link.revision && !local.deleted && !local.archived &&
                action == PimAction::synchronize) {
                PimRecord archived = local;
                archived.id = 0;
                SaveIncoming(guest, std::move(archived));
            } else SaveIncoming(guest, local);
            return;
        }
        if (guest.flags & (recDeleted | recArchived)) {
            Missing(link);
            return;
        }

        const bool guestChanged = GuestHash(guest) != link.guestHash;
        const bool localChanged = local.revision != link.revision;
        matched.insert(local.id);
        if (action == PimAction::desktopOverwrites) {
            if (local.deleted || local.archived) {
                overwriteDeletes.emplace_back(guest.uid, true);
            } else Push(local, guest.uid, &guest);
            return;
        }
        if (action == PimAction::handheldOverwrites) {
            if (guestChanged || localChanged || local.deleted || local.archived) SaveIncoming(guest, local);
            return;
        }
        if (!guestChanged) {
            if (local.deleted || local.archived) {
                RemoveGuest(guest.uid);
                RemoveLink(guest.uid);
            } else if (localChanged) Push(local, guest.uid, &guest);
            return;
        }
        if (!localChanged || local.deleted || local.archived) {
            SaveIncoming(guest, local);
            return;
        }
        DlpRecord projected = BuildOutgoing(local, guest.uid, false);
        if (RecordHash(projected) == RecordHash(guest)) {
            Remember(local, guest.uid, guest);
            return;
        }
        PimRecord duplicate = local;
        duplicate.id = 0;
        // Adopt the changed Palm record as a new desktop record. The previous desktop record
        // remains available to upload after the import pass.
        SaveIncoming(guest, std::move(duplicate));
        matched.erase(local.id);
        ++counts.conflicts;
    }

    void Missing(const PimLink& link) {
        const auto record = profile.FindPimRecord(link.recordId);
        if (!record) throw std::runtime_error("Missing mapped profile record");
        const auto& local = *record;
        if (!local.deleted && !local.archived && (action == PimAction::desktopOverwrites ||
            (action == PimAction::synchronize && local.revision != link.revision))) {
            RemoveLink(link.guestId);
            return;
        }
        ProfileChange change(profile);
        if (!local.deleted && !local.archived) {
            std::string error;
            Check(profile.DeletePimRecord(local.id, local.revision, error), error);
            ++counts.deleted;
        }
        RemoveLink(link.guestId);
        matched.insert(local.id);
        change.Commit();
    }

    void PreflightOverwrite(const std::vector<PimRecord>& records) {
        Categories planned = categories;
        std::string error;
        for (const auto& local : records) {
            if (local.archived) continue;
            Bytes encoded;
            Check(EncodePalmPim(local, database.format, encoded, error), error);
            Check(encoded.size() <= 0xffff - 8, "Palm record is too large to sync");
            if (planned.Slot(local.category) == 16) planned.Add(local.category);
        }
    }

    DlpRecord BuildOutgoing(const PimRecord& local, uint32_t uid, bool addCategory) {
        std::string error;
        DlpRecord record;
        record.uid = uid;
        record.flags = local.secret ? recSecret : 0;
        Check(EncodePalmPim(local, database.format, record.data, error), error);
        Check(record.data.size() <= 0xffff - 8, "Palm record is too large to sync");
        record.category = categories.Slot(local.category);
        if (record.category == 16 && addCategory) {
            record.category = categories.Add(local.category);
            Must(dlp.WriteAppBlock(handle, categories.Block()), "Write categories");
        }
        return record;
    }

    std::string GuestHash(const DlpRecord& record) const {
        return RecordHash(record) + "/" + categories.Name(record.category);
    }

    void Remember(const PimRecord& local, uint32_t uid, const DlpRecord& guest) {
        std::string error;
        Check(profile.SavePimLink(scope, {uid, local.id, local.revision, GuestHash(guest)}, error), error);
    }

    void Push(const PimRecord& local, uint32_t uid, const DlpRecord* current = nullptr) {
        DlpRecord out = BuildOutgoing(local, uid, true);
        const auto hash = RecordHash(out);
        if (current && hash == RecordHash(*current)) {
            Remember(local, uid, *current);
            return;
        }
        std::string error;
        if (!uid) {
            Check(profile.SetPendingPimWrite(scope, {local.id, local.revision, hash}, error), error);
        }
        uint32_t newUid = 0;
        Must(dlp.WritePimRecord(handle, out, newUid), "Write Palm record");
        if (uid && newUid != uid) throw std::runtime_error("Palm changed an existing record ID");
        ProfileChange change(profile);
        Remember(local, newUid, out);
        if (!uid) Check(profile.ClearPendingPimWrite(scope, local.id, error), error);
        change.Commit();
        ++counts.uploaded;
    }

    DlpClient& dlp;
    SyncProfile& profile;
    std::string scope;
    const PalmPimDatabase& database;
    PimAction action;
    PimSyncCounts& counts;
    Categories categories{Bytes{}};
    uint8_t handle{0};
    bool open{false};
    std::map<uint32_t, PimLink> links;
    std::map<uint32_t, DlpRecord> incoming;
    std::set<uint32_t> present;
    std::set<int64_t> matched;
    std::vector<std::pair<uint32_t, bool>> overwriteDeletes;
};

}  // namespace

std::vector<PalmPimDatabase> SelectPalmPimDatabases(const std::vector<DbInfo>& list) {
    auto find = [&](const char* name, const char* creator) {
        return std::find_if(list.begin(), list.end(), [&](const DbInfo& db) {
            return db.name == name && db.creator == FourCc(creator) && db.type == FourCc("DATA");
        });
    };
    auto writable = [&](const auto& it) {
        return it != list.end() && !(it->attributes & (dbResource | dbReadOnly | dbCopyPrevention));
    };
    std::vector<PalmPimDatabase> out;
    for (const auto& name : names) {
        const auto enhanced = find(name.garnet, name.garnetCreator);
        if (enhanced != list.end()) {
            if (writable(enhanced)) out.push_back({name.kind, PalmPimFormat::garnet, name.garnet});
        } else if (writable(find(name.classic, name.classicCreator))) {
            out.push_back({name.kind, PalmPimFormat::classic, name.classic});
        }
    }
    return out;
}

bool SyncPalmPim(DlpClient& dlp, SyncProfile& profile, const std::string& deviceKey, const std::string& userKey,
                 const PalmPimDatabase& database, bool fast, PimSyncCounts& counts, std::string& error) {
    const PimAction action = profile.GetPimAction(deviceKey, database.kind);
    if (action == PimAction::skip)
        return profile.SetDeviceValue(userKey + "/" + database.name, "pim-categories", "", &error);
    try {
        Conduit(dlp, profile, userKey + "/" + database.name, database, action, counts).Run(fast);
        return profile.FinishPimAction(deviceKey, database.kind, action, error);
    } catch (const std::runtime_error& e) {
        error = database.name + ": " + e.what();
        return false;
    }
}

}  // namespace frondSync
