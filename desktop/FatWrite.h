#pragma once
// FAT directory entry pieces for the folder import (FolderCard.h): the storage library's
// (storage/FatNames.h) with Qt types. Header only.
#include "storage/FatNames.h"

#include <QByteArray>
#include <QDateTime>
#include <QPair>
#include <QSet>
#include <QString>
#include <cstdint>
#include <set>

namespace frondFolderCard {

// The same entries Android's FatImage writes: a Windows-style 8.3 name (upper case, anything
// past ASCII as _, a ~N tail when something was lost or taken) and long-name entries whenever
// that short name isn't the name exactly. No case bits and no code page bytes in short names,
// so every reader gets the long name as it was. mtools can't be told to do this: it keeps a
// lower-case or accented name short, in code page 850 with case bits.

inline QByteArray shortName(const QString& name, const QSet<QString>& taken) {
    std::set<std::string> names;
    for (const auto& t : taken) names.insert(t.toStdString());
    const auto s = frondStorage::fat::shortName(name.toStdString(), names);
    return QByteArray(s.data(), int(s.size()));
}
inline QString shortDisplay(const QByteArray& s) { return QString::fromStdString(frondStorage::fat::shortDisplay(s.toStdString())); }
inline uint8_t shortChecksum(const QByteArray& s) { return frondStorage::fat::shortChecksum(s.toStdString()); }
inline void putLe16(QByteArray& b, int at, uint32_t v) { b[at] = char(v); b[at + 1] = char(v >> 8); }
inline void putLe32(QByteArray& b, int at, uint32_t v) { putLe16(b, at, v); putLe16(b, at + 2, v >> 16); }
inline uint32_t le16(const QByteArray& b, int at) { return uint8_t(b[at]) | uint32_t(uint8_t(b[at + 1])) << 8; }
inline uint32_t le32(const QByteArray& b, int at) { return le16(b, at) | le16(b, at + 2) << 16; }
inline QPair<uint16_t, uint16_t> fatStamp(const QDateTime& when) {
    const auto local = when.isValid() ? when.toLocalTime() : QDateTime::currentDateTime();
    const auto d = local.date();
    const auto t = local.time();
    std::tm tm{};
    tm.tm_year = d.year() - 1900;
    tm.tm_mon = d.month() - 1;
    tm.tm_mday = d.day();
    tm.tm_hour = t.hour();
    tm.tm_min = t.minute();
    tm.tm_sec = t.second();
    const auto s = frondStorage::fat::stampOf(tm);
    return {s.date, s.time};
}
// One directory entry with its long-name entries before it.
inline QByteArray dirEntry(const QString& name, const QByteArray& shortBytes, uint8_t attr, uint32_t cluster, uint32_t size,
                           const QDateTime& when) {
    const auto [date, time] = fatStamp(when);
    const auto e = frondStorage::fat::dirEntry(name.toStdString(), shortBytes.toStdString(), attr, cluster, size, {date, time});
    return QByteArray(reinterpret_cast<const char*>(e.data()), int(e.size()));
}

}  // namespace frondFolderCard
