#pragma once
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include "device_profile5.h"

namespace frondDanaSession {
struct Header { bool wrapped{}, valid{}, portable{}; qint64 offset{}; quint32 version{}; QString device; };
inline quint32 u32(const QByteArray& bytes, int at) {
    quint32 result=0;
    for (int i=0;i<4;++i) result |= quint32(uchar(bytes[at+i])) << (8*i);
    return result;
}
inline quint32 hash(const QByteArray& bytes, quint32 value=2166136261u) {
    for (auto c : bytes) value=(value ^ uchar(c))*16777619u;
    return value;
}
// Display metadata only. Loading still goes through the core's media and history checks.
inline bool isDana(const QByteArray& name) { return name=="alphasmart-dana" || name=="alphasmart-dana-wireless"; }
inline Header inspect(const QString& path) {
    QFile file(path);
    Header out;
    if (!file.open(QIODevice::ReadOnly)) return out;
    const auto magic=file.read(13);
    if (magic!="FRONDLINKSD1\n" && magic!="FRONDPORTSD1\n") return out;
    out.wrapped=true; out.portable=magic=="FRONDPORTSD1\n";
    if (file.size()<64 || file.size()>(512ll<<20)) return out;
    const auto descriptor=file.read(12);
    if (descriptor.size()!=12) return out;
    const auto size=u32(descriptor,0);
    if (!size || size>8192 || file.size()<25+qint64(size)+40) return out;
    const auto manifest=file.read(size);
    if (manifest.size()!=size || hash(manifest)!=u32(descriptor,8) || manifest.contains('\0') || manifest.contains('\r')) return out;
    auto lines=QString::fromUtf8(manifest).split('\n');
    if (lines.last().isEmpty()) lines.removeLast();
    if (lines.size()<2 || lines.size()>3 || lines[0]!=(out.portable?"frond-portable-sd 3":"frond-linked-sd 3")) return out;
    QSet<QString> seenSlots, keys, serials;
    const QRegularExpression word(QStringLiteral("\"(?:[^\"\\\\]|\\\\.)*\"|[^\\s]+"));
    for (int i=1;i<lines.size();++i) {
        QStringList fields;
        auto match=word.globalMatch(lines[i]);
        while (match.hasNext()) fields.append(match.next().captured());
        if (fields.size()!=(out.portable?10:13)) return out;
        const auto id=fields[0]; const auto key=fields[3];
        if ((id!="0" && id!="2") || seenSlots.contains(id) || fields[1]!="sd" ||
            !QRegularExpression("^[0-9a-f]{32}$").match(key).hasMatch() || keys.contains(key) || serials.contains(key.left(8))) return out;
        seenSlots.insert(id); keys.insert(key); serials.insert(key.left(8));
        if (out.portable) { if (fields[2]!=(id=="0"?"sd0.frondsd":"sd2.frondsd")) return out; }
        else if (!fields[2].startsWith("\"/") || !fields[2].endsWith('"')) return out;
        bool ok=false;
        const auto serial=fields[4].toULongLong(&ok);
        if (!ok || serial!=key.left(8).toULongLong(nullptr,16) ||
            (fields[6]!="0" && fields[6]!="1") || (fields[7]!="0" && fields[7]!="1")) return out;
        const auto protocol=fields[out.portable?9:11];
        const auto bytes=fields[5].toULongLong(&ok);
        if (!ok || (protocol!="sd" && protocol!="mmc") ||
            !deviceProfileCardSizeValid5(deviceTypeDana,DeviceCardSlot5(id=="0"?0:2),
                protocol=="mmc"?DeviceCardFamily5::legacyMmc:DeviceCardFamily5::sdsc,bytes)) return out;
        for (int n : out.portable?QList<int>{8}:QList<int>{8,9,10,12}) {
            fields[n].toULongLong(&ok); if (!ok || fields[n].startsWith('-')) return out;
        }
    }
    out.offset=file.pos();
    quint32 checksum=2166136261u;
    while (!file.atEnd()) { const auto bytes=file.read(65536); if (bytes.isEmpty()) return out; checksum=hash(bytes,checksum); }
    if (checksum!=u32(descriptor,4) || !file.seek(out.offset)) return out;
    const auto machine=file.read(40);
    if (machine.size()!=40 || u32(machine,0)!=0x19800819u || u32(machine,4)<6 || u32(machine,4)>8 ||
        !isDana(machine.mid(8,32).split('\0').first())) return out;
    out.device=QString::fromLatin1(machine.mid(8,32).split('\0').first());
    out.version=u32(machine,4); out.valid=true;
    return out;
}
}
