// The desktop store cases of the fuzz regressions (QtCore): a stick header whose size overflows and
// JSON numbers too big for 64 bits in card.json and connections.json, apart from fuzz-regressions.cpp as the
// desktop headers use zlib and that file uses miniz.
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

#include "CardStore.h"
#include "LinkStore.h"

#define CHECK(x) \
    do { \
        if (!(x)) { std::fprintf(stderr, "fuzz regressions: FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } \
    } while (0)

void stickHeader() {
    // A classic stick header with 0xffffffff blocks of 0xffffffff pages: the byte size doesn't fit
    // in 63 bits, so it isn't a card file (the old code overflowed a signed multiply).
    QTemporaryDir dir;
    CHECK(dir.isValid());
    QByteArray header(64, '\0');
    header.replace(0, 8, QByteArray("FRONDMS\0", 8));
    for (int at = 12; at < 20; ++at) header[at] = char(0xff);
    QFile file(dir.path() + "/ms.frondms");
    CHECK(file.open(QIODevice::WriteOnly) && file.write(header) == 64);
    file.close();
    qint64 logical = 0;
    CHECK(!frondCards::cardHeader(file.fileName(), nullptr, &logical));
    // A real 64 MB stick still reads.
    header.fill('\0');
    header.replace(0, 8, QByteArray("FRONDMS\0", 8));
    header[12] = 64, header[17] = 8;  // 64 blocks of 2048 pages
    CHECK(file.open(QIODevice::WriteOnly) && file.write(header) == 64);
    file.close();
    CHECK(frondCards::cardHeader(file.fileName(), nullptr, &logical) && logical == 64ll * 2048 * 512);
}

static void put(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size());
}

void storeNumbers() {
    // A card.json size of 1e300 reads as 0 (the old cast to qint64 was undefined).
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString id = "0b6f2c1e-4a5d-4f3e-9c7b-1a2b3c4d5e6f", card = dir.path() + "/" + id;
    CHECK(QDir().mkpath(card));
    put(card + "/card.json", "{\"version\":1,\"id\":\"" + id.toLatin1() + "\",\"kind\":\"sd\",\"bytes\":1e300}");
    frondCards::Card read;
    CHECK(frondCards::readCard(card, &read) && read.bytes == 0);
    put(card + "/card.json", "{\"version\":1,\"id\":\"" + id.toLatin1() + "\",\"kind\":\"sd\",\"bytes\":67108864}");
    CHECK(frondCards::readCard(card, &read) && read.bytes == 67108864);

    // The same in connections.json: lastSeen and bluetoothRadioAt.
    const auto links = dir.path() + "/links";
    CHECK(QDir().mkpath(links));
    put(links + "/connections.json", "{\"version\":1,\"entries\":[{\"id\":\"ffeeddccbbaa99887766554433221100\","
        "\"key\":\"0202020202020202020202020202020202020202020202020202020202020202\",\"lastSeen\":-1e300,"
        "\"bluetoothRadioAt\":1e19}]}");
    frondLink::Store store;
    CHECK(store.open(links).isEmpty() && store.peers.size() == 1);
    CHECK(store.peers[0].lastSeen == 0 && store.peers[0].bluetoothRadioAt == 0);
}
