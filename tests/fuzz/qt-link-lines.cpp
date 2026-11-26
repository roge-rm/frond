// The link hub's parsers (desktop/LinkLines.h): window input split into JSON lines as it arrives in
// pieces, typed peer addresses from those lines, and every line and the whole input as a discovery
// beacon. QtCore only.
#include "../../desktop/LinkLines.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    using namespace frondLink;
    if (size > 2 * kClientInputMax) return 0;
    const QByteArray all(reinterpret_cast<const char*>(bytes), int(size));
    parseBeacon(all);
    // Pieces of 1 to 64 bytes, the size taken from the byte before each piece.
    QByteArray input;
    for (int at = 0; at < all.size();) {
        const int piece = 1 + (uint8_t(all[at]) & 63);
        input += all.mid(at, piece);
        at += piece;
        if (input.size() > kClientInputMax) break;
        for (QJsonObject op; takeClientLine(input, op);) {
            if (op.isEmpty()) continue;
            if (op.contains("address")) {
                const auto target = splitAddress(op.value("address").toString().trimmed(),
                                                 quint16(op.value("port").toInt(5035)), 5035);
                if (!target.second) __builtin_trap();
            }
            parseBeacon(op.value("beacon").toString().toLatin1());
        }
    }
    return 0;
}
