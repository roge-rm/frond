#pragma once
// A host folder copied once onto a new SD or Memory Stick Pro card: the folder's size on a FAT16
// card of each size (as `frond --card make` formats one), and the copy itself through the storage
// library's FatImage. Header only; CardStore.h calls it.
#include "CardStorage.h"
#include "FatWrite.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QPair>
#include <QSet>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QVector>
#include <algorithm>
#include <functional>

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

// Copies the folder's tree onto a new card's file system through the storage library, in one
// change with one commit (FatImage::batch): folders and files with their dates. `progress` gets
// the file bytes so far; false cancels, and then nothing of it stays. Empty on success.
inline QString fill(frondCardFile::FatImage& fat, const QString& folder, const Plan& plan, const Progress& progress) {
    const qint64 total = std::max<qint64>(plan.bytes, 1);
    qint64 done = 0;
    const auto filters = QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks;
    try {
        fat.batch([&] {
            std::function<void(const QString&, const QString&)> put = [&](const QString& dir, const QString& path) {
                for (const auto& e : QDir(dir).entryInfoList(filters, QDir::Name)) {
                    if (e.isDir()) {
                        fat.makeFolder(path, e.fileName(), e.lastModified());
                        put(e.absoluteFilePath(), frondCardFile::FatImage::joined(path, e.fileName()));
                        continue;
                    }
                    QFile in(e.absoluteFilePath());
                    if (!in.open(QIODevice::ReadOnly)) throw frondCardFile::Error(QObject::tr("Couldn't read %1.").arg(e.fileName()));
                    fat.add(path, e.fileName(), [&](char* b, qint64 n) {
                        const qint64 got = in.read(b, n);
                        if (got <= 0) return qint64(0);
                        done += got;
                        if (progress && !progress(std::min(done, total), total)) throw frondCardFile::Cancelled();
                        return got;
                    }, e.size(), e.lastModified());
                }
            };
            put(folder, QStringLiteral("/"));
        });
    } catch (const frondCardFile::Cancelled&) {
        return QObject::tr("Cancelled.");
    } catch (const frondCardFile::Error& e) {
        return e.message.contains(QStringLiteral("enough space")) ? QObject::tr("The folder doesn't fit on a card that size.")
                                                                  : QObject::tr("Couldn't copy the folder: %1").arg(e.message);
    }
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
