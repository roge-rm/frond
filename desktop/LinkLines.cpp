#include "LinkLines.h"
#include <QJsonDocument>

namespace frondLink {
using namespace frond::link;

const QByteArray kBeaconMagic("FRLB");

bool takeClientLine(QByteArray& input, QJsonObject& op) {
    const int at = input.indexOf('\n');
    if (at < 0) return false;
    const auto doc = QJsonDocument::fromJson(input.left(at));
    input.remove(0, at + 1);
    op = doc.isObject() ? doc.object() : QJsonObject();
    return true;
}

QPair<QString, quint16> splitAddress(QString address, quint16 port, quint16 fallback) {
    if (address.startsWith('[') && address.contains("]:")) {
        port = quint16(address.section("]:", 1).toUInt());
        address = address.section("]:", 0, 0).mid(1);
    } else if (address.count(':') == 1) {
        port = quint16(address.section(':', 1).toUInt());
        address = address.section(':', 0, 0);
    }
    return {address, port ? port : fallback};
}

std::optional<Beacon> parseBeacon(const QByteArray& data) {
    if (!data.startsWith(kBeaconMagic)) return std::nullopt;
    Reader r(reinterpret_cast<const uint8_t*>(data.constData()) + 4, size_t(data.size() - 4));
    Beacon b;
    const auto version = r.u8();
    b.type = r.u8();
    b.id = r.array<16>();
    b.port = r.u16();
    b.name = QString::fromStdString(r.str8()).left(64);
    if (!r.ok() || version != 1) return std::nullopt;
    return b;
}

}  // namespace frondLink
