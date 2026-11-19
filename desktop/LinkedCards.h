#pragma once
// frond-68k's card replies and messages, for the Dana, the VZ Palms and the T415, whose cards are
// linked card files attached by path (sd-attach, sd2-attach, ms-attach).
#include <QEventLoop>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include "SessionStore.h"
#include "DesktopClient.h"

namespace frondDanaCards {
inline QString quote(QString path) { path.replace("\\","\\\\"); path.replace("\"","\\\""); return '"'+path+'"'; }
inline QList<int> sizes(const DeviceProfile5& profile, DeviceCardSlot5 slot, DeviceCardFamily5 family) {
    QList<int> result;
    for (int size : {32,64,128,256,512,1024,1886})
        if (deviceProfileCardSizeValid5(profile.deviceType,slot,family,quint64(size)<<20)) result.append(size);
    return result;
}
// Whether a .frondsd card's payload is a size this slot takes for the family.
inline bool fits(const QString& path, DeviceCardSlot5 slot, DeviceCardFamily5 family, int32_t deviceType=deviceTypeDana) {
    qint64 logical=0;
    if (!frondCards::cardHeader(path,nullptr,&logical) || logical<=0) return false;
    return deviceProfileCardSizeValid5(deviceType,slot,family,quint64(logical));
}
// Desktop text for a card command the core refused; other refusals pass through as they are.
inline QString message(const QString& core) {
    if (core.contains("duplicate identity or serial")) return QObject::tr("That card is already in the other slot.");
    if (core.contains("cannot lock card file")) return QObject::tr("That card is open somewhere else.");
    return core;
}
// Why a card can't be exported to path, empty when it can. The core only says the file couldn't be made.
inline QString exportProblem(const QString& path) {
    const QFileInfo file(path);
    if (file.exists()) return QObject::tr("Choose a new file. The core won't replace an existing card.");
    if (!QFileInfo(file.absolutePath()).isWritable()) return QObject::tr("Can't write to that folder.");
    return {};
}
struct State {
    bool known{}, present{}, inserted{}, protectedCard{}, failed{};
    QString identity, protocol;
};
inline State state(const QString& text) {
    if (text=="none") return {true};
    const auto fields=text.split(' ');
    if (fields.size()<3 || fields.size()>5 || (fields[0]!="inserted" && fields[0]!="ejected") ||
        !QRegularExpression("^[0-9a-f]{32}$").match(fields[1]).hasMatch() ||
        (fields[2]!="protected" && fields[2]!="writable")) return {};
    QSet<QString> flags;
    for (int i=3;i<fields.size();++i) {
        if ((fields[i]!="mmc" && fields[i]!="io-error") || flags.contains(fields[i])) return {};
        flags.insert(fields[i]);
    }
    return {true,true,fields[0]=="inserted",fields[2]=="protected",flags.contains("io-error"),fields[1],flags.contains("mmc")?"mmc":"sd"};
}

// Sends one command to the running core and waits for its reply, keeping the window alive. Used
// where a flow needs the answer before it goes on (Save a Copy's card check).
struct Reply { bool ok{}; QString message; };
inline Reply request(DesktopClient& client, const QString& command, const QString& argument = {}) {
    QEventLoop loop; QTimer poll, deadline; Reply out{false, QObject::tr("Couldn't reach the Palm.")}; bool sent = false;
    deadline.setSingleShot(true); deadline.start(120000);
    const auto done = QObject::connect(&client, &DesktopClient::result, &loop, [&](const QString& name, bool ok, const QString&, const QString& message) {
        if (sent && name == command) { out = {ok, message}; loop.quit(); }
    });
    QObject::connect(&client, &DesktopClient::finished, &loop, &QEventLoop::quit);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    auto send = [&] {
        if (!sent && client.available()) {
            client.setProperty("danaCardRequest", command);
            sent = client.request(command, argument);
        }
    };
    QObject::connect(&poll, &QTimer::timeout, &loop, send); poll.start(50); send(); loop.exec(); QObject::disconnect(done);
    client.setProperty("danaCardRequest", QVariant());
    return out;
}
}
