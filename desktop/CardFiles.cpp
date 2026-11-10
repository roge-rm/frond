#include "CardFiles.h"
#include "FolderCard.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include "zip/miniz.h"

namespace frondCardFiles {
using frondCardFile::Cancelled;
using Error = frondCardFile::Error;

namespace {
QString lastName(const QString& path) { return FatImage::parts(path).value(FatImage::parts(path).size() - 1); }
QString parentOf(const QString& path) {
    auto p = FatImage::parts(path);
    if (!p.isEmpty()) p.removeLast();
    return '/' + p.join('/');
}
QSet<QString> names(FatImage& fat, const QString& folder) {
    QSet<QString> out;
    for (const auto& e : fat.list(folder)) out.insert(e.name.toLower());
    return out;
}
std::optional<Entry> named(FatImage& fat, const QString& folder, const QString& name) {
    for (const auto& e : fat.list(folder))
        if (e.name.compare(name, Qt::CaseInsensitive) == 0) return e;
    return std::nullopt;
}
QString keepBoth(FatImage& fat, const QString& folder, const QString& name) {
    const auto used = names(fat, folder);
    return keepBothName(name, [&](const QString& n) { return used.contains(n.toLower()); });
}
void check(const Cancelled& cancelled) {
    if (cancelled && cancelled()) throw frondCardFile::Cancelled();
}
}  // namespace

Clash Resolver::decide(const QString& name) {
    if (remembered) return *remembered;
    const auto d = ask(name);
    if (d.all && d.clash != Clash::Cancel) remembered = d.clash;
    return d.clash;
}

QString keepBothName(const QString& name, const std::function<bool(const QString&)>& taken) {
    const auto dotAt = name.lastIndexOf('.');
    const int dot = dotAt > 0 ? int(dotAt) : -1;
    const auto base = dot >= 0 ? name.left(dot) : name;
    const auto ext = dot >= 0 ? name.mid(dot) : QString();
    for (int n = 2; n <= 99999; ++n) {
        const auto tail = QStringLiteral(" (%1)").arg(n) + ext;
        auto room = base.left(std::max<qsizetype>(1, 255 - tail.size()));
        if (!room.isEmpty() && room.back().isHighSurrogate()) room.chop(1);
        while (room.endsWith(' ')) room.chop(1);
        const auto candidate = room + tail;
        if (!taken(candidate)) return candidate;
    }
    throw Error(QStringLiteral("Too many similar names in this folder."));
}

Report paste(FatImage& fat, const QStringList& sources, const QString& destFolder, bool move, Resolver& resolver,
             const Cancelled& cancelled, const Bytes& bytes) {
    for (const auto& source : sources) {
        const auto entry = fat.find(source);
        if (!entry) throw Error(QStringLiteral("%1 isn't on the card.").arg(lastName(source)));
        if (entry->directory && FatImage::within(source, destFolder)) throw Error(QStringLiteral("%1 can't go into itself.").arg(entry->name));
    }
    Report report;
    qint64 copied = 0;
    for (const auto& source : sources) {
        check(cancelled);
        const auto name = lastName(source);
        const bool here = FatImage::parts(parentOf(source.toLower())) == FatImage::parts(destFolder.toLower());
        if (move && here) { report.skipped++; continue; }
        auto target = name;
        QString victim;
        if (const auto taken = named(fat, destFolder, name)) {
            switch (here ? Clash::KeepBoth : resolver.decide(name)) {
            case Clash::Cancel: throw frondCardFile::Cancelled();
            case Clash::Skip: report.skipped++; continue;
            case Clash::KeepBoth: target = keepBoth(fat, destFolder, name); break;
            case Clash::Replace: {
                victim = FatImage::joined(destFolder, taken->name);
                if (FatImage::within(victim, source)) throw Error(QStringLiteral("%1 holds what you are replacing it with.").arg(taken->name));
                break;
            }
            }
        }
        const auto base = copied;
        const auto work = [&] {
            if (move) fat.move(source, destFolder, target);
            else fat.copy(source, destFolder, target, cancelled, [&](qint64 n) { if (bytes) bytes(base + n); });
        };
        // A replace swaps the entries in one commit once the new one is complete.
        if (victim.isEmpty()) work();
        else fat.replace(victim, work);
        if (!move) copied += fat.sizeOf(FatImage::joined(destFolder, target));
        report.done++;
    }
    return report;
}

void rename(FatImage& fat, const QString& path, const QString& newName) {
    const auto name = newName.trimmed();
    FatImage::checkName(name);
    if (lastName(path) == name) return;
    fat.move(path, parentOf(path), name);
}

int remove(FatImage& fat, const QStringList& paths, const Cancelled& cancelled) {
    int n = 0;
    for (const auto& path : paths) {
        check(cancelled);
        fat.remove(path);
        n++;
    }
    return n;
}

qint64 bytesIn(FatImage& fat, const QStringList& paths) {
    qint64 n = 0;
    for (const auto& p : paths) n += fat.sizeOf(p);
    return n;
}

QString addFile(FatImage& fat, const QString& folder, const QString& hostFile, Resolver& resolver, const Cancelled& cancelled,
                const Bytes& bytes, const QString& wanted) {
    const auto name = wanted.isEmpty() ? QFileInfo(hostFile).fileName() : wanted;
    FatImage::checkName(name);
    QFile in(hostFile);
    if (!in.open(QIODevice::ReadOnly)) throw Error(QStringLiteral("Couldn't read %1.").arg(name));
    const qint64 size = in.size();
    auto target = name;
    QString victim;
    if (const auto taken = named(fat, folder, name)) {
        switch (resolver.decide(name)) {
        case Clash::Cancel: throw frondCardFile::Cancelled();
        case Clash::Skip: return {};
        case Clash::KeepBoth: target = keepBoth(fat, folder, name); break;
        case Clash::Replace:
            if (taken->directory) throw Error(QStringLiteral("%1 is a folder on the card.").arg(name));
            victim = FatImage::joined(folder, taken->name);
            break;
        }
    }
    qint64 done = 0;
    const auto work = [&] {
        fat.add(folder, target, [&](char* b, qint64 n) -> qint64 {
            check(cancelled);
            const auto r = in.read(b, n);
            if (r < 0) throw Error(QStringLiteral("Couldn't read %1.").arg(name));
            done += r;
            if (bytes) bytes(done);
            return r;
        }, size);
    };
    // A replace keeps the old file until the new one is complete, then swaps them in one commit.
    if (victim.isEmpty()) work();
    else fat.replace(victim, work);
    return target;
}

qint64 hostBytes(const QStringList& hostPaths, QString* problem) {
    qint64 total = 0;
    for (const auto& path : hostPaths) {
        const QFileInfo info(path);
        if (!frondFolderCard::nameUsable(info.fileName())) {
            if (problem) *problem = QStringLiteral("%1 can't be used on a card.").arg(info.fileName());
            return total;
        }
        if (info.isDir()) {
            const auto plan = frondFolderCard::measure(path);
            if (!plan.problem.isEmpty() && problem && problem->isEmpty()) *problem = plan.problem;
            total += plan.bytes;
        } else total += info.size();
    }
    return total;
}

Report addHost(FatImage& fat, const QStringList& hostPaths, const QString& folder, Resolver& resolver, const Cancelled& cancelled,
               const Bytes& bytes) {
    Report report;
    qint64 copied = 0;
    std::function<void(const QString&, const QString&)> into = [&](const QString& parent, const QString& host) {
        check(cancelled);
        const QFileInfo info(host);
        auto name = info.fileName();
        if (!frondFolderCard::nameUsable(name)) throw Error(QStringLiteral("\"%1\" can't be used on a card.").arg(name));
        if (info.isDir()) {
            const auto taken = named(fat, parent, name);
            if (!taken) fat.makeFolder(parent, name);
            else if (taken->directory) name = taken->name;
            else {
                name = keepBoth(fat, parent, name);
                fat.makeFolder(parent, name);
            }
            const auto here = FatImage::joined(parent, name);
            const auto children = QDir(host).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks,
                                                           QDir::Name | QDir::DirsFirst);
            for (const auto& child : children) into(here, child.absoluteFilePath());
        } else if (info.isFile()) {
            const auto base = copied;
            const auto put = addFile(fat, parent, host, resolver, cancelled, [&](qint64 n) { if (bytes) bytes(base + n); });
            if (put.isEmpty()) report.skipped++; else report.done++;
            copied += info.size();
        }
    };
    for (const auto& host : hostPaths) into(folder, host);
    return report;
}

int exportTo(FatImage& fat, const QStringList& paths, const QString& hostFolder, const Cancelled& cancelled, const Bytes& bytes) {
    int files = 0;
    qint64 copied = 0;
    auto freeName = [](const QString& dir, const QString& name) {
        if (!QFileInfo(dir + '/' + name).exists() && !QFileInfo(dir + '/' + name).isSymLink()) return name;
        return keepBothName(name, [&](const QString& n) { return QFileInfo(dir + '/' + n).exists() || QFileInfo(dir + '/' + n).isSymLink(); });
    };
    std::function<void(const QString&, const Entry&, const QString&)> out = [&](const QString& path, const Entry& entry, const QString& dir) {
        check(cancelled);
        const auto name = freeName(dir, entry.name);
        const auto target = dir + '/' + name;
        if (entry.directory) {
            if (!QDir().mkdir(target)) throw Error(QStringLiteral("Couldn't make the folder %1.").arg(name));
            for (const auto& child : fat.list(path)) out(FatImage::joined(path, child.name), child, target);
        } else {
            // Never over an existing file.
            QFile file(target);
            if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) throw Error(QStringLiteral("Couldn't write %1.").arg(name));
            try {
                fat.read(entry, [&](const char* b, qint64 n) {
                    check(cancelled);
                    if (file.write(b, n) != n) throw Error(QStringLiteral("Couldn't write %1.").arg(name));
                    copied += n;
                    if (bytes) bytes(copied);
                });
                if (!file.flush()) throw Error(QStringLiteral("Couldn't write %1.").arg(name));
            } catch (...) {
                file.close();
                file.remove();
                throw;
            }
            file.close();
            if (entry.modified.isValid()) {
                if (file.open(QIODevice::ReadWrite)) {
                    file.setFileTime(entry.modified, QFileDevice::FileModificationTime);
                    file.close();
                }
            }
            files++;
        }
    };
    for (const auto& path : paths) {
        const auto entry = fat.find(path);
        if (!entry) throw Error(QStringLiteral("%1 isn't on the card.").arg(lastName(path)));
        out(path, *entry, hostFolder);
    }
    return files;
}

int unzipInto(const QString& zip, const QString& hostFolder, QString* error) {
    auto fail = [&](const QString& message) {
        if (error) *error = message;
        return -1;
    };
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_file(&archive, QFile::encodeName(zip).constData(), 0)) return fail(QStringLiteral("Couldn't read the export."));
    int files = 0;
    QString problem;
    const auto count = mz_zip_reader_get_num_files(&archive);
    for (mz_uint i = 0; i < count && problem.isEmpty(); ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&archive, i, &stat)) { problem = QStringLiteral("Couldn't read the export."); break; }
        if (mz_zip_reader_is_file_a_directory(&archive, i)) continue;
        auto name = QString::fromUtf8(stat.m_filename).section('/', -1);
        if (name.isEmpty() || name == "." || name == "..") continue;
        if (QFileInfo::exists(hostFolder + '/' + name))
            name = keepBothName(name, [&](const QString& n) { return QFileInfo::exists(hostFolder + '/' + n); });
        QFile out(hostFolder + '/' + name);
        if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { problem = QStringLiteral("Couldn't write %1.").arg(name); break; }
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&archive, i, &size, 0);
        const bool written = data && out.write(static_cast<const char*>(data), qint64(size)) == qint64(size) && out.flush();
        mz_free(data);
        out.close();
        if (!written) {
            out.remove();
            problem = QStringLiteral("Couldn't write %1.").arg(name);
            break;
        }
        files++;
    }
    mz_zip_reader_end(&archive);
    if (!problem.isEmpty()) return fail(problem);
    return files;
}

}  // namespace frondCardFiles
