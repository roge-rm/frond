#include "FatImage.h"
#include "FatWrite.h"
#include <QDate>
#include <QTime>
#include <algorithm>
#include <set>

namespace frondCardFile {
namespace {

constexpr int maxDepth = 64;
constexpr int longOffsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

int u16(const QByteArray& b, qint64 at) { return uint8_t(b[int(at)]) | (uint8_t(b[int(at + 1)]) << 8); }
quint32 u32(const QByteArray& b, qint64 at) { return quint32(u16(b, at)) | (quint32(u16(b, at + 2)) << 16); }
void put16(QByteArray& b, qint64 at, int v) { b[int(at)] = char(v); b[int(at + 1)] = char(v >> 8); }
void put32(QByteArray& b, qint64 at, quint32 v) { put16(b, at, int(v & 0xffff)); put16(b, at + 2, int(v >> 16)); }

QString longPart(const QByteArray& b, int at) {
    QString s;
    for (int o : longOffsets) {
        const int c = u16(b, at + o);
        if (c == 0 || c == 0xffff) break;
        s += QChar(char16_t(c));
    }
    return s;
}
// The short name as a reader shows it, with the case bits other writers set.
QString shortDisplay(const QByteArray& b, int at) {
    const int caseBits = uint8_t(b[at + 12]);
    QByteArray raw = b.mid(at, 11);
    if (uint8_t(raw[0]) == 0x05) raw[0] = char(0xe5);
    QString base = QString::fromLatin1(raw.left(8));
    QString ext = QString::fromLatin1(raw.mid(8, 3));
    while (base.endsWith(' ')) base.chop(1);
    while (ext.endsWith(' ')) ext.chop(1);
    if (caseBits & 0x08) base = base.toLower();
    if (caseBits & 0x10) ext = ext.toLower();
    return ext.isEmpty() ? base : base + '.' + ext;
}
int checksum(const QByteArray& b, int at) {
    int sum = 0;
    for (int i = 0; i < 11; ++i) sum = (((sum & 1) << 7) + (sum >> 1) + uint8_t(b[at + i])) & 0xff;
    return sum;
}
QDateTime fromDos(int date, int time) {
    if (date == 0) return {};
    const QDate d(1980 + (date >> 9), (date >> 5) & 15, date & 31);
    const QTime t(time >> 11, (time >> 5) & 63, (time & 31) * 2);
    if (!d.isValid()) return {};
    return QDateTime(d, t.isValid() ? t : QTime(0, 0));
}
void stamp(QByteArray& b, int at, quint16 date, quint16 time) {
    put16(b, at + 14, time);
    put16(b, at + 16, date);
    put16(b, at + 18, date);
    put16(b, at + 22, time);
    put16(b, at + 24, date);
}
bool sameName(const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) == 0; }

}  // namespace

FatImage::FatImage(Blocks& b) : blocks(b) {
    const auto size = blocks.length();
    if (size < (qint64(32) << 20) || size > (qint64(2048) << 20) || size % sectorBytes)
        throw Error(QStringLiteral("Use a 32 MB to 2 GB FAT16 card image."));
    auto boot = readAt(0, sectorBytes);
    if (boot.mid(54, 8) != "FAT16   ") {
        // A single primary partition, as the core makes. Never guess offsets.
        const int type = uint8_t(boot[450]);
        const qint64 begin = u32(boot, 454), sectors = u32(boot, 458);
        if (u16(boot, 510) != 0xaa55 || (type != 4 && type != 6 && type != 14) || !begin || !sectors || (begin + sectors) * sectorBytes > size)
            throw Error(QStringLiteral("The card has no FAT16 partition."));
        offset = begin * sectorBytes;
        boot = readAt(offset, sectorBytes);
    }
    if (boot.mid(54, 8) != "FAT16   " || u16(boot, 11) != sectorBytes || u16(boot, 510) != 0xaa55)
        throw Error(QStringLiteral("The card doesn't hold a FAT16 file system."));
    clusterSectors = uint8_t(boot[13]);
    const int reserved = u16(boot, 14);
    fatCount = uint8_t(boot[16]);
    rootEntries = u16(boot, 17);
    const qint64 total = u16(boot, 19) ? qint64(u16(boot, 19)) : qint64(u32(boot, 32));
    fatSectors = u16(boot, 22);
    if (!clusterSectors || (clusterSectors & (clusterSectors - 1)) || !reserved || !fatCount || !fatSectors || !rootEntries)
        throw Error(QStringLiteral("The card's FAT16 header is damaged."));
    fatStart = offset + qint64(reserved) * sectorBytes;
    rootStart = fatStart + qint64(fatCount) * fatSectors * sectorBytes;
    const int rootSectors = (rootEntries * 32 + sectorBytes - 1) / sectorBytes;
    dataStart = rootStart + qint64(rootSectors) * sectorBytes;
    const qint64 dataSectors = total - (reserved + qint64(fatCount) * fatSectors + rootSectors);
    const qint64 count = dataSectors / clusterSectors;
    if (count < 4085 || count >= 65525 || offset + total * sectorBytes > size || (count + 2) * 2 > qint64(fatSectors) * sectorBytes)
        throw Error(QStringLiteral("The card's FAT16 header is damaged."));
    clusterCount = quint32(count);
    const auto raw = readAt(fatStart, (count + 2) * 2);
    fat.resize(size_t(count + 2));
    for (size_t i = 0; i < fat.size(); ++i) fat[i] = quint32(u16(raw, qint64(i) * 2));
    fatOnDisk = fat;
}

qint64 FatImage::freeBytes() const {
    qint64 n = 0;
    for (quint32 c = 2; c < clusterCount + 2; ++c) if (!fat[c]) ++n;
    return n * clusterBytes();
}

// Card bytes, with staged sectors laid over them so a change sees its own edits.
QByteArray FatImage::readAt(qint64 at, qint64 length) {
    QByteArray b(int(length), '\0');
    blocks.read(at, b.data(), length);
    const qint64 first = at / sectorBytes * sectorBytes;
    for (auto it = staged.lower_bound(first); it != staged.end() && it->first < at + length; ++it) {
        const qint64 from = std::max(at, it->first);
        const qint64 until = std::min(at + length, it->first + sectorBytes);
        std::memcpy(b.data() + (from - at), it->second.constData() + (from - it->first), size_t(until - from));
    }
    return b;
}
// Straight to the card: new data in free clusters.
void FatImage::writeData(qint64 at, const QByteArray& b) {
    blocks.write(at, b.constData(), b.size());
    bytesWritten += b.size();
}
// Keeps a metadata edit in memory, whole sectors, until commit.
void FatImage::stage(qint64 at, const char* b, qint64 length) {
    qint64 done = 0;
    while (done < length) {
        const qint64 position = at + done;
        const qint64 sectorAt = position / sectorBytes * sectorBytes;
        auto it = staged.find(sectorAt);
        if (it == staged.end()) {
            QByteArray sector(sectorBytes, '\0');
            blocks.read(sectorAt, sector.data(), sectorBytes);
            it = staged.emplace(sectorAt, sector).first;
        }
        const qint64 inSector = position - sectorAt;
        const qint64 n = std::min<qint64>(sectorBytes - inSector, length - done);
        std::memcpy(it->second.data() + inSector, b + done, size_t(n));
        done += n;
    }
}

std::vector<quint32> FatImage::chain(quint32 start) const {
    std::vector<quint32> list;
    quint32 c = start;
    while (c >= 2 && c < clusterCount + 2) {
        if (list.size() > clusterCount) throw Error(QStringLiteral("The card's FAT has a loop."));
        list.push_back(c);
        c = fat[c];
    }
    if (c >= 1 && c < 0xfff8) throw Error(QStringLiteral("The card's FAT is damaged."));
    return list;
}

QByteArray FatImage::folderBytes(const Folder& f) {
    if (f.root()) return readAt(rootStart, qint64(rootEntries) * 32);
    const auto list = chain(f.cluster);
    QByteArray out;
    out.reserve(int(list.size() * clusterBytes()));
    for (const auto c : list) out += readAt(clusterAt(c), clusterBytes());
    return out;
}
void FatImage::writeSlots(const Folder& f, const QByteArray& bytes, int first, int count) {
    const auto list = f.root() ? std::vector<quint32>() : chain(f.cluster);
    for (int slot = first; slot < first + count; ++slot) {
        const qint64 at = qint64(slot) * 32;
        const qint64 position = f.root() ? rootStart + at : clusterAt(list[size_t(at / clusterBytes())]) + at % clusterBytes();
        stage(position, bytes.constData() + at, 32);
    }
}

QList<FatImage::Slot> FatImage::slotsIn(const Folder& f) {
    const auto bytes = folderBytes(f);
    QList<Slot> result;
    QString longParts[20];
    bool have[20] = {};
    int longFirst = -1, longSum = -1, longCount = 0;
    for (int i = 0; i < bytes.size() / 32; ++i) {
        const int at = i * 32;
        const int first = uint8_t(bytes[at]);
        if (first == 0) break;
        const int attr = uint8_t(bytes[at + 11]);
        if (first == 0xe5) { longFirst = -1; continue; }
        if ((attr & 0x3f) == 0x0f) {
            const int order = first & 0x3f;
            if (first & 0x40) {
                std::fill(std::begin(have), std::end(have), false);
                longFirst = i;
                longCount = order;
                longSum = uint8_t(bytes[at + 13]);
            }
            if (longFirst >= 0 && order >= 1 && order <= 20 && uint8_t(bytes[at + 13]) == longSum) {
                longParts[order - 1] = longPart(bytes, at);
                have[order - 1] = true;
            } else longFirst = -1;
            continue;
        }
        const auto shortText = shortDisplay(bytes, at);
        const int sum = checksum(bytes, at);
        bool complete = longFirst >= 0 && sum == longSum && longCount >= 1 && longCount <= 20;
        for (int k = 0; complete && k < longCount; ++k) if (!have[k]) complete = false;
        QString longName;
        if (complete) for (int k = 0; k < longCount; ++k) longName += longParts[k];
        const int start = complete ? longFirst : i;
        longFirst = -1;
        if (attr & 0x08) continue;
        if (shortText == "." || shortText == "..") continue;
        Entry e{complete ? longName : shortText, shortText, bool(attr & 0x10), qint64(u32(bytes, at + 28)), quint32(u16(bytes, at + 26)),
                fromDos(u16(bytes, at + 24), u16(bytes, at + 22))};
        result.append({e, start, i - start + 1});
    }
    return result;
}

// A subfolder starts at a used cluster; anything else would read as the top folder or nothing.
FatImage::Folder FatImage::sub(const Entry& e) const {
    const auto c = e.firstCluster;
    if (c < 2 || c >= clusterCount + 2 || fat[c] == 0) throw Error(QStringLiteral("The folder %1 is damaged.").arg(e.name));
    return Folder{c};
}
FatImage::Folder FatImage::folder(const QString& path) {
    Folder f;
    for (const auto& part : parts(path)) {
        const auto list = slotsIn(f);
        const auto hit = std::find_if(list.begin(), list.end(), [&](const Slot& s) { return sameName(s.entry.name, part); });
        if (hit == list.end()) throw Error(QStringLiteral("There's no folder %1 on the card.").arg(part));
        if (!hit->entry.directory) throw Error(QStringLiteral("%1 is a file.").arg(part));
        f = sub(hit->entry);
    }
    return f;
}

QList<Entry> FatImage::list(const QString& path) {
    QList<Entry> out;
    for (const auto& s : slotsIn(folder(path))) out.append(s.entry);
    std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.directory != b.directory) return a.directory;
        return a.name.toLower() < b.name.toLower();
    });
    return out;
}

std::optional<Entry> FatImage::find(const QString& path) {
    auto p = parts(path);
    if (p.isEmpty()) return std::nullopt;
    const auto last = p.takeLast();
    for (const auto& s : slotsIn(folder('/' + p.join('/'))))
        if (sameName(s.entry.name, last)) return s.entry;
    return std::nullopt;
}

void FatImage::read(const Entry& entry, const std::function<void(const char*, qint64)>& out) {
    if (entry.directory) throw Error(QStringLiteral("%1 is a folder.").arg(entry.name));
    qint64 left = entry.size;
    QByteArray buffer(int(clusterBytes()), '\0');
    for (const auto c : chain(entry.firstCluster)) {
        if (left <= 0) break;
        const qint64 n = std::min(left, clusterBytes());
        blocks.read(clusterAt(c), buffer.data(), n);
        out(buffer.constData(), n);
        left -= n;
    }
    if (left > 0) throw Error(QStringLiteral("%1 is shorter on the card than its size says.").arg(entry.name));
}

void FatImage::add(const QString& path, const QString& name, const std::function<qint64(char*, qint64)>& input, qint64 size) {
    change([&] {
        checkName(name);
        if (size < 0 || size >= (qint64(1) << 32)) throw Error(QStringLiteral("%1 is too big for a card.").arg(name));
        const auto f = folder(path);
        const auto existing = slotsIn(f);
        refuseTaken(existing, name);
        const auto clusters = allocate(int((size + clusterBytes() - 1) / clusterBytes()));
        QByteArray buffer(int(clusterBytes()), '\0');
        qint64 left = size;
        for (const auto c : clusters) {
            const qint64 n = std::min(left, clusterBytes());
            qint64 got = 0;
            while (got < n) {
                const auto r = input(buffer.data() + got, n - got);
                if (r <= 0) throw Error(QStringLiteral("The file ended early."));
                got += r;
            }
            std::memset(buffer.data() + n, 0, size_t(clusterBytes() - n));
            writeData(clusterAt(c), buffer);
            left -= n;
        }
        addEntry(f, existing, name, 0x20, clusters.empty() ? 0 : clusters.front(), size);
    });
}

void FatImage::makeFolder(const QString& path, const QString& name) {
    change([&] {
        checkName(name);
        const auto parent = folder(path);
        const auto existing = slotsIn(parent);
        refuseTaken(existing, name);
        const auto cluster = allocate(1).front();
        const auto [date, time] = frondFolderCard::fatStamp(QDateTime::currentDateTime());
        writeData(clusterAt(cluster), folderBlock(cluster, parent.cluster, date, time));
        addEntry(parent, existing, name, 0x10, cluster, 0);
    });
}

// A new folder's first cluster: the "." and ".." entries, then empty.
QByteArray FatImage::folderBlock(quint32 cluster, quint32 parent, quint16 date, quint16 time) {
    QByteArray block(int(clusterBytes()), '\0');
    auto dot = [&](int at, const char* label, quint32 target) {
        QByteArray name(11, ' ');
        for (int i = 0; label[i]; ++i) name[i] = label[i];
        block.replace(at, 11, name);
        block[at + 11] = 0x10;
        stamp(block, at, date, time);
        put16(block, at + 26, int(target));
    };
    dot(0, ".", cluster);
    dot(32, "..", parent);
    return block;
}

QPair<FatImage::Folder, FatImage::Slot> FatImage::locate(const QString& path) {
    auto p = parts(path);
    if (p.isEmpty()) throw Error(QStringLiteral("That is the top folder."));
    const auto last = p.takeLast();
    const auto parent = folder('/' + p.join('/'));
    for (const auto& s : slotsIn(parent))
        if (sameName(s.entry.name, last)) return {parent, s};
    throw Error(QStringLiteral("There's no %1 on the card.").arg(last));
}

QByteArray FatImage::rawOf(const Folder& parent, const Slot& slot) {
    return folderBytes(parent).mid((slot.first + slot.count - 1) * 32, 32);
}

void FatImage::eraseSlots(const Folder& parent, const Slot& slot) {
    auto bytes = folderBytes(parent);
    for (int i = slot.first; i < slot.first + slot.count; ++i) bytes[i * 32] = char(0xe5);
    writeSlots(parent, bytes, slot.first, slot.count);
}

void FatImage::freeTree(const Entry& entry, int depth) {
    if (depth > maxDepth) throw Error(QStringLiteral("Folders are nested too deeply."));
    if (entry.directory)
        for (const auto& s : slotsIn(sub(entry))) freeTree(s.entry, depth + 1);
    for (const auto c : chain(entry.firstCluster)) fat[c] = 0;
}

void FatImage::remove(const QString& path) {
    change([&] {
        const auto [parent, slot] = locate(path);
        freeTree(slot.entry, 0);
        eraseSlots(parent, slot);
    });
}

void FatImage::move(const QString& from, const QString& toFolder, const QString& newName) {
    change([&] {
        const auto [parent, slot] = locate(from);
        const auto name = newName.isEmpty() ? slot.entry.name : newName;
        checkName(name);
        const auto dest = folder(toFolder);
        if (slot.entry.directory && within(from, toFolder)) throw Error(QStringLiteral("%1 can't go into itself.").arg(slot.entry.name));
        const auto raw = rawOf(parent, slot);
        eraseSlots(parent, slot);
        const auto existing = slotsIn(dest);
        refuseTaken(existing, name);
        addEntry(dest, existing, name, uint8_t(raw[11]), slot.entry.firstCluster, slot.entry.size, raw);
        if (slot.entry.directory && dest.cluster != parent.cluster) {
            const Folder inner{slot.entry.firstCluster};
            auto bytes = folderBytes(inner);
            put16(bytes, 32 + 26, int(dest.cluster));
            writeSlots(inner, bytes, 1, 1);
        }
    });
}

void FatImage::copy(const QString& from, const QString& toFolder, const QString& newName, const std::function<bool()>& cancelled,
                    const std::function<void(qint64)>& bytes) {
    change([&] {
        const auto [parent, slot] = locate(from);
        const auto name = newName.isEmpty() ? slot.entry.name : newName;
        checkName(name);
        const auto dest = folder(toFolder);
        if (slot.entry.directory && within(from, toFolder)) throw Error(QStringLiteral("%1 can't go into itself.").arg(slot.entry.name));
        refuseTaken(slotsIn(dest), name);
        qint64 done = 0;
        copyOne(rawOf(parent, slot), slot.entry, dest, name, 0, cancelled, [&](qint64 n) {
            done += n;
            if (bytes) bytes(done);
        });
    });
}

void FatImage::copyOne(const QByteArray& raw, const Entry& entry, const Folder& dest, const QString& name, int depth,
                       const std::function<bool()>& cancelled, const std::function<void(qint64)>& bytes) {
    if (depth > maxDepth) throw Error(QStringLiteral("Folders are nested too deeply."));
    if (cancelled && cancelled()) throw Cancelled();
    const auto existing = slotsIn(dest);
    if (entry.directory) {
        const auto cluster = allocate(1).front();
        writeData(clusterAt(cluster), folderBlock(cluster, dest.cluster, quint16(u16(raw, 24)), quint16(u16(raw, 22))));
        addEntry(dest, existing, name, 0x10, cluster, 0, raw);
        const Folder inner = sub(entry);
        const auto innerBytes = folderBytes(inner);
        const Folder target{cluster};
        for (const auto& child : slotsIn(inner)) {
            const auto childRaw = innerBytes.mid((child.first + child.count - 1) * 32, 32);
            copyOne(childRaw, child.entry, target, child.entry.name, depth + 1, cancelled, bytes);
        }
    } else {
        const auto source = chain(entry.firstCluster);
        const auto clusters = allocate(int(source.size()));
        QByteArray buffer(int(clusterBytes()), '\0');
        qint64 left = entry.size;
        for (size_t i = 0; i < clusters.size(); ++i) {
            if (cancelled && cancelled()) throw Cancelled();
            const qint64 n = std::max<qint64>(0, std::min(left, clusterBytes()));
            blocks.read(clusterAt(source[i]), buffer.data(), n);
            std::memset(buffer.data() + n, 0, size_t(clusterBytes() - n));
            writeData(clusterAt(clusters[i]), buffer);
            left -= n;
            bytes(n);
        }
        addEntry(dest, existing, name, uint8_t(raw[11]), clusters.empty() ? 0 : clusters.front(), entry.size, raw);
    }
}

qint64 FatImage::sizeOf(const QString& path) {
    const auto located = locate(path);
    std::function<qint64(const Entry&, int)> total = [&](const Entry& e, int depth) -> qint64 {
        if (depth > maxDepth) throw Error(QStringLiteral("Folders are nested too deeply."));
        if (!e.directory) return e.size;
        qint64 sum = 0;
        for (const auto& s : slotsIn(sub(e))) sum += total(s.entry, depth + 1);
        return sum;
    };
    return total(located.second.entry, 0);
}

void FatImage::refuseTaken(const QList<Slot>& existing, const QString& name) {
    for (const auto& s : existing)
        if (sameName(s.entry.name, name) || sameName(s.entry.shortName, name))
            throw Error(QStringLiteral("%1 is already on the card. Choose another name.").arg(name));
}

std::vector<quint32> FatImage::allocate(int count) {
    std::vector<quint32> free;
    for (quint32 c = 2; int(free.size()) < count && c < clusterCount + 2; ++c)
        if (!fat[c]) free.push_back(c);
    if (int(free.size()) < count) throw Error(QStringLiteral("There isn't enough space on the card."));
    for (size_t i = 0; i < free.size(); ++i) fat[free[i]] = i + 1 < free.size() ? free[i + 1] : 0xffff;
    return free;
}

void FatImage::addEntry(const Folder& f, const QList<Slot>& existing, const QString& name, int attr, quint32 cluster, qint64 size,
                        const QByteArray& templ) {
    // Long names count too, so a new ~N alias never equals another file's name.
    QSet<QString> taken;
    for (const auto& s : existing) { taken.insert(s.entry.shortName.toUpper()); taken.insert(s.entry.name.toUpper()); }
    const auto shortBytes = frondFolderCard::shortName(name, taken);
    if (shortBytes.isEmpty()) throw Error(QStringLiteral("Too many similar names in this folder."));
    const bool needsLong = frondFolderCard::shortDisplay(shortBytes) != name;
    const int longCount = needsLong ? int((name.size() + 12) / 13) : 0;
    const int total = longCount + 1;
    auto [bytes, first] = freeSlots(f, total);
    const int sum = frondFolderCard::shortChecksum(shortBytes);
    for (int k = 0; k < longCount; ++k) {
        const int order = longCount - k;
        const int at = (first + k) * 32;
        std::memset(bytes.data() + at, 0, 32);
        bytes[at] = char(order | (k == 0 ? 0x40 : 0));
        bytes[at + 11] = 0x0f;
        bytes[at + 13] = char(sum);
        for (int j = 0; j < 13; ++j) {
            const int index = (order - 1) * 13 + j;
            const int c = index < name.size() ? name[index].unicode() : index == name.size() ? 0 : 0xffff;
            put16(bytes, at + longOffsets[j], c);
        }
    }
    const int at = (first + longCount) * 32;
    std::memset(bytes.data() + at, 0, 32);
    bytes.replace(at, 11, shortBytes);
    bytes[at + 11] = char(attr);
    const auto [date, time] = frondFolderCard::fatStamp(QDateTime::currentDateTime());
    stamp(bytes, at, date, time);
    // A moved or copied entry keeps its dates.
    if (templ.size() == 32) {
        for (int i = 13; i <= 19; ++i) bytes[at + i] = templ[i];
        for (int i = 22; i <= 25; ++i) bytes[at + i] = templ[i];
    }
    put16(bytes, at + 26, int(cluster));
    put32(bytes, at + 28, quint32(size));
    writeSlots(f, bytes, first, total);
}

// Finds count free slots in a row, adding a cluster to a full subfolder. Returns the folder's bytes
// (grown if needed) and the first slot.
QPair<QByteArray, int> FatImage::freeSlots(const Folder& f, int count) {
    auto bytes = folderBytes(f);
    auto search = [&]() {
        int run = 0;
        for (int i = 0; i < bytes.size() / 32; ++i) {
            const int first = uint8_t(bytes[i * 32]);
            if (first == 0 || first == 0xe5) {
                if (++run == count) return i - count + 1;
            } else run = 0;
        }
        return -1;
    };
    int found = search();
    while (found < 0) {
        if (f.root()) throw Error(QStringLiteral("The card's top folder is full. Use a subfolder."));
        const auto list = chain(f.cluster);
        if (list.empty()) throw Error(QStringLiteral("The card's FAT is damaged."));
        // A FAT folder holds at most 65536 entries (2 MB).
        if (qint64(list.size() + 1) * clusterBytes() > 65536 * 32)
            throw Error(QStringLiteral("That folder is full (a card folder holds at most 65536 names). Use another folder."));
        const auto added = allocate(1).front();
        fat[list.back()] = added;
        writeData(clusterAt(added), QByteArray(int(clusterBytes()), '\0'));
        bytes = folderBytes(f);
        found = search();
    }
    return {bytes, found};
}

// Runs one change and commits it, or rolls the in-memory state back when it fails. Data already
// written went into clusters the card's FAT still calls free.
void FatImage::change(const std::function<void()>& block) {
    try {
        block();
        commit();
    } catch (...) {
        fat = fatOnDisk;
        staged.clear();
        try { blocks.settleData(); } catch (...) {}
        throw;
    }
}

// Stages the FAT sectors that differ from the card, in every FAT copy.
void FatImage::stageFat() {
    std::set<qint64> sectors;
    for (size_t i = 0; i < fat.size(); ++i)
        if (fat[i] != fatOnDisk[i]) sectors.insert(qint64(i) * 2 / sectorBytes);
    for (const auto s : sectors) {
        const size_t from = size_t(s * sectorBytes / 2);
        const size_t until = std::min(from + sectorBytes / 2, fat.size());
        QByteArray raw(int((until - from) * 2), '\0');
        for (size_t i = from; i < until; ++i) put16(raw, qint64(i - from) * 2, int(fat[i]));
        for (int copy = 0; copy < fatCount; ++copy)
            stage(fatStart + qint64(copy) * fatSectors * sectorBytes + s * sectorBytes, raw.constData(), raw.size());
    }
}

void FatImage::commit() {
    stageFat();
    if (staged.empty()) return;
    blocks.commit(staged);
    fatOnDisk = fat;
    staged.clear();
    committed = true;
}

bool FatImage::fatCopiesMatch() {
    const auto first = readAt(fatStart, qint64(fatSectors) * sectorBytes);
    for (int i = 1; i < fatCount; ++i)
        if (readAt(fatStart + qint64(i) * fatSectors * sectorBytes, qint64(fatSectors) * sectorBytes) != first) return false;
    return true;
}

QStringList FatImage::check() {
    QStringList problems;
    if (!fatCopiesMatch()) problems << QStringLiteral("FAT copies differ");
    std::vector<QString> owner(clusterCount + 2);
    std::function<void(const Folder&, const QString&, int)> walk = [&](const Folder& f, const QString& path, int depth) {
        if (depth > maxDepth) { problems << path + " nested too deeply"; return; }
        for (const auto& slot : slotsIn(f)) {
            const auto& e = slot.entry;
            const auto full = joined(path, e.name);
            std::vector<quint32> list;
            try { list = chain(e.firstCluster); } catch (const Error& x) { problems << full + ": " + x.message; continue; }
            for (const auto c : list) {
                if (!owner[c].isEmpty()) problems << QStringLiteral("%1 shares cluster %2 with %3").arg(full).arg(c).arg(owner[c]);
                owner[c] = full;
            }
            if (e.directory) {
                if (list.empty()) problems << full + " has no cluster";
                else walk(Folder{e.firstCluster}, full, depth + 1);
            } else if (qint64(list.size()) != (e.size + clusterBytes() - 1) / clusterBytes())
                problems << QStringLiteral("%1 size %2 doesn't fit %3 clusters").arg(full).arg(e.size).arg(list.size());
        }
    };
    walk(Folder{}, QStringLiteral("/"), 0);
    for (quint32 c = 2; c < clusterCount + 2; ++c)
        if (fat[c] && owner[c].isEmpty()) problems << QStringLiteral("cluster %1 is lost").arg(c);
    return problems;
}

QStringList FatImage::parts(const QString& path) { return path.split('/', Qt::SkipEmptyParts); }
QString FatImage::joined(const QString& folder, const QString& name) {
    return folder == QStringLiteral("/") || folder.isEmpty() ? '/' + name : folder + '/' + name;
}
bool FatImage::within(const QString& outer, const QString& inner) {
    const auto o = parts(outer.toLower()), i = parts(inner.toLower());
    return i.size() >= o.size() && i.mid(0, o.size()) == o;
}
void FatImage::checkName(const QString& name) {
    static const QString forbidden = QStringLiteral("\"*/:<>?\\|");
    bool bad = name.isEmpty() || name.size() > 255 || name == "." || name == ".." || name.endsWith('.') || name.endsWith(' ') ||
               name.startsWith(' ');
    for (const QChar ch : name) if (ch.unicode() < 0x20 || ch.unicode() == 0x7f || forbidden.contains(ch)) bad = true;
    if (bad) throw Error(QStringLiteral("FAT can't use the name \"%1\".").arg(name));
}

}  // namespace frondCardFile
