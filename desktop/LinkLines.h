// The link hub's parsers, apart from the hub so they run without sockets or an event loop (and
// can be fuzzed): the JSON lines windows send, typed peer addresses and discovery beacons.
#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <optional>
#include <QPair>
#include "../link/bytes.h"

namespace frondLink {

// A window's unread input past this drops it.
constexpr int kClientInputMax = 1 << 20;

// Takes the next whole line off input. Returns false when no full line is left; else op is the
// line's object, or empty when the line isn't a JSON object.
bool takeClientLine(QByteArray& input, QJsonObject& op);

// host, host:port or [v6]:port, with port as the default. A port of 0 becomes fallback.
QPair<QString, quint16> splitAddress(QString address, quint16 port, quint16 fallback);

// Beacon: "FRLB", version 1, type (1 beacon, 2 query), install id, TCP port, name.
struct Beacon {
    quint8 type{};
    frond::link::InstallId id{};
    quint16 port{};
    QString name;  // at most 64 characters
};
extern const QByteArray kBeaconMagic;
std::optional<Beacon> parseBeacon(const QByteArray& datagram);

}  // namespace frondLink
