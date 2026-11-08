#pragma once
// A Frond card file edited in place for one Files… visit, the same as Android's CardFile.kt: the
// same header handling, the same <card>.files-intent and <card>.files-commit files, so either
// frontend settles what the other left. Header only, so CardStore.h can settle cards.
//
// sd.frondsd for SD, ms.frondmsp for a Memory Stick Pro (uarm/FrondSdImage.h). A 64-byte header,
// the card's bytes from file offset 64, then for v2 one CRC-32 per 1 MiB chunk. v2 header, little
// endian: 8 version, 12 sector size, 16 card bytes, 24 header CRC (the header with this field
// zeroed, then the table), 28 generation, 36 table offset (64 + card bytes), 44 chunk bytes. v1 has
// the CRC of all the card's bytes at 24 and no table; its first change makes it v2.
//
// Crash safety, per change. 1: before data goes into a chunk, the chunk's number is added to the
// intent file and fsynced; the data only goes into clusters the card's FAT calls free. 2: the data
// is fsynced, then the FAT and folder sectors, the changed table entries and the new header go into
// the commit file (a temp, fsynced, renamed). 3: they're written in place and fsynced, then both
// files go. settle() finishes a cut-off change: a commit file is replayed (the change is done); an
// intent file alone gets its chunks' CRCs and the header rewritten (the change isn't done, only
// free clusters changed). Either way the card loads, at the visit's generation.
//
// One CardFile is one visit: every change it writes, and a settle after a crash, moves the
// generation to the one at open plus 1. The card file holds an open file description lock (fcntl
// F_OFD_SETLK, which also conflicts with Android's FileChannel lock) while open. Not thread safe.

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QPair>
#include <QString>
#include <QtEndian>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <zlib.h>

namespace frondCardFile {

// A failed card change or read. `what()` is for logs; `message` is shown.
class Error : public std::exception {
 public:
    explicit Error(QString text) : message(std::move(text)), utf8(message.toUtf8()) {}
    const char* what() const noexcept override { return utf8.constData(); }
    QString message;
 private:
    QByteArray utf8;
};
// A change stopped by its caller; what was done stays, the rest is rolled back.
class Cancelled : public Error {
 public:
    Cancelled() : Error(QStringLiteral("Cancelled.")) {}
};

constexpr int headerBytes = 64;
constexpr int sectorBytes = 512;
constexpr qint64 chunkBytes = 1 << 20;
constexpr qint64 maxPayload = qint64(2048) << 20;
inline QString inUseMessage() { return QStringLiteral("The card is open somewhere else. Close it there first."); }
inline QString damagedMessage() { return QStringLiteral("The card file is damaged."); }
inline const QByteArray& magic() { static const QByteArray m("FRONDSD\0", 8); return m; }
inline const QByteArray& intentMagic() { static const QByteArray m("FRONDFI1", 8); return m; }
inline const QByteArray& commitMagic() { static const QByteArray m("FRONDFC1", 8); return m; }
inline const QByteArray& commitEnd() { static const QByteArray m("FCEND\0\0\0", 8); return m; }
constexpr int intentHeader = 24;
constexpr qint64 commitMaxBytes = qint64(64) << 20;

// The files a cut-off change leaves beside the card. Not core journal names (those are
// <card>.journal-N), so neither the core nor the frontends' journal checks take them for a save.
inline QString intentOf(const QString& card) { return card + QStringLiteral(".files-intent"); }
inline QString commitOf(const QString& card) { return card + QStringLiteral(".files-commit"); }

inline qint64 chunks(qint64 payload) { return (payload + chunkBytes - 1) / chunkBytes; }
inline int chunkOf(qint64 at) { return int(at / chunkBytes); }

// Where a FatImage's bytes live. Offsets are card bytes from 0, whole sectors for metadata.
class Blocks {
 public:
    virtual ~Blocks() = default;
    virtual qint64 length() const = 0;
    virtual void read(qint64 at, char* b, qint64 n) = 0;
    // New data, only into clusters the FAT on the card calls free.
    virtual void write(qint64 at, const char* b, qint64 n) = 0;
    virtual void sync() = 0;
    // Commits one change's metadata, whole sectors by card offset.
    virtual void commit(const std::map<qint64, QByteArray>& sectors) = 0;
    // A change failed after writing data: the card is made sound again now.
    virtual void settleData() {}
};

namespace detail {
inline bool preadAll(int fd, char* b, qint64 n, qint64 at) {
    while (n > 0) {
        const auto r = ::pread(fd, b, size_t(n), off_t(at));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        b += r; n -= r; at += r;
    }
    return true;
}
inline bool pwriteAll(int fd, const char* b, qint64 n, qint64 at) {
    while (n > 0) {
        const auto r = ::pwrite(fd, b, size_t(n), off_t(at));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        b += r; n -= r; at += r;
    }
    return true;
}
inline bool writeAll(int fd, const QByteArray& b) {
    const char* p = b.constData();
    qint64 n = b.size();
    while (n > 0) {
        const auto r = ::write(fd, p, size_t(n));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}
inline qint64 sizeOf(int fd) {
    struct stat st {};
    return ::fstat(fd, &st) == 0 ? qint64(st.st_size) : -1;
}
inline void syncDirOf(const QString& file) {
    const int fd = ::open(QFile::encodeName(QFileInfo(file).absolutePath()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}
inline bool tryLock(int fd) {
    struct flock l {};
    l.l_type = F_WRLCK;
    l.l_whence = SEEK_SET;
    return ::fcntl(fd, F_OFD_SETLK, &l) == 0;
}
inline quint32 crc(const char* b, qint64 n, quint32 start = 0) {
    uLong c = start;
    while (n > 0) {
        const auto part = uInt(std::min<qint64>(n, 1 << 30));
        c = ::crc32(c, reinterpret_cast<const Bytef*>(b), part);
        b += part; n -= part;
    }
    return quint32(c);
}
inline quint32 le32(const QByteArray& b, int at) { return qFromLittleEndian<quint32>(b.constData() + at); }
inline quint64 le64(const QByteArray& b, int at) { return qFromLittleEndian<quint64>(b.constData() + at); }
inline void put32(QByteArray& b, int at, quint32 v) { qToLittleEndian(v, b.data() + at); }
inline void put64(QByteArray& b, int at, quint64 v) { qToLittleEndian(v, b.data() + at); }
inline bool zeros(const QByteArray& b, int from, int until) {
    for (int i = from; i < until; ++i) if (b[i]) return false;
    return true;
}

inline QByteArray tableBytes(const std::vector<quint32>& table, size_t from, size_t until) {
    QByteArray b(int(4 * (until - from)), '\0');
    for (size_t i = from; i < until; ++i) put32(b, int(4 * (i - from)), table[i]);
    return b;
}
inline quint32 headerCrc(const QByteArray& header, const std::vector<quint32>& table) {
    auto copy = header.left(headerBytes);
    for (int i = 24; i < 28; ++i) copy[i] = 0;
    const auto t = tableBytes(table, 0, table.size());
    return crc(t.constData(), t.size(), crc(copy.constData(), copy.size()));
}
inline QByteArray headerV2(qint64 payload, quint64 generation, const std::vector<quint32>& table) {
    QByteArray head(headerBytes, '\0');
    head.replace(0, 8, magic());
    put32(head, 8, 2); put32(head, 12, sectorBytes); put64(head, 16, quint64(payload)); put64(head, 28, generation);
    put64(head, 36, quint64(headerBytes + payload)); put32(head, 44, quint32(chunkBytes));
    put32(head, 24, headerCrc(head, table));
    return head;
}
inline bool readTable(int fd, qint64 payload, std::vector<quint32>& table) {
    const auto count = chunks(payload);
    QByteArray raw(int(4 * count), '\0');
    if (!preadAll(fd, raw.data(), raw.size(), headerBytes + payload)) return false;
    table.assign(size_t(count), 0);
    for (qint64 i = 0; i < count; ++i) table[size_t(i)] = le32(raw, int(4 * i));
    return true;
}
// CRC-32 of chunk i as it will be, with `over` (card offset to whole sector) laid on top.
inline bool chunkCrc(int fd, qint64 payload, qint64 i, const std::map<qint64, QByteArray>& over, quint32& out) {
    const qint64 start = i * chunkBytes;
    const qint64 n = std::min(chunkBytes, payload - start);
    QByteArray buf(int(n), '\0');
    if (!preadAll(fd, buf.data(), n, headerBytes + start)) return false;
    for (auto it = over.lower_bound(start - sectorBytes + 1); it != over.end() && it->first < start + n; ++it) {
        const qint64 from = std::max(it->first, start);
        const qint64 until = std::min(it->first + sectorBytes, start + n);
        std::memcpy(buf.data() + (from - start), it->second.constData() + (from - it->first), size_t(until - from));
    }
    out = crc(buf.constData(), n);
    return true;
}

// Header: magic, record count (u32), file length after (u64). Records: file offset (u64), length
// (u32), bytes. Trailer: end magic and the CRC-32 of everything before it. Big endian.
inline QByteArray encodeCommit(const std::vector<std::pair<qint64, QByteArray>>& records, qint64 finalLength) {
    QByteArray out = commitMagic();
    char word[8];
    qToBigEndian(quint32(records.size()), word); out.append(word, 4);
    qToBigEndian(quint64(finalLength), word); out.append(word, 8);
    for (const auto& [at, bytes] : records) {
        qToBigEndian(quint64(at), word); out.append(word, 8);
        qToBigEndian(quint32(bytes.size()), word); out.append(word, 4);
        out += bytes;
    }
    out += commitEnd();
    qToBigEndian(crc(out.constData(), out.size()), word);
    out.append(word, 4);
    return out;
}
inline bool decodeCommit(const QByteArray& b, qint64& finalLength, std::vector<std::pair<qint64, QByteArray>>& records) {
    if (b.size() < 20 + 12 || b.left(8) != commitMagic()) return false;
    const int end = b.size() - 12;
    if (b.mid(end, 8) != commitEnd()) return false;
    if (crc(b.constData(), end + 8) != qFromBigEndian<quint32>(b.constData() + end + 8)) return false;
    const auto count = qint32(qFromBigEndian<quint32>(b.constData() + 8));
    finalLength = qint64(qFromBigEndian<quint64>(b.constData() + 12));
    qint64 at = 20;
    records.clear();
    for (qint32 i = 0; i < count; ++i) {
        if (at + 12 > end) return false;
        const auto offset = qint64(qFromBigEndian<quint64>(b.constData() + at));
        const auto length = qint32(qFromBigEndian<quint32>(b.constData() + at + 8));
        if (offset < 0 || length < 0 || at + 12 + length > end || offset + length > finalLength) return false;
        records.emplace_back(offset, b.mid(int(at + 12), length));
        at += 12 + length;
    }
    return at == end;
}

// Rewrites the CRCs of the chunks named (all of them for a card that isn't v2 yet) and the header
// at `target`.
inline bool refresh(int fd, qint64 payload, quint64 target, const std::vector<qint32>& listed) {
    QByteArray head(headerBytes, '\0');
    if (!preadAll(fd, head.data(), headerBytes, 0)) return false;
    const auto count = chunks(payload);
    const bool v2 = head.left(8) == magic() && le32(head, 8) == 2 && qint64(le64(head, 16)) == payload &&
                    sizeOf(fd) == headerBytes + payload + 4 * count;
    std::vector<quint32> table(size_t(count), 0);
    if (v2 && !readTable(fd, payload, table)) return false;
    std::set<qint64> which;
    if (v2) { for (auto i : listed) if (i >= 0 && i < count) which.insert(i); }
    else for (qint64 i = 0; i < count; ++i) which.insert(i);
    for (auto i : which)
        if (!chunkCrc(fd, payload, i, {}, table[size_t(i)])) return false;
    if (::ftruncate(fd, off_t(headerBytes + payload + 4 * count)) != 0) return false;
    const auto t = tableBytes(table, 0, table.size());
    if (!pwriteAll(fd, t.constData(), t.size(), headerBytes + payload) || ::fsync(fd) != 0) return false;
    const auto h = headerV2(payload, std::max(target, v2 ? le64(head, 28) : quint64(0)), table);
    return pwriteAll(fd, h.constData(), h.size(), 0) && ::fsync(fd) == 0;
}

inline QByteArray readWhole(const QString& path, qint64 limit) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > limit) return {};
    return f.readAll();
}

// Settling is safe to repeat: replaying a commit writes the same bytes, and the intent step only
// recomputes CRCs from what is on the card.
inline bool settleLocked(const QString& card, int fd) {
    const auto commit = commitOf(card);
    QFile::remove(commit + QStringLiteral(".tmp"));
    if (QFileInfo::exists(commit)) {
        const auto raw = readWhole(commit, commitMaxBytes);
        qint64 finalLength = 0;
        std::vector<std::pair<qint64, QByteArray>> records;
        if (!raw.isEmpty() && decodeCommit(raw, finalLength, records)) {
            if (sizeOf(fd) < finalLength && ::ftruncate(fd, off_t(finalLength)) != 0) return false;
            for (const auto& [at, bytes] : records)
                if (!pwriteAll(fd, bytes.constData(), bytes.size(), at)) return false;
            if (::fsync(fd) != 0) return false;
        }
        QFile::remove(commit);
        syncDirOf(card);
    }
    const auto intent = intentOf(card);
    if (QFileInfo::exists(intent)) {
        const auto b = readWhole(intent, qint64(64) << 20);
        // A short header means nothing was written yet: it is fsynced before any data.
        if (b.size() >= intentHeader && b.left(8) == intentMagic()) {
            const auto payload = qint64(le64(b, 8));
            const auto target = le64(b, 16);
            std::vector<qint32> listed;
            for (int i = 0; i < (b.size() - intentHeader) / 4; ++i) listed.push_back(qint32(le32(b, intentHeader + 4 * i)));
            if (payload >= sectorBytes && payload <= maxPayload && payload % sectorBytes == 0 && sizeOf(fd) >= headerBytes + payload &&
                !refresh(fd, payload, target, listed))
                return false;
        }
        QFile::remove(intent);
        syncDirOf(card);
    }
    return true;
}
}  // namespace detail

// Whether a cut-off change is waiting beside the card file.
inline bool unsettled(const QString& card) {
    return QFileInfo::exists(intentOf(card)) || QFileInfo::exists(commitOf(card));
}

// Whether a card file is open in a CardFile now (this process or another).
inline bool inUse(const QString& card) {
    if (!QFileInfo(card).isFile()) return false;
    const int fd = ::open(QFile::encodeName(card).constData(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    const bool free = detail::tryLock(fd);
    ::close(fd);
    return !free;
}

// Finishes a cut-off change, if there is one. False when the card is open in a CardFile now, or
// can't be written; true when there was nothing to do or it's done.
inline bool settle(const QString& card) {
    if (!QFileInfo(card).isFile() || !unsettled(card)) return true;
    const int fd = ::open(QFile::encodeName(card).constData(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = detail::tryLock(fd) && detail::settleLocked(card, fd);
    ::close(fd);
    return ok;
}

// A card file nobody has open in Files…, with any change a crash cut off finished.
inline bool ready(const QString& card) { return !inUse(card) && settle(card); }
inline QString inFilesMessage() { return QStringLiteral("The card is open in Files. Close it first."); }

// What the core's load checks (uarm/MachineFolder.h): a sound v2 header and every chunk's CRC, or
// a v1 file's whole CRC. Reads the whole card. The generation, or nothing.
inline std::optional<quint64> verify(const QString& card) {
    using namespace detail;
    const int fd = ::open(QFile::encodeName(card).constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    struct Closer { int fd; ~Closer() { ::close(fd); } } closer{fd};
    QByteArray head(headerBytes, '\0');
    const auto length = sizeOf(fd);
    if (length < headerBytes || !preadAll(fd, head.data(), headerBytes, 0)) return std::nullopt;
    if (head.left(8) != magic() || le32(head, 12) != sectorBytes) return std::nullopt;
    const auto payload = qint64(le64(head, 16));
    if (payload < sectorBytes || payload % sectorBytes || payload > maxPayload) return std::nullopt;
    if (le32(head, 8) == 1) {
        if (length != headerBytes + payload) return std::nullopt;
        quint32 c = 0;
        QByteArray buf(int(chunkBytes), '\0');
        for (qint64 done = 0; done < payload;) {
            const auto n = std::min(chunkBytes, payload - done);
            if (!preadAll(fd, buf.data(), n, headerBytes + done)) return std::nullopt;
            c = crc(buf.constData(), n, c);
            done += n;
        }
        return c == le32(head, 24) ? std::optional<quint64>(0) : std::nullopt;
    }
    if (le32(head, 8) != 2) return std::nullopt;
    if (qint64(le64(head, 36)) != headerBytes + payload || le32(head, 44) != chunkBytes || !zeros(head, 48, headerBytes) ||
        length != headerBytes + payload + 4 * chunks(payload))
        return std::nullopt;
    std::vector<quint32> table;
    if (!readTable(fd, payload, table) || headerCrc(head, table) != le32(head, 24)) return std::nullopt;
    for (size_t i = 0; i < table.size(); ++i) {
        quint32 c = 0;
        if (!chunkCrc(fd, payload, qint64(i), {}, c) || c != table[i]) return std::nullopt;
    }
    return le64(head, 28);
}

// A v2 card file of `payload` (whole sectors) at `generation`, as the card tool makes one.
inline QByteArray encodeV2(const QByteArray& payload, quint64 generation) {
    std::vector<quint32> table(size_t(chunks(payload.size())));
    for (size_t i = 0; i < table.size(); ++i) {
        const qint64 start = qint64(i) * chunkBytes;
        table[i] = detail::crc(payload.constData() + start, std::min<qint64>(chunkBytes, payload.size() - start));
    }
    return detail::headerV2(payload.size(), generation, table) + payload + detail::tableBytes(table, 0, table.size());
}

class CardFile : public Blocks {
 public:
    // Opens a card file for a visit, settling a cut-off change first. Throws Error(inUseMessage())
    // when another CardFile (this process or another) has it.
    static std::unique_ptr<CardFile> open(const QString& card, bool writable) {
        const int fd = ::open(QFile::encodeName(card).constData(), O_RDWR | O_CLOEXEC);
        if (fd < 0) throw Error(QStringLiteral("Couldn't open the card file."));
        if (!detail::tryLock(fd)) { ::close(fd); throw Error(inUseMessage()); }
        std::unique_ptr<CardFile> c(new CardFile(card, fd, writable));
        if (!detail::settleLocked(card, fd)) throw Error(QStringLiteral("Couldn't finish the last change to the card."));
        c->load();
        return c;
    }
    ~CardFile() override {
        try { if (!dead) settleData(); } catch (...) {}
        if (intent >= 0) ::close(intent);
        ::close(fd);  // lets go of the lock
    }
    CardFile(const CardFile&) = delete;
    CardFile& operator=(const CardFile&) = delete;

    const QString path;
    const bool writable;
    qint64 payloadBytes() const { return payload; }
    quint64 generation() const { return gen; }

    // Called once, before the first byte this visit writes, e.g. to mark the machine's slot swapped.
    // It may throw Error to stop the change.
    std::function<void()> beforeFirstWrite;
    // Bytes written into the card file, the write calls (file offset, length), and journal bytes.
    qint64 cardBytesWritten{0}, journalBytesWritten{0};
    QList<QPair<qint64, qint64>> writes;
    // Tests: called at "writing" (after the first data of a change), "data", "journal", "partly",
    // "applied". After a simulated crash (`dead`) nothing more reaches the disk.
    std::function<void(const char*)> crashHook;
    bool dead{false};

    qint64 length() const override { return payload; }
    void read(qint64 at, char* b, qint64 n) override {
        if (at < 0 || at + n > payload) throw Error(QStringLiteral("The card is shorter than its file system says."));
        if (!detail::preadAll(fd, b, n, headerBytes + at)) throw Error(QStringLiteral("Couldn't read the card."));
    }
    void write(qint64 at, const char* b, qint64 n) override {
        if (!writable) throw Error(QStringLiteral("The card is write protected."));
        if (at < 0 || at + n > payload) throw Error(QStringLiteral("The card is shorter than its file system says."));
        if (dead) return;
        start();
        std::vector<qint32> fresh;
        for (int i = chunkOf(at); i <= chunkOf(at + n - 1); ++i)
            if (!dirty.count(i)) fresh.push_back(i);
        const bool first = dirty.empty();
        if (!fresh.empty()) {
            note(fresh);
            dirty.insert(fresh.begin(), fresh.end());
        }
        put(headerBytes + at, b, n);
        if (first && !fresh.empty()) hook("writing");
    }
    void sync() override {
        if (!dead && ::fsync(fd) != 0) throw Error(QStringLiteral("Couldn't write the card."));
    }

    // Commits one change: `sectors` are whole card sectors by card offset (FAT and folders). With no
    // sectors, fixes the CRCs of chunks that got data without a commit (a failed change).
    void commit(const std::map<qint64, QByteArray>& sectors) override {
        using namespace detail;
        if (!writable) throw Error(QStringLiteral("The card is write protected."));
        if (dead) return;
        if (sectors.empty() && dirty.empty()) return;
        start();
        if (::fsync(fd) != 0) throw Error(QStringLiteral("Couldn't write the card."));
        hook("data");
        if (dead) return;
        std::set<int> touched(dirty.begin(), dirty.end());
        for (const auto& s : sectors) { touched.insert(chunkOf(s.first)); touched.insert(chunkOf(s.first + sectorBytes - 1)); }
        const auto count = size_t(chunks(payload));
        std::vector<quint32> next = table.empty() ? std::vector<quint32>(count, 0) : table;
        auto recompute = [&](size_t i) {
            if (!chunkCrc(fd, payload, qint64(i), sectors, next[i])) throw Error(QStringLiteral("Couldn't read the card."));
        };
        if (table.empty()) for (size_t i = 0; i < count; ++i) recompute(i);
        else for (auto i : touched) recompute(size_t(i));
        std::vector<std::pair<qint64, QByteArray>> records;
        for (const auto& [at, sector] : sectors) records.emplace_back(headerBytes + at, sector);
        const qint64 tableAt = headerBytes + payload;
        if (table.empty()) records.emplace_back(tableAt, tableBytes(next, 0, count));
        else {
            // Runs of changed entries.
            for (size_t i = 0; i < count;) {
                if (next[i] == table[i]) { ++i; continue; }
                size_t j = i;
                while (j < count && next[j] != table[j]) ++j;
                records.emplace_back(tableAt + 4 * qint64(i), tableBytes(next, i, j));
                i = j;
            }
        }
        records.emplace_back(0, headerV2(payload, target, next));
        const qint64 finalLength = tableAt + 4 * qint64(count);
        const auto encoded = encodeCommit(records, finalLength);
        const auto journal = commitOf(path);
        const auto temp = journal + QStringLiteral(".tmp");
        {
            const int out = ::open(QFile::encodeName(temp).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            const bool ok = out >= 0 && writeAll(out, encoded) && ::fsync(out) == 0;
            if (out >= 0) ::close(out);
            if (!ok || ::rename(QFile::encodeName(temp).constData(), QFile::encodeName(journal).constData()) != 0) {
                QFile::remove(temp);
                throw Error(QStringLiteral("Couldn't write the card's journal."));
            }
        }
        syncDirOf(path);
        journalBytesWritten += encoded.size();
        hook("journal");
        if (dead) return;
        if (sizeOf(fd) < finalLength && ::ftruncate(fd, off_t(finalLength)) != 0) throw Error(QStringLiteral("Couldn't write the card."));
        for (size_t k = 0; k < records.size(); ++k) {
            if (k == records.size() / 2) { hook("partly"); if (dead) return; }
            put(records[k].first, records[k].second.constData(), records[k].second.size());
        }
        if (::fsync(fd) != 0) throw Error(QStringLiteral("Couldn't write the card."));
        hook("applied");
        if (dead) return;
        QFile::remove(journal);
        if (intent >= 0) { ::close(intent); intent = -1; }
        QFile::remove(intentOf(path));
        syncDirOf(path);
        table = next;
        version = 2;
        gen = target;
        dirty.clear();
    }

    // A change that failed after writing data: the chunk CRCs are fixed now so the card loads.
    void settleData() override {
        if (!dirty.empty() && !dead) commit({});
    }

 private:
    CardFile(const QString& card, int fd, bool writable) : path(card), writable(writable), fd(fd) {}

    void load() {
        using namespace detail;
        QByteArray head(headerBytes, '\0');
        const auto size = sizeOf(fd);
        if (size < headerBytes || !preadAll(fd, head.data(), headerBytes, 0)) throw Error(damagedMessage());
        if (head.left(8) != magic() || le32(head, 12) != sectorBytes) throw Error(damagedMessage());
        version = int(le32(head, 8));
        payload = qint64(le64(head, 16));
        if (payload < sectorBytes || payload % sectorBytes || payload > maxPayload) throw Error(damagedMessage());
        if (version == 1) {
            if (!zeros(head, 28, headerBytes) || size != headerBytes + payload) throw Error(damagedMessage());
            gen = 0;
            table.clear();
        } else if (version == 2) {
            gen = le64(head, 28);
            if (qint64(le64(head, 36)) != headerBytes + payload || le32(head, 44) != chunkBytes || !zeros(head, 48, headerBytes) ||
                size != headerBytes + payload + 4 * chunks(payload))
                throw Error(damagedMessage());
            std::vector<quint32> t;
            if (!readTable(fd, payload, t) || headerCrc(head, t) != le32(head, 24)) throw Error(damagedMessage());
            table = t;
        } else throw Error(damagedMessage());
        target = gen + 1;
    }
    void hook(const char* step) { if (crashHook) crashHook(step); }
    void start() {
        if (started) return;
        if (beforeFirstWrite) beforeFirstWrite();
        started = true;
    }
    // Adds chunk numbers to the intent file, making it first with the card's size and the visit's
    // generation. Fsynced before the data is written.
    void note(const std::vector<qint32>& list) {
        using namespace detail;
        QByteArray bytes(int(4 * list.size()), '\0');
        for (size_t i = 0; i < list.size(); ++i) put32(bytes, int(4 * i), quint32(list[i]));
        if (intent < 0) {
            const auto name = intentOf(path);
            intent = ::open(QFile::encodeName(name).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (intent < 0) throw Error(QStringLiteral("Couldn't write the card's journal."));
            QByteArray head(intentHeader, '\0');
            head.replace(0, 8, intentMagic());
            put64(head, 8, quint64(payload));
            put64(head, 16, target);
            if (!writeAll(intent, head) || ::fsync(intent) != 0) throw Error(QStringLiteral("Couldn't write the card's journal."));
            syncDirOf(path);
            journalBytesWritten += intentHeader;
        }
        if (!writeAll(intent, bytes) || ::fsync(intent) != 0) throw Error(QStringLiteral("Couldn't write the card's journal."));
        journalBytesWritten += bytes.size();
    }
    void put(qint64 at, const char* b, qint64 n) {
        if (!detail::pwriteAll(fd, b, n, at)) throw Error(QStringLiteral("Couldn't write the card."));
        cardBytesWritten += n;
        writes.append({at, n});
    }

    int fd{-1};
    int intent{-1};
    int version{0};
    qint64 payload{0};
    quint64 gen{0}, target{0};
    // Chunk CRCs as on the card; empty for a v1 card until its first change.
    std::vector<quint32> table;
    // Chunks that got data since the last commit, all named in the intent file.
    std::set<int> dirty;
    bool started{false};
};

// A raw card image, for the SD exchange's private copies: writes go straight in, and a commit
// writes its sectors and syncs. The caller keeps the image's earlier copy until a change is checked.
class RawBlocks : public Blocks {
 public:
    RawBlocks(const QString& image, bool writable) : writable(writable) {
        fd = ::open(QFile::encodeName(image).constData(), (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
        if (fd < 0) throw Error(QStringLiteral("Couldn't open the card image."));
        size = detail::sizeOf(fd);
    }
    ~RawBlocks() override { ::close(fd); }
    qint64 length() const override { return size; }
    void read(qint64 at, char* b, qint64 n) override {
        if (at < 0 || at + n > size || !detail::preadAll(fd, b, n, at)) throw Error(QStringLiteral("The card is shorter than its file system says."));
    }
    void write(qint64 at, const char* b, qint64 n) override {
        if (!writable) throw Error(QStringLiteral("The card is write protected."));
        if (at < 0 || at + n > size || !detail::pwriteAll(fd, b, n, at)) throw Error(QStringLiteral("Couldn't write the card image."));
    }
    void sync() override { ::fsync(fd); }
    void commit(const std::map<qint64, QByteArray>& sectors) override {
        for (const auto& [at, sector] : sectors) write(at, sector.constData(), sector.size());
        sync();
    }
 private:
    int fd{-1};
    qint64 size{0};
    bool writable;
};

}  // namespace frondCardFile
