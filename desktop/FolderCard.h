#pragma once
// A host folder copied once onto a new SD or Memory Stick Pro card: the folder's size on a FAT16
// card of each size, and the copy itself into a raw image (checked by reading it back with
// mtools) that the card tool then turns into a card file. Header only; CardStore.h calls it.
#include "FatWrite.h"
#include "SdImage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDirIterator>
#include <QPair>
#include <QSet>
#include <QList>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QVector>
#include <algorithm>
#include <functional>
#include <sys/stat.h>

namespace frondFolderCard {

// Called with bytes done and the total; false cancels.
using Progress = std::function<bool(qint64, qint64)>;

// What a folder takes on a card: its files' sizes and how many directory slots each folder needs.
struct Plan {
    QString problem;            // set when the folder can't go on a card at all
    qint64 bytes{0};            // file bytes
    int files{0}, folders{0};
    int rootSlots{0};
    QVector<qint64> sizes;      // one per file
    QVector<int> folderSlots;   // one per folder below the top: its entries, long names, "." and ".."
    bool cancelled{false};
};

// The FAT16 layout of a card of `mib` MiB, as frondSd::layout() makes it.
struct Geometry { unsigned clusterBytes{0}, clusters{0}; };
inline Geometry geometry(unsigned mib) {
    const uint32_t start = 63, total = uint32_t((uint64_t(mib) << 20) / 512), sectors = total - start;
    const uint8_t cluster = mib <= 64 ? 4 : mib <= 256 ? 8 : mib == 512 ? 16 : mib == 1024 ? 32 : 64;
    uint16_t fatSectors = 1;
    const uint16_t reserved = 1;
    uint32_t clusters = 0;
    for (;;) {
        clusters = (sectors - reserved - 32 - 2 * fatSectors) / cluster;
        if (clusters > 65524) clusters = 65524;
        const auto required = uint16_t(((clusters + 2) * 2 + 511) / 512);
        if (fatSectors >= required) break;
        fatSectors = required;
    }
    return {unsigned(cluster) * 512, clusters};
}

// Directory slots for one name: the short entry and its long-name entries (counted for every name).
inline int slotsFor(const QString& name) { return 1 + (name.size() + 12) / 13; }

inline bool nameUsable(const QString& name) {
    static const QRegularExpression forbidden(QStringLiteral(R"([\x00-\x1f\x7f*?<>|":\\/])"));
    return !name.isEmpty() && name.size() <= 255 && name != "." && name != ".." && !name.endsWith('.') && !name.endsWith(' ') &&
           !name.startsWith(' ') && !forbidden.match(name).hasMatch();
}

// Walks `folder`. Links are skipped. A name FAT can't hold, or two names that differ only in case,
// stops it with `problem` set.
inline void walk(const QString& dir, bool top, Plan& plan, const std::function<bool()>& cancelled) {
    if (!plan.problem.isEmpty()) return;
    if (cancelled && cancelled()) { plan.cancelled = true; return; }
    const auto entries = QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks);
    int used = top ? 0 : 2;
    QStringList seen;
    for (const auto& e : entries) {
        if (!nameUsable(e.fileName())) {
            plan.problem = QObject::tr("%1 can't be used on a card.").arg(e.fileName());
            return;
        }
        const auto lower = e.fileName().toLower();
        if (seen.contains(lower)) {
            plan.problem = QObject::tr("%1 and another name differ only in case, which a card can't tell apart.").arg(e.fileName());
            return;
        }
        seen << lower;
        used += slotsFor(e.fileName());
        if (e.isDir()) {
            ++plan.folders;
            walk(e.absoluteFilePath(), false, plan, cancelled);
            if (!plan.problem.isEmpty() || plan.cancelled) return;
        } else {
            ++plan.files;
            plan.bytes += e.size();
            plan.sizes << e.size();
        }
    }
    if (top) plan.rootSlots = used; else plan.folderSlots << used;
}

inline Plan measure(const QString& folder, const std::function<bool()>& cancelled = {}) {
    Plan plan;
    walk(folder, true, plan, cancelled);
    // The root folder of a FAT16 card has 512 slots, one of them the volume label.
    if (plan.problem.isEmpty() && !plan.cancelled && plan.rootSlots > 511)
        plan.problem = QObject::tr("There are too many names in the top folder for a card.");
    return plan;
}

// Bytes the folder takes in whole clusters on a card of `mib`, with a few spare clusters.
inline qint64 clusterBytesNeeded(const Plan& plan, unsigned mib) {
    const auto g = geometry(mib);
    qint64 clusters = 4;
    for (auto size : plan.sizes) clusters += (size + g.clusterBytes - 1) / g.clusterBytes;
    for (int used : plan.folderSlots) clusters += std::max<qint64>(1, (qint64(used) * 32 + g.clusterBytes - 1) / g.clusterBytes);
    return clusters * g.clusterBytes;
}
inline bool fits(const Plan& plan, unsigned mib) {
    if (!plan.problem.isEmpty()) return false;
    const auto g = geometry(mib);
    return clusterBytesNeeded(plan, mib) <= qint64(g.clusters) * g.clusterBytes;
}
// The smallest of `sizes` (MiB, ascending) the folder fits on, or -1.
inline int smallestFit(const Plan& plan, const QList<int>& sizes) {
    for (int mib : sizes)
        if (fits(plan, unsigned(mib))) return mib;
    return -1;
}

// ---- Copying ----

inline QProcessEnvironment mtoolsEnvironment() {
    auto env = QProcessEnvironment::systemEnvironment();
    for (const auto& key : env.keys())
        if (key.startsWith("MTOOLS")) env.remove(key);
    env.insert("MTOOLSRC", "/dev/null");
    env.insert("LC_ALL", "C.UTF-8");
    return env;
}
inline bool mtoolsAvailable() {
    return !QStandardPaths::findExecutable("mcopy").isEmpty();
}

// A blank FAT16 card of `mib` MiB as a sparse raw image: the same partition and file system
// layout a new card has. Returns false when it can't be written.
inline bool blankImage(const QString& path, unsigned mib) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || !f.resize(qint64(mib) << 20)) return false;
    for (const auto& s : frondSd::layout(mib))
        if (!f.seek(qint64(s.index) * 512) || f.write(reinterpret_cast<const char*>(s.bytes.data()), 512) != 512) return false;
    return f.flush();
}
// The partition starts at sector 63 in every card made here.
constexpr qint64 partitionOffset = 63 * 512;

// Runs an mtools program on the image; polls `progress` with the image's allocated bytes so the
// caller sees the copy grow and can cancel. Empty on success.
inline QString runMtools(const QString& tool, const QString& image, const QStringList& args, const Progress& progress, qint64 total,
                         bool* cancelled = nullptr) {
    QProcess p;
    p.setProcessEnvironment(mtoolsEnvironment());
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStandardPaths::findExecutable(tool),
            QStringList{QStringLiteral("-i"), image + "@@" + QString::number(partitionOffset)} + args);
    p.closeWriteChannel();
    if (!p.waitForStarted(5000)) return QObject::tr("Install mtools to import a folder (Debian: sudo apt install mtools).");
    QByteArray output;
    while (p.state() != QProcess::NotRunning) {
        p.waitForFinished(100);
        output += p.readAll();
        if (!progress) continue;
        struct stat st{};
        const qint64 done = ::stat(QFile::encodeName(image).constData(), &st) == 0 ? qint64(st.st_blocks) * 512 : 0;
        if (!progress(std::min(done, total), total)) {
            p.kill();
            p.waitForFinished();
            if (cancelled) *cancelled = true;
            return QObject::tr("Cancelled.");
        }
    }
    output += p.readAll();
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        const auto text = QString::fromUtf8(output).trimmed();
        return text.contains("full", Qt::CaseInsensitive)
                   ? QObject::tr("The folder doesn't fit on a card that size.")
                   : QObject::tr("Couldn't copy the folder: %1").arg(text);
    }
    return {};
}

// ---- Writing the folder onto the blank FAT16 image ----
// Names and entries come from FatWrite.h.

struct TreeNode {
    QString name, path;
    bool dir{false};
    qint64 size{0};
    QDateTime when;
    uint32_t cluster{0}, clusters{0};
    std::vector<TreeNode> kids;
};
inline void readTree(const QString& path, TreeNode& node) {
    for (const auto& e : QDir(path).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks)) {
        TreeNode kid;
        kid.name = e.fileName(); kid.path = e.absoluteFilePath(); kid.dir = e.isDir(); kid.size = kid.dir ? 0 : e.size();
        kid.when = e.lastModified();
        if (kid.dir) readTree(kid.path, kid);
        node.kids.push_back(std::move(kid));
    }
}
// The entries of a folder: "." and ".." below the top, then one per child.
inline QByteArray folderEntries(const TreeNode& node, bool top, uint32_t parentCluster) {
    QByteArray out;
    if (!top) {
        QByteArray dot(11, ' '), dotdot(11, ' ');
        dot[0] = '.'; dotdot[0] = '.'; dotdot[1] = '.';
        const auto entry = [&](const QByteArray& n, uint32_t cluster) {
            auto e = dirEntry(shortDisplay(n), n, 0x10, cluster, 0, node.when);
            return e.right(32);
        };
        out += entry(dot, node.cluster);
        out += entry(dotdot, parentCluster);
    }
    QSet<QString> taken;
    for (const auto& kid : node.kids) {
        const auto s = shortName(kid.name, taken);
        taken.insert(shortDisplay(s));
        out += dirEntry(kid.name, s, kid.dir ? 0x10 : 0x20, kid.cluster, uint32_t(kid.size), kid.when);
    }
    return out;
}

// Writes the folder's contents to the top of a blank card image made by blankImage. Files and
// folders each get a run of clusters. Empty on success.
inline QString writeFolder(const QString& image, const QString& folder, const Plan& plan, const Progress& progress) {
    QFile f(image);
    if (!f.open(QIODevice::ReadWrite)) return QObject::tr("Couldn't make the card.");
    if (!f.seek(partitionOffset)) return QObject::tr("Couldn't make the card.");
    const auto boot = f.read(512);
    if (boot.size() != 512) return QObject::tr("Couldn't make the card.");
    const uint32_t sectorBytes = le16(boot, 11), perCluster = uint8_t(boot[13]), reserved = le16(boot, 14), fats = uint8_t(boot[16]),
                   rootEntries = le16(boot, 17), fatSectors = le16(boot, 22);
    const uint32_t sectors = le16(boot, 19) ? le16(boot, 19) : le32(boot, 32);
    if (sectorBytes != 512 || !perCluster || !fats || !fatSectors) return QObject::tr("Couldn't make the card.");
    const uint32_t rootStart = reserved + fats * fatSectors, dataStart = rootStart + rootEntries * 32 / 512;
    const uint32_t clusterCount = std::min<uint32_t>((sectors - dataStart) / perCluster, 65524);
    const qint64 clusterBytes = qint64(perCluster) * 512;
    const auto sectorAt = [&](uint32_t sector) { return partitionOffset + qint64(sector) * 512; };
    const auto clusterAt = [&](uint32_t c) { return sectorAt(dataStart + (c - 2) * perCluster); };

    TreeNode root;
    root.dir = true;
    root.when = QFileInfo(folder).lastModified();
    readTree(folder, root);
    // Clusters in order: each folder, then its files, then its subfolders.
    uint32_t next = 2;
    bool full = false;
    std::function<void(TreeNode&, bool)> place = [&](TreeNode& node, bool top) {
        if (!top) {
            // Sized with the children's short names unknown; the entry count doesn't depend on them.
            const qint64 bytes = folderEntries(node, false, 0).size();
            node.clusters = uint32_t(std::max<qint64>(1, (bytes + clusterBytes - 1) / clusterBytes));
            node.cluster = next; next += node.clusters;
        }
        for (auto& kid : node.kids) {
            if (kid.dir) continue;
            kid.clusters = uint32_t((kid.size + clusterBytes - 1) / clusterBytes);
            kid.cluster = kid.clusters ? next : 0;
            next += kid.clusters;
        }
        for (auto& kid : node.kids) if (kid.dir) place(kid, false);
        if (next > clusterCount + 2) full = true;
    };
    place(root, true);
    if (full) return QObject::tr("The folder doesn't fit on a card that size.");

    const qint64 total = std::max<qint64>(plan.bytes, 1);
    qint64 done = 0;
    QString error;
    std::vector<uint16_t> fat(fatSectors * 256, 0);
    fat[0] = 0xfff8; fat[1] = 0xffff;
    const auto chain = [&](uint32_t first, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) fat[first + i] = i + 1 < count ? uint16_t(first + i + 1) : uint16_t(0xffff);
    };
    std::function<bool(const TreeNode&, bool, uint32_t)> write = [&](const TreeNode& node, bool top, uint32_t parent) {
        const auto entries = folderEntries(node, top, parent);
        if (top) {
            // Slot 0 holds the volume label.
            if (entries.size() > qint64(rootEntries - 1) * 32) { error = QObject::tr("There are too many names in the top folder for a card."); return false; }
            if (!f.seek(sectorAt(rootStart) + 32) || f.write(entries) != entries.size()) { error = QObject::tr("Couldn't write the card."); return false; }
        } else {
            chain(node.cluster, node.clusters);
            QByteArray block(qint64(node.clusters) * clusterBytes, '\0');
            block.replace(0, entries.size(), entries);
            if (!f.seek(clusterAt(node.cluster)) || f.write(block) != block.size()) { error = QObject::tr("Couldn't write the card."); return false; }
        }
        for (const auto& kid : node.kids) {
            if (kid.dir) continue;
            if (kid.clusters) chain(kid.cluster, kid.clusters);
            QFile in(kid.path);
            if (!in.open(QIODevice::ReadOnly)) { error = QObject::tr("Couldn't read %1.").arg(kid.name); return false; }
            if (kid.clusters && !f.seek(clusterAt(kid.cluster))) { error = QObject::tr("Couldn't write the card."); return false; }
            qint64 left = kid.size;
            while (left > 0) {
                const auto chunk = in.read(std::min<qint64>(left, 1 << 20));
                if (chunk.isEmpty()) { error = QObject::tr("Couldn't read %1.").arg(kid.name); return false; }
                if (f.write(chunk) != chunk.size()) { error = QObject::tr("Couldn't write the card."); return false; }
                left -= chunk.size();
                done += chunk.size();
                if (progress && !progress(std::min(done, total), total)) { error = QObject::tr("Cancelled."); return false; }
            }
        }
        for (const auto& kid : node.kids)
            if (kid.dir && !write(kid, false, top ? 0 : node.cluster)) return false;
        return true;
    };
    if (!write(root, true, 0)) return error;
    QByteArray table(qint64(fatSectors) * 512, '\0');
    for (size_t i = 0; i < fat.size(); ++i) putLe16(table, int(i * 2), fat[i]);
    for (uint32_t copy = 0; copy < fats; ++copy)
        if (!f.seek(sectorAt(reserved + copy * fatSectors)) || f.write(table) != table.size()) return QObject::tr("Couldn't write the card.");
    if (!f.flush()) return QObject::tr("Couldn't write the card.");
    if (progress && !progress(total, total)) return QObject::tr("Cancelled.");
    return {};
}

// Writes the folder's contents to the top of the card image, then reads them back with mtools,
// a reader of its own, for sameTree. `scratch` is a folder for the read back. Empty on success.
inline QString copyFolder(const QString& image, const QString& folder, const QString& scratch, const Plan& plan, const Progress& progress) {
    if (QDir(folder).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks).isEmpty()) return {};
    if (const auto error = writeFolder(image, folder, plan, progress); !error.isEmpty()) return error;
    QDir().mkpath(scratch);
    const qint64 total = std::max<qint64>(plan.bytes, 1);
    if (const auto error = runMtools(QStringLiteral("mcopy"), image, {QStringLiteral("-s"), QStringLiteral("-m"), QStringLiteral("::/*"), scratch}, {}, total);
        !error.isEmpty())
        return error;
    return {};
}

// Whether `copy` holds the same tree as `source`: the same names exactly, sizes and bytes.
inline bool sameTree(const QString& source, const QString& copy) {
    const auto a = QDir(source).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks);
    const auto b = QDir(copy).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks);
    if (a.size() != b.size()) return false;
    for (const auto& x : a) {
        const auto match = std::find_if(b.begin(), b.end(), [&](const QFileInfo& y) { return y.fileName() == x.fileName(); });
        if (match == b.end() || match->isDir() != x.isDir()) return false;
        if (x.isDir()) {
            if (!sameTree(x.absoluteFilePath(), match->absoluteFilePath())) return false;
        } else {
            if (x.size() != match->size()) return false;
            QFile fx(x.absoluteFilePath()), fy(match->absoluteFilePath());
            if (!fx.open(QIODevice::ReadOnly) || !fy.open(QIODevice::ReadOnly)) return false;
            while (!fx.atEnd()) {
                if (fx.read(1 << 20) != fy.read(1 << 20)) return false;
            }
        }
    }
    return true;
}

}  // namespace frondFolderCard
