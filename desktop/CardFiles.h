#pragma once
// The file manager's operations on a card, the same as Android's CardFiles.kt: paste (copy or
// move), rename, delete, files and folders from the host keeping their structure, copies out to a
// host folder, and the name-clash rules. No widgets, so the tests run it.
#include "FatImage.h"
#include <functional>

namespace frondCardFiles {
using frondCardFile::Entry;
using frondCardFile::FatImage;

enum class Clash { Replace, Skip, KeepBoth, Cancel };

// The answer to a name clash, and whether it holds for the rest of the job.
struct Decision {
    Clash clash{Clash::Cancel};
    bool all{false};
};

// Asks about one clash; a "for all" answer is reused for the rest of the job.
class Resolver {
 public:
    explicit Resolver(std::function<Decision(const QString& name)> ask) : ask(std::move(ask)) {}
    Clash decide(const QString& name);
 private:
    std::function<Decision(const QString&)> ask;
    std::optional<Clash> remembered;
};

struct Report {
    int done{0}, skipped{0};
};

using Cancelled = std::function<bool()>;
using Bytes = std::function<void(qint64)>;

// "name (2).ext", the first that isn't taken, cut to fit FAT's 255.
QString keepBothName(const QString& name, const std::function<bool(const QString&)>& taken);

// Copies or moves each source into destFolder. A folder can't go into itself, and nothing starts if
// any source would. A copy next to its original is named "(2)" without asking.
Report paste(FatImage& fat, const QStringList& sources, const QString& destFolder, bool move, Resolver& resolver,
             const Cancelled& cancelled = {}, const Bytes& bytes = {});
void rename(FatImage& fat, const QString& path, const QString& newName);
int remove(FatImage& fat, const QStringList& paths, const Cancelled& cancelled = {});
// The bytes in the sources, for a progress bar.
qint64 bytesIn(FatImage& fat, const QStringList& paths);

// A host file into the folder under `name` (its own name when empty), with the clash rule. Returns
// the name it went in under, or empty when skipped.
QString addFile(FatImage& fat, const QString& folder, const QString& hostFile, Resolver& resolver, const Cancelled& cancelled = {},
                const Bytes& bytes = {}, const QString& name = {});
// Host files and folders into `folder`, a folder each with everything in it. A folder already
// there is filled; a file's clash follows the resolver.
Report addHost(FatImage& fat, const QStringList& hostPaths, const QString& folder, Resolver& resolver, const Cancelled& cancelled = {},
               const Bytes& bytes = {});
// The file bytes under host paths, and a problem that stops them going on a card.
qint64 hostBytes(const QStringList& hostPaths, QString* problem = nullptr);

// Copies files and folders off the card into a host folder. A name already there gets " (2)".
// Returns the number of files.
int exportTo(FatImage& fat, const QStringList& paths, const QString& hostFolder, const Cancelled& cancelled = {}, const Bytes& bytes = {});

}  // namespace frondCardFiles
