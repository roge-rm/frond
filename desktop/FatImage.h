#pragma once
// Reads and edits a FAT16 card in place, the same as Android's FatImage.kt: the card the core makes
// (one primary FAT16 partition) or a FAT16 superfloppy, 32 MiB to 2 GiB. Lists folders, copies
// files in and out, makes folders, and copies, moves, renames and deletes inside the card.
// New data only goes into free clusters; the FAT and folder sectors a change touches are kept in
// memory and committed together by the Blocks (CardFile: crash safe with the chunk CRCs and header).
// A change that fails rolls back to the FAT on the card. Not thread safe; one editor per card.
#include "CardFile.h"
#include <QDateTime>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace frondCardFile {

struct Entry {
    QString name, shortName;
    bool directory{false};
    qint64 size{0};
    quint32 firstCluster{0};
    QDateTime modified;  // invalid when the card has none
};

class FatImage {
 public:
    // Reads the card's layout. Throws Error.
    explicit FatImage(Blocks& blocks);

    qint64 clusterBytes() const { return qint64(clusterSectors) * sectorBytes; }
    qint64 freeBytes() const;
    qint64 totalBytes() const { return qint64(clusterCount) * clusterBytes(); }
    // Bytes written to the card, and whether any change was committed.
    qint64 bytesWritten{0};
    bool committed{false};

    // A folder, "/" or "/PALM/Launcher": folders first, then files, by name.
    QList<Entry> list(const QString& path);
    std::optional<Entry> find(const QString& path);
    // The file's bytes, a cluster at a time.
    void read(const Entry& entry, const std::function<void(const char*, qint64)>& out);
    // A new file of `size` bytes from `input` (reads up to n bytes, returns how many, 0 at the end).
    void add(const QString& path, const QString& name, const std::function<qint64(char*, qint64)>& input, qint64 size);
    void makeFolder(const QString& path, const QString& name);
    // A file, or a folder with everything in it.
    void remove(const QString& path);
    // Into toFolder, under newName when given (a rename when the folder is the same).
    void move(const QString& from, const QString& toFolder, const QString& newName = {});
    // A file or folder into toFolder under newName (or its own). `bytes` gets the amount so far.
    void copy(const QString& from, const QString& toFolder, const QString& newName = {},
              const std::function<bool()>& cancelled = {}, const std::function<void(qint64)>& bytes = {});
    // Runs `work` (an add, copy or move that makes an entry of the victim's name) and removes
    // `victim` in the same commit. The victim's clusters stay used until then, so a cancel, a
    // failure or a crash leaves the old one; the new data needs room beside it.
    void replace(const QString& victim, const std::function<void()>& work);
    // The bytes in a file, or in every file under a folder.
    qint64 sizeOf(const QString& path);

    // Whether every FAT copy matches the first, and a check as fsck would do it: chains sound and
    // used once, sizes that fit and no lost clusters. The problems found.
    bool fatCopiesMatch();
    QStringList check();

    static QStringList parts(const QString& path);
    static QString joined(const QString& folder, const QString& name);
    // Whether `inner` is `outer` or below it.
    static bool within(const QString& outer, const QString& inner);
    // Throws when FAT can't hold the name.
    static void checkName(const QString& name);

 private:
    struct Slot { Entry entry; int first{0}, count{0}; };
    struct Folder { quint32 cluster{0}; bool root() const { return cluster == 0; } };

    QByteArray readAt(qint64 at, qint64 length);
    void writeData(qint64 at, const QByteArray& b);
    void stage(qint64 at, const char* b, qint64 length);
    qint64 clusterAt(quint32 cluster) const { return dataStart + qint64(cluster - 2) * clusterBytes(); }
    std::vector<quint32> chain(quint32 start) const;
    QByteArray folderBytes(const Folder& folder);
    void writeSlots(const Folder& folder, const QByteArray& bytes, int first, int count);
    QList<Slot> slotsIn(const Folder& folder);
    Folder folder(const QString& path);
    Folder sub(const Entry& entry) const;
    QPair<Folder, Slot> locate(const QString& path);
    QByteArray rawOf(const Folder& parent, const Slot& slot);
    void eraseSlots(const Folder& parent, const Slot& slot);
    void freeTree(const Entry& entry, int depth);
    void copyOne(const QByteArray& raw, const Entry& entry, const Folder& dest, const QString& name, int depth,
                 const std::function<bool()>& cancelled, const std::function<void(qint64)>& bytes);
    QByteArray folderBlock(quint32 cluster, quint32 parent, quint16 date, quint16 time);
    void refuseTaken(const QList<Slot>& existing, const QString& name);
    std::vector<quint32> allocate(int count);
    void addEntry(const Folder& folder, const QList<Slot>& existing, const QString& name, int attr, quint32 cluster, qint64 size,
                  const QByteArray& templ = {});
    QPair<QByteArray, int> freeSlots(const Folder& folder, int count);
    void change(const std::function<void()>& block);
    void stageFat();
    void commit();

    Blocks& blocks;
    qint64 offset{0}, fatStart{0}, rootStart{0}, dataStart{0};
    int clusterSectors{0}, fatCount{0}, fatSectors{0}, rootEntries{0};
    quint32 clusterCount{0};
    std::vector<quint32> fat, fatOnDisk;
    // Metadata sectors changed by the current change, by card offset, until commit.
    std::map<qint64, QByteArray> staged;
    // A change inside another (replace) commits with the outer one.
    int changing{0};
};

}  // namespace frondCardFile
