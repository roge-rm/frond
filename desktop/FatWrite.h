#pragma once
// FAT16 directory entry pieces shared by the folder import (FolderCard.h) and the in-place writer
// of the Files exchange (SdExchange.cpp). Header only.
#include <QByteArray>
#include <QDateTime>
#include <QPair>
#include <QSet>
#include <QString>
#include <algorithm>
#include <cstdint>

namespace frondFolderCard {

// The same entries Android's FatImage writes: a Windows-style 8.3 name (upper case, anything
// past ASCII as _, a ~N tail when something was lost or taken) and long-name entries whenever
// that short name isn't the name exactly. No case bits and no code page bytes in short names,
// so every reader gets the long name as it was. mtools can't be told to do this: it keeps a
// lower-case or accented name short, in code page 850 with case bits.

inline QByteArray shortName(const QString& name, const QSet<QString>& taken) {
    const auto upper = name.toUpper();
    const auto dotAt = upper.lastIndexOf('.');
    const int dot = dotAt > 0 ? int(dotAt) : -1;
    bool lossy = false;
    const auto clean = [&](const QString& part) {
        QString out;
        for (const QChar ch : part) {
            if (ch == ' ' || ch == '.') lossy = true;
            else if (ch.unicode() > 0x7e || QStringLiteral("+,;=[]").contains(ch)) { out += '_'; lossy = true; }
            else out += ch;
        }
        return out;
    };
    auto base = clean(dot >= 0 ? upper.left(dot) : upper);
    auto ext = dot >= 0 ? clean(upper.mid(dot + 1)) : QString();
    if (base.size() > 8) lossy = true;
    if (ext.size() > 3) { ext = ext.left(3); lossy = true; }
    if (base.isEmpty()) { base = QStringLiteral("_"); lossy = true; }
    const auto key = [](const QString& b, const QString& e) { return e.isEmpty() ? b : b + '.' + e; };
    const auto pack = [](const QString& b, const QString& e) {
        QByteArray out(11, ' ');
        for (int i = 0; i < 8 && i < b.size(); ++i) out[i] = char(b[i].unicode());
        for (int i = 0; i < 3 && i < e.size(); ++i) out[8 + i] = char(e[i].unicode());
        return out;
    };
    if (!lossy && base.size() <= 8 && !taken.contains(key(base, ext))) return pack(base, ext);
    for (int n = 1; n <= 999999; ++n) {
        const auto tail = QStringLiteral("~%1").arg(n);
        const auto b = base.left(8 - tail.size()) + tail;
        if (!taken.contains(key(b, ext))) return pack(b, ext);
    }
    return {};
}
inline QString shortDisplay(const QByteArray& s) {
    const auto base = QString::fromLatin1(s.left(8)).trimmed(), ext = QString::fromLatin1(s.mid(8, 3)).trimmed();
    return ext.isEmpty() ? base : base + '.' + ext;
}
inline uint8_t shortChecksum(const QByteArray& s) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; ++i) sum = uint8_t(((sum & 1) << 7) + (sum >> 1) + uint8_t(s[i]));
    return sum;
}
inline void putLe16(QByteArray& b, int at, uint32_t v) { b[at] = char(v); b[at + 1] = char(v >> 8); }
inline void putLe32(QByteArray& b, int at, uint32_t v) { putLe16(b, at, v); putLe16(b, at + 2, v >> 16); }
inline uint32_t le16(const QByteArray& b, int at) { return uint8_t(b[at]) | uint32_t(uint8_t(b[at + 1])) << 8; }
inline uint32_t le32(const QByteArray& b, int at) { return le16(b, at) | le16(b, at + 2) << 16; }
inline QPair<uint16_t, uint16_t> fatStamp(const QDateTime& when) {
    const auto local = when.isValid() ? when.toLocalTime() : QDateTime::currentDateTime();
    const auto d = local.date();
    const auto t = local.time();
    const auto date = uint16_t((std::clamp(d.year() - 1980, 0, 127) << 9) | (d.month() << 5) | d.day());
    const auto time = uint16_t((t.hour() << 11) | (t.minute() << 5) | (t.second() / 2));
    return {date, time};
}
// One directory entry with its long-name entries before it.
inline QByteArray dirEntry(const QString& name, const QByteArray& shortBytes, uint8_t attr, uint32_t cluster, uint32_t size,
                           const QDateTime& when) {
    const bool needsLong = shortDisplay(shortBytes) != name;
    const int longCount = needsLong ? int((name.size() + 12) / 13) : 0;
    QByteArray out((longCount + 1) * 32, '\0');
    const auto sum = shortChecksum(shortBytes);
    static const int offsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (int k = 0; k < longCount; ++k) {
        const int order = longCount - k, at = k * 32;
        out[at] = char(order | (k == 0 ? 0x40 : 0));
        out[at + 11] = 0x0f;
        out[at + 13] = char(sum);
        for (int j = 0; j < 13; ++j) {
            const int index = (order - 1) * 13 + j;
            const uint32_t c = index < name.size() ? name[index].unicode() : index == name.size() ? 0 : 0xffff;
            putLe16(out, at + offsets[j], c);
        }
    }
    const int at = longCount * 32;
    out.replace(at, 11, shortBytes);
    out[at + 11] = char(attr);
    const auto [date, time] = fatStamp(when);
    putLe16(out, at + 14, time); putLe16(out, at + 16, date); putLe16(out, at + 18, date);
    putLe16(out, at + 22, time); putLe16(out, at + 24, date);
    putLe16(out, at + 26, cluster); putLe32(out, at + 28, size);
    return out;
}


}  // namespace frondFolderCard
